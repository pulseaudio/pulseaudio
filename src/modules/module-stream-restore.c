/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering

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

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <gdbm.h>

#include <pulse/xmalloc.h>
#include <pulse/volume.h>
#include <pulse/timeval.h>
#include <pulse/util.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/namereg.h>

#include "module-stream-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore the volume/mute/device state of streams");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

#define SAVE_INTERVAL 10

static const char* const valid_modargs[] = {
    "restore_device",
    "restore_volume",
    "restore_muted",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_subscription *subscription;
    pa_hook_slot
        *sink_input_new_hook_slot,
        *sink_input_fixate_hook_slot,
        *source_output_new_hook_slot;
    pa_time_event *save_time_event;
    GDBM_FILE gdbm_file;

    pa_bool_t restore_device:1;
    pa_bool_t restore_volume:1;
    pa_bool_t restore_muted:1;
};

struct entry {
    char device[PA_NAME_MAX];
    pa_channel_map channel_map;
    pa_cvolume volume;
    pa_bool_t muted:1;
};

static void save_time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(tv);
    pa_assert(u);

    pa_assert(e == u->save_time_event);
    u->core->mainloop->time_free(u->save_time_event);
    u->save_time_event = NULL;

    gdbm_sync(u->gdbm_file);
    pa_log_info("Synced.");
}

static char *get_name(pa_proplist *p, const char *prefix) {
    const char *r;

    if (!p)
        return NULL;

    if ((r = pa_proplist_gets(p, PA_PROP_MEDIA_ROLE)))
        return pa_sprintf_malloc("%s-by-media-role:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_APPLICATION_ID)))
        return pa_sprintf_malloc("%s-by-application-id:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_APPLICATION_NAME)))
        return pa_sprintf_malloc("%s-by-application-name:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_MEDIA_NAME)))
        return pa_sprintf_malloc("%s-by-media-name:%s", prefix, r);

    return NULL;
}

static struct entry* read_entry(struct userdata *u, char *name) {
    datum key, data;
    struct entry *e;

    pa_assert(u);
    pa_assert(name);

    key.dptr = name;
    key.dsize = strlen(name);

    data = gdbm_fetch(u->gdbm_file, key);

    if (!data.dptr)
        goto fail;

    if (data.dsize != sizeof(struct entry)) {
        pa_log_warn("Database contains entry for stream %s of wrong size %lu != %lu", name, (unsigned long) data.dsize, (unsigned long) sizeof(struct entry));
        goto fail;
    }

    e = (struct entry*) data.dptr;

    if (!memchr(e->device, 0, sizeof(e->device))) {
        pa_log_warn("Database contains entry for stream %s with missing NUL byte in device name", name);
        goto fail;
    }

    if (!(pa_cvolume_valid(&e->volume))) {
        pa_log_warn("Invalid volume stored in database for stream %s", name);
        goto fail;
    }

    if (!(pa_channel_map_valid(&e->channel_map))) {
        pa_log_warn("Invalid channel map stored in database for stream %s", name);
        goto fail;
    }

    if (e->volume.channels != e->channel_map.channels) {
        pa_log_warn("Volume and channel map don't match in database entry for stream %s", name);
        goto fail;
    }

    return e;

fail:

    pa_xfree(data.dptr);
    return NULL;
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry entry, *old;
    char *name;
    datum key, data;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        pa_sink_input *sink_input;

        if (!(sink_input = pa_idxset_get_by_index(c->sink_inputs, idx)))
            return;

        if (!(name = get_name(sink_input->proplist, "sink-input")))
            return;

        entry.channel_map = sink_input->channel_map;
        entry.volume = *pa_sink_input_get_volume(sink_input);
        entry.muted = pa_sink_input_get_mute(sink_input);
        pa_strlcpy(entry.device, sink_input->sink->name, sizeof(entry.device));

    } else {
        pa_source_output *source_output;

        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT);

        if (!(source_output = pa_idxset_get_by_index(c->source_outputs, idx)))
            return;

        if (!(name = get_name(source_output->proplist, "source-output")))
            return;

        /* The following fields are filled in to make the entry valid
         * according to read_entry(). They are otherwise useless */
        entry.channel_map = source_output->channel_map;
        pa_cvolume_reset(&entry.volume, entry.channel_map.channels);
        pa_strlcpy(entry.device, source_output->source->name, sizeof(entry.device));
    }

    if ((old = read_entry(u, name))) {

        if (pa_cvolume_equal(pa_cvolume_remap(&old->volume, &old->channel_map, &entry.channel_map), &entry.volume) &&
            !old->muted == !entry.muted &&
            strcmp(old->device, entry.device) == 0) {

            pa_xfree(old);
            pa_xfree(name);
            return;
        }

        pa_xfree(old);
    }

    key.dptr = name;
    key.dsize = strlen(name);

    data.dptr = (void*) &entry;
    data.dsize = sizeof(entry);

    pa_log_info("Storing volume/mute/device for stream %s.", name);

    gdbm_store(u->gdbm_file, key, data, GDBM_REPLACE);

    if (!u->save_time_event) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec += SAVE_INTERVAL;
        u->save_time_event = u->core->mainloop->time_new(u->core->mainloop, &tv, save_time_callback, u);
    }

    pa_xfree(name);
}

static pa_hook_result_t sink_input_new_hook_callback(pa_core *c, pa_sink_input_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(new_data);

    if (!(name = get_name(new_data->proplist, "sink-input")))
        return PA_HOOK_OK;

    if ((e = read_entry(u, name))) {
        pa_sink *s;

        if (u->restore_device &&
            e->device[0] &&
            (s = pa_namereg_get(c, e->device, PA_NAMEREG_SINK, TRUE))) {

            pa_log_info("Restoring device for stream %s.", name);
            new_data->sink = s;
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_fixate_hook_callback(pa_core *c, pa_sink_input_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(new_data);

    if (!(name = get_name(new_data->proplist, "sink-input")))
        return PA_HOOK_OK;

    if ((e = read_entry(u, name))) {

        if (u->restore_volume) {
            pa_log_info("Restoring volume for sink input %s.", name);
            pa_sink_input_new_data_set_volume(new_data, pa_cvolume_remap(&e->volume, &e->channel_map, &new_data->channel_map));
        }

        if (u->restore_muted) {
            pa_log_info("Restoring mute state for sink input %s.", name);
            pa_sink_input_new_data_set_muted(new_data, e->muted);
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_new_hook_callback(pa_core *c, pa_source_output_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(new_data);

    if (!(name = get_name(new_data->proplist, "source-output")))
        return PA_HOOK_OK;

    if ((e = read_entry(u, name))) {
        pa_source *s;

        if (u->restore_device &&
            e->device[0] &&
            (s = pa_namereg_get(c, e->device, PA_NAMEREG_SOURCE, TRUE))) {

            pa_log_info("Restoring device for stream %s.", name);
            new_data->source = s;
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    char *fname, *fn;
    char hn[256];
    pa_sink_input *si;
    pa_source_output *so;
    uint32_t idx;
    pa_bool_t restore_device = TRUE, restore_volume = TRUE, restore_muted = TRUE;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "restore_device", &restore_device) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_volume", &restore_volume) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_muted", &restore_muted) < 0) {
        pa_log("restore_device=, restore_volume= and restore_muted= expect boolean arguments");
        goto fail;
    }

    if (!restore_muted && !restore_volume && !restore_device)
        pa_log_warn("Neither restoring volume, nor restoring muted, nor restoring device enabled!");

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->save_time_event = NULL;
    u->restore_device = restore_device;
    u->restore_volume = restore_volume;
    u->restore_muted = restore_muted;

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK_INPUT|PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, subscribe_callback, u);

    if (restore_device) {
        u->sink_input_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_new_hook_callback, u);
        u->source_output_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) source_output_new_hook_callback, u);
    }

    if (restore_volume || restore_muted)
        u->sink_input_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_fixate_hook_callback, u);

    if (!pa_get_host_name(hn, sizeof(hn)))
        goto fail;

    fn = pa_sprintf_malloc("stream-volumes.%s."CANONICAL_HOST".gdbm", hn);
    fname = pa_state_path(fn);
    pa_xfree(fn);

    if (!fname)
        goto fail;

    if (!(u->gdbm_file = gdbm_open(fname, 0, GDBM_WRCREAT, 0600, NULL))) {
        pa_log("Failed to open volume database '%s': %s", fname, gdbm_strerror(gdbm_errno));
        pa_xfree(fname);
        goto fail;
    }

    pa_log_info("Sucessfully opened database file '%s'.", fname);
    pa_xfree(fname);

    for (si = pa_idxset_first(m->core->sink_inputs, &idx); si; si = pa_idxset_next(m->core->sink_inputs, &idx))
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, si->index, u);

    for (so = pa_idxset_first(m->core->source_outputs, &idx); so; so = pa_idxset_next(m->core->source_outputs, &idx))
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW, so->index, u);

    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return  -1;
}

void pa__done(pa_module*m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
        pa_subscription_free(u->subscription);

    if (u->sink_input_new_hook_slot)
        pa_hook_slot_free(u->sink_input_new_hook_slot);
    if (u->sink_input_fixate_hook_slot)
        pa_hook_slot_free(u->sink_input_fixate_hook_slot);
    if (u->source_output_new_hook_slot)
        pa_hook_slot_free(u->source_output_new_hook_slot);

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    if (u->gdbm_file)
        gdbm_close(u->gdbm_file);

    pa_xfree(u);
}
