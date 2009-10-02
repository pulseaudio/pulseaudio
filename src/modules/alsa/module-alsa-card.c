/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/i18n.h>

#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/queue.h>

#include <modules/reserve-wrap.h>

#ifdef HAVE_UDEV
#include <modules/udev-util.h>
#endif

#include "alsa-util.h"
#include "alsa-sink.h"
#include "alsa-source.h"
#include "module-alsa-card-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("ALSA Card");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "name=<name for the card/sink/source, to be prefixed> "
        "card_name=<name for the card> "
        "card_properties=<properties for the card> "
        "sink_name=<name for the sink> "
        "sink_properties=<properties for the sink> "
        "source_name=<name for the source> "
        "source_properties=<properties for the source> "
        "device_id=<ALSA card index> "
        "format=<sample format> "
        "rate=<sample rate> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "mmap=<enable memory mapping?> "
        "tsched=<enable system timer based scheduling mode?> "
        "tsched_buffer_size=<buffer size when using timer based scheduling> "
        "tsched_buffer_watermark=<lower fill watermark> "
        "profile=<profile name> "
        "ignore_dB=<ignore dB information from the device?>");

static const char* const valid_modargs[] = {
    "name",
    "card_name",
    "card_properties",
    "sink_name",
    "sink_properties",
    "source_name",
    "source_properties",
    "device_id",
    "format",
    "rate",
    "fragments",
    "fragment_size",
    "mmap",
    "tsched",
    "tsched_buffer_size",
    "tsched_buffer_watermark",
    "profile",
    "ignore_dB",
    NULL
};

#define DEFAULT_DEVICE_ID "0"

struct userdata {
    pa_core *core;
    pa_module *module;

    char *device_id;

    pa_card *card;

    pa_modargs *modargs;

    pa_alsa_profile_set *profile_set;
};

struct profile_data {
    pa_alsa_profile *profile;
};

static void add_profiles(struct userdata *u, pa_hashmap *h) {
    pa_alsa_profile *ap;
    void *state;

    pa_assert(u);
    pa_assert(h);

    PA_HASHMAP_FOREACH(ap, u->profile_set->profiles, state) {
        struct profile_data *d;
        pa_card_profile *cp;
        pa_alsa_mapping *m;
        uint32_t idx;

        cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
        cp->priority = ap->priority;

        if (ap->output_mappings) {
            cp->n_sinks = pa_idxset_size(ap->output_mappings);

            PA_IDXSET_FOREACH(m, ap->output_mappings, idx)
                if (m->channel_map.channels > cp->max_sink_channels)
                    cp->max_sink_channels = m->channel_map.channels;
        }

        if (ap->input_mappings) {
            cp->n_sources = pa_idxset_size(ap->input_mappings);

            PA_IDXSET_FOREACH(m, ap->input_mappings, idx)
                if (m->channel_map.channels > cp->max_source_channels)
                    cp->max_source_channels = m->channel_map.channels;
        }

        d = PA_CARD_PROFILE_DATA(cp);
        d->profile = ap;

        pa_hashmap_put(h, cp->name, cp);
    }
}

static void add_disabled_profile(pa_hashmap *profiles) {
    pa_card_profile *p;
    struct profile_data *d;

    p = pa_card_profile_new("off", _("Off"), sizeof(struct profile_data));

    d = PA_CARD_PROFILE_DATA(p);
    d->profile = NULL;

    pa_hashmap_put(profiles, p->name, p);
}

static int card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    struct userdata *u;
    struct profile_data *nd, *od;
    uint32_t idx;
    pa_alsa_mapping *am;
    pa_queue *sink_inputs = NULL, *source_outputs = NULL;

    pa_assert(c);
    pa_assert(new_profile);
    pa_assert_se(u = c->userdata);

    nd = PA_CARD_PROFILE_DATA(new_profile);
    od = PA_CARD_PROFILE_DATA(c->active_profile);

    if (od->profile && od->profile->output_mappings)
        PA_IDXSET_FOREACH(am, od->profile->output_mappings, idx) {
            if (!am->sink)
                continue;

            if (nd->profile &&
                nd->profile->output_mappings &&
                pa_idxset_get_by_data(nd->profile->output_mappings, am, NULL))
                continue;

            sink_inputs = pa_sink_move_all_start(am->sink, sink_inputs);
            pa_alsa_sink_free(am->sink);
            am->sink = NULL;
        }

    if (od->profile && od->profile->input_mappings)
        PA_IDXSET_FOREACH(am, od->profile->input_mappings, idx) {
            if (!am->source)
                continue;

            if (nd->profile &&
                nd->profile->input_mappings &&
                pa_idxset_get_by_data(nd->profile->input_mappings, am, NULL))
                continue;

            source_outputs = pa_source_move_all_start(am->source, source_outputs);
            pa_alsa_source_free(am->source);
            am->source = NULL;
        }

    if (nd->profile && nd->profile->output_mappings)
        PA_IDXSET_FOREACH(am, nd->profile->output_mappings, idx) {

            if (!am->sink)
                am->sink = pa_alsa_sink_new(c->module, u->modargs, __FILE__, c, am);

            if (sink_inputs && am->sink) {
                pa_sink_move_all_finish(am->sink, sink_inputs, FALSE);
                sink_inputs = NULL;
            }
        }

    if (nd->profile && nd->profile->input_mappings)
        PA_IDXSET_FOREACH(am, nd->profile->input_mappings, idx) {

            if (!am->source)
                am->source = pa_alsa_source_new(c->module, u->modargs, __FILE__, c, am);

            if (source_outputs && am->source) {
                pa_source_move_all_finish(am->source, source_outputs, FALSE);
                source_outputs = NULL;
            }
        }

    if (sink_inputs)
        pa_sink_move_all_fail(sink_inputs);

    if (source_outputs)
        pa_source_move_all_fail(source_outputs);

    return 0;
}

static void init_profile(struct userdata *u) {
    uint32_t idx;
    pa_alsa_mapping *am;
    struct profile_data *d;

    pa_assert(u);

    d = PA_CARD_PROFILE_DATA(u->card->active_profile);

    if (d->profile && d->profile->output_mappings)
        PA_IDXSET_FOREACH(am, d->profile->output_mappings, idx)
            am->sink = pa_alsa_sink_new(u->module, u->modargs, __FILE__, u->card, am);

    if (d->profile && d->profile->input_mappings)
        PA_IDXSET_FOREACH(am, d->profile->input_mappings, idx)
            am->source = pa_alsa_source_new(u->module, u->modargs, __FILE__, u->card, am);
}

static void set_card_name(pa_card_new_data *data, pa_modargs *ma, const char *device_id) {
    char *t;
    const char *n;

    pa_assert(data);
    pa_assert(ma);
    pa_assert(device_id);

    if ((n = pa_modargs_get_value(ma, "card_name", NULL))) {
        pa_card_new_data_set_name(data, n);
        data->namereg_fail = TRUE;
        return;
    }

    if ((n = pa_modargs_get_value(ma, "name", NULL)))
        data->namereg_fail = TRUE;
    else {
        n = device_id;
        data->namereg_fail = FALSE;
    }

    t = pa_sprintf_malloc("alsa_card.%s", n);
    pa_card_new_data_set_name(data, t);
    pa_xfree(t);
}

int pa__init(pa_module *m) {
    pa_card_new_data data;
    pa_modargs *ma;
    int alsa_card_index;
    struct userdata *u;
    pa_reserve_wrapper *reserve = NULL;
    const char *description;
    char *fn = NULL;

    pa_alsa_refcnt_inc();

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->device_id = pa_xstrdup(pa_modargs_get_value(ma, "device_id", DEFAULT_DEVICE_ID));
    u->modargs = ma;

    if ((alsa_card_index = snd_card_get_index(u->device_id)) < 0) {
        pa_log("Card '%s' doesn't exist: %s", u->device_id, pa_alsa_strerror(alsa_card_index));
        goto fail;
    }

    if (!pa_in_system_mode()) {
        char *rname;

        if ((rname = pa_alsa_get_reserve_name(u->device_id))) {
            reserve = pa_reserve_wrapper_get(m->core, rname);
            pa_xfree(rname);

            if (!reserve)
                goto fail;
        }
    }

#ifdef HAVE_UDEV
    fn = pa_udev_get_property(alsa_card_index, "PULSE_PROFILE_SET");
#endif

    u->profile_set = pa_alsa_profile_set_new(fn, &u->core->default_channel_map);
    pa_xfree(fn);

    if (!u->profile_set)
        goto fail;

    pa_alsa_profile_set_probe(u->profile_set, u->device_id, &m->core->default_sample_spec, m->core->default_n_fragments, m->core->default_fragment_size_msec);

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;

    pa_alsa_init_proplist_card(m->core, data.proplist, alsa_card_index);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->device_id);
    pa_alsa_init_description(data.proplist);
    set_card_name(&data, ma, u->device_id);

    if (reserve)
        if ((description = pa_proplist_gets(data.proplist, PA_PROP_DEVICE_DESCRIPTION)))
            pa_reserve_wrapper_set_application_device_name(reserve, description);

    data.profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    add_profiles(u, data.profiles);

    if (pa_hashmap_isempty(data.profiles)) {
        pa_log("Failed to find a working profile.");
        pa_card_new_data_done(&data);
        goto fail;
    }

    add_disabled_profile(data.profiles);

    if (pa_modargs_get_proplist(ma, "card_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_card_new_data_done(&data);
        goto fail;
    }

    u->card = pa_card_new(m->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card)
        goto fail;

    u->card->userdata = u;
    u->card->set_profile = card_set_profile;

    init_profile(u);

    if (reserve)
        pa_reserve_wrapper_unref(reserve);

    return 0;

fail:
    if (reserve)
        pa_reserve_wrapper_unref(reserve);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;
    int n = 0;
    uint32_t idx;
    pa_sink *sink;
    pa_source *source;

    pa_assert(m);
    pa_assert_se(u = m->userdata);
    pa_assert(u->card);

    PA_IDXSET_FOREACH(sink, u->card->sinks, idx)
        n += pa_sink_linked_by(sink);

    PA_IDXSET_FOREACH(source, u->card->sources, idx)
        n += pa_source_linked_by(source);

    return n;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        goto finish;

    if (u->card && u->card->sinks) {
        pa_sink *s;

        while ((s = pa_idxset_steal_first(u->card->sinks, NULL)))
            pa_alsa_sink_free(s);
    }

    if (u->card && u->card->sources) {
        pa_source *s;

        while ((s = pa_idxset_steal_first(u->card->sources, NULL)))
            pa_alsa_source_free(s);
    }

    if (u->card)
        pa_card_free(u->card);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    if (u->profile_set)
        pa_alsa_profile_set_free(u->profile_set);

    pa_xfree(u->device_id);
    pa_xfree(u);

finish:
    pa_alsa_refcnt_dec();
}
