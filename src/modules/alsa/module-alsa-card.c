/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>

#include "alsa-util.h"
#include "module-alsa-card-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("ALSA Card");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "name=<name for the sink/source> "
        "device_id=<ALSA card index> "
        "format=<sample format> "
        "rate=<sample rate> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "mmap=<enable memory mapping?> "
        "tsched=<enable system timer based scheduling mode?> "
        "tsched_buffer_size=<buffer size when using timer based scheduling> "
        "tsched_buffer_watermark=<lower fill watermark> "
        "profile=<profile name>");

static const char* const valid_modargs[] = {
    "sink_name",
    "device",
    "device_id",
    "format",
    "rate",
    "channels",
    "channel_map",
    "fragments",
    "fragment_size",
    "mmap",
    "tsched",
    "tsched_buffer_size",
    "tsched_buffer_watermark",
    NULL
};

#define DEFAULT_DEVICE_ID "0"

struct userdata {
    pa_core *core;
    pa_module *module;

    char *device_id;

    pa_card *card;
};

struct profile_data {
    const pa_alsa_profile_info *sink, *source;
};

static void enumerate_cb(
        const pa_alsa_profile_info *sink,
        const pa_alsa_profile_info *source,
        void *userdata) {

    pa_hashmap *profiles = (pa_hashmap *) userdata;
    char *t, *n;
    pa_card_profile *p;
    struct profile_data *d;

    if (sink && source) {
        n = pa_sprintf_malloc("%s+%s", sink->name, source->name);
        t = pa_sprintf_malloc("Output %s + Input %s", sink->description, source->description);
    } else if (sink) {
        n = pa_xstrdup(sink->name);
        t = pa_sprintf_malloc("Output %s", sink->description);
    } else {
        pa_assert(source);
        n = pa_xstrdup(source->name);
        t = pa_sprintf_malloc("Input %s", source->description);
    }

    pa_log_info("Found output profile '%s'", t);

    p = pa_card_profile_new(n, t, sizeof(struct profile_data));

    pa_xfree(t);
    pa_xfree(n);

    p->n_sinks = !!sink;
    p->n_sources = !!source;

    if (sink)
        p->max_sink_channels = sink->map.channels;
    if (source)
        p->max_source_channels = source->map.channels;

    d = PA_CARD_PROFILE_DATA(p);

    d->sink = sink;
    d->source = source;

    pa_hashmap_put(profiles, p->name, p);
}

int pa__init(pa_module*m) {
    pa_card_new_data data;
    pa_modargs *ma;
    int alsa_card_index;
    struct userdata *u;

    pa_alsa_redirect_errors_inc();

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->device_id = pa_xstrdup(pa_modargs_get_value(ma, "device_id", DEFAULT_DEVICE_ID));

    if ((alsa_card_index = snd_card_get_index(u->device_id)) < 0) {
        pa_log("Card '%s' doesn't exist: %s", u->device_id, snd_strerror(alsa_card_index));
        goto fail;
    }

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_alsa_init_proplist_card(data.proplist, alsa_card_index);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->device_id);
    pa_card_new_data_set_name(&data, pa_modargs_get_value(ma, "name", u->device_id));

    data.profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    if (pa_alsa_probe_profiles(u->device_id, &m->core->default_sample_spec, enumerate_cb, data.profiles) < 0) {
        pa_card_new_data_done(&data);
        goto fail;
    }

    u->card = pa_card_new(m->core, &data);
    pa_card_new_data_done(&data);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        goto finish;

    if (u->card)
        pa_card_free(u->card);

    pa_xfree(u->device_id);
    pa_xfree(u);

finish:
    snd_config_update_free_global();
    pa_alsa_redirect_errors_dec();
}
