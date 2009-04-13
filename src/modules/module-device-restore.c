/***
  This file is part of PulseAudio.

  Copyright 2006-2008 Lennart Poettering

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

#include "module-device-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore the volume/mute state of devices");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "restore_volume=<Save/restore volumes?> "
        "restore_muted=<Save/restore muted states?>");

#define SAVE_INTERVAL 10

static const char* const valid_modargs[] = {
    "restore_volume",
    "restore_muted",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_subscription *subscription;
    pa_hook_slot
        *sink_fixate_hook_slot,
        *source_fixate_hook_slot;
    pa_time_event *save_time_event;
    GDBM_FILE gdbm_file;

    pa_bool_t restore_volume:1;
    pa_bool_t restore_muted:1;
};

#define ENTRY_VERSION 1

struct entry {
    uint8_t version;
    pa_bool_t muted:1;
    pa_channel_map channel_map;
    pa_cvolume volume;
} PA_GCC_PACKED;

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

static struct entry* read_entry(struct userdata *u, const char *name) {
    datum key, data;
    struct entry *e;

    pa_assert(u);
    pa_assert(name);

    key.dptr = (char*) name;
    key.dsize = (int) strlen(name);

    data = gdbm_fetch(u->gdbm_file, key);

    if (!data.dptr)
        goto fail;

    if (data.dsize != sizeof(struct entry)) {
        pa_log_debug("Database contains entry for device %s of wrong size %lu != %lu. Probably due to upgrade, ignoring.", name, (unsigned long) data.dsize, (unsigned long) sizeof(struct entry));
        goto fail;
    }

    e = (struct entry*) data.dptr;

    if (e->version != ENTRY_VERSION) {
        pa_log_debug("Version of database entry for device %s doesn't match our version. Probably due to upgrade, ignoring.", name);
        goto fail;
    }

    if (!(pa_cvolume_valid(&e->volume))) {
        pa_log_warn("Invalid volume stored in database for device %s", name);
        goto fail;
    }

    if (!(pa_channel_map_valid(&e->channel_map))) {
        pa_log_warn("Invalid channel map stored in database for device %s", name);
        goto fail;
    }

    if (e->volume.channels != e->channel_map.channels) {
        pa_log_warn("Volume and channel map don't match in database entry for device %s", name);
        goto fail;
    }

    return e;

fail:

    pa_xfree(data.dptr);
    return NULL;
}

static void trigger_save(struct userdata *u) {
    struct timeval tv;

    if (u->save_time_event)
        return;

    pa_gettimeofday(&tv);
    tv.tv_sec += SAVE_INTERVAL;
    u->save_time_event = u->core->mainloop->time_new(u->core->mainloop, &tv, save_time_callback, u);
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry entry, *old;
    char *name;
    datum key, data;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    memset(&entry, 0, sizeof(entry));
    entry.version = ENTRY_VERSION;

    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK) {
        pa_sink *sink;

        if (!(sink = pa_idxset_get_by_index(c->sinks, idx)))
            return;

        name = pa_sprintf_malloc("sink:%s", sink->name);
        entry.channel_map = sink->channel_map;
        entry.volume = *pa_sink_get_volume(sink, FALSE, TRUE);
        entry.muted = pa_sink_get_mute(sink, FALSE);

    } else {
        pa_source *source;

        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE);

        if (!(source = pa_idxset_get_by_index(c->sources, idx)))
            return;

        name = pa_sprintf_malloc("source:%s", source->name);
        entry.channel_map = source->channel_map;
        entry.volume = *pa_source_get_volume(source, FALSE);
        entry.muted = pa_source_get_mute(source, FALSE);
    }

    if ((old = read_entry(u, name))) {

        if (pa_cvolume_equal(pa_cvolume_remap(&old->volume, &old->channel_map, &entry.channel_map), &entry.volume) &&
            !old->muted == !entry.muted) {

            pa_xfree(old);
            pa_xfree(name);
            return;
        }

        pa_xfree(old);
    }

    key.dptr = name;
    key.dsize = (int) strlen(name);

    data.dptr = (void*) &entry;
    data.dsize = sizeof(entry);

    pa_log_info("Storing volume/mute for device %s.", name);

    gdbm_store(u->gdbm_file, key, data, GDBM_REPLACE);

    pa_xfree(name);

    trigger_save(u);
}

static pa_hook_result_t sink_fixate_hook_callback(pa_core *c, pa_sink_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(new_data);

    name = pa_sprintf_malloc("sink:%s", new_data->name);

    if ((e = read_entry(u, name))) {

        if (u->restore_volume) {

            if (!new_data->volume_is_set) {
                pa_log_info("Restoring volume for sink %s.", new_data->name);
                pa_sink_new_data_set_volume(new_data, pa_cvolume_remap(&e->volume, &e->channel_map, &new_data->channel_map));
            } else
                pa_log_debug("Not restoring volume for sink %s, because already set.", new_data->name);

        }

        if (u->restore_muted) {

            if (!new_data->muted_is_set) {
                pa_log_info("Restoring mute state for sink %s.", new_data->name);
                pa_sink_new_data_set_muted(new_data, e->muted);
            } else
                pa_log_debug("Not restoring mute state for sink %s, because already set.", new_data->name);
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_fixate_hook_callback(pa_core *c, pa_source_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(new_data);

    name = pa_sprintf_malloc("source:%s", new_data->name);

    if ((e = read_entry(u, name))) {

        if (u->restore_volume) {

            if (!new_data->volume_is_set) {
                pa_log_info("Restoring volume for source %s.", new_data->name);
                pa_source_new_data_set_volume(new_data, pa_cvolume_remap(&e->volume, &e->channel_map, &new_data->channel_map));
            } else
                pa_log_debug("Not restoring volume for source %s, because already set.", new_data->name);
        }

        if (u->restore_muted) {

            if (!new_data->muted_is_set) {
                pa_log_info("Restoring mute state for source %s.", new_data->name);
                pa_source_new_data_set_muted(new_data, e->muted);
            } else
                pa_log_debug("Not restoring mute state for source %s, because already set.", new_data->name);
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
    pa_sink *sink;
    pa_source *source;
    uint32_t idx;
    pa_bool_t restore_volume = TRUE, restore_muted = TRUE;
    int gdbm_cache_size;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "restore_volume", &restore_volume) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_muted", &restore_muted) < 0) {
        pa_log("restore_volume= and restore_muted= expect boolean arguments");
        goto fail;
    }

    if (!restore_muted && !restore_volume)
        pa_log_warn("Neither restoring volume nor restoring muted enabled!");

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->save_time_event = NULL;
    u->restore_volume = restore_volume;
    u->restore_muted = restore_muted;
    u->gdbm_file = NULL;

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SOURCE, subscribe_callback, u);

    if (restore_muted || restore_volume) {
        u->sink_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) sink_fixate_hook_callback, u);
        u->source_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) source_fixate_hook_callback, u);
    }

    /* We include the host identifier in the file name because gdbm
     * files are CPU dependant, and we don't want things to go wrong
     * if we are on a multiarch system. */

    fn = pa_sprintf_malloc("device-volumes."CANONICAL_HOST".gdbm");
    fname = pa_state_path(fn, TRUE);
    pa_xfree(fn);

    if (!fname)
        goto fail;

    if (!(u->gdbm_file = gdbm_open(fname, 0, GDBM_WRCREAT|GDBM_NOLOCK, 0600, NULL))) {
        pa_log("Failed to open volume database '%s': %s", fname, gdbm_strerror(gdbm_errno));
        pa_xfree(fname);
        goto fail;
    }

    /* By default the cache of gdbm is rather large, let's reduce it a bit to save memory */
    gdbm_cache_size = 10;
    gdbm_setopt(u->gdbm_file, GDBM_CACHESIZE, &gdbm_cache_size, sizeof(gdbm_cache_size));

    pa_log_info("Sucessfully opened database file '%s'.", fname);
    pa_xfree(fname);

    for (sink = pa_idxset_first(m->core->sinks, &idx); sink; sink = pa_idxset_next(m->core->sinks, &idx))
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_NEW, sink->index, u);

    for (source = pa_idxset_first(m->core->sources, &idx); source; source = pa_idxset_next(m->core->sources, &idx))
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_NEW, source->index, u);

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

    if (u->sink_fixate_hook_slot)
        pa_hook_slot_free(u->sink_fixate_hook_slot);
    if (u->source_fixate_hook_slot)
        pa_hook_slot_free(u->source_fixate_hook_slot);

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    if (u->gdbm_file)
        gdbm_close(u->gdbm_file);

    pa_xfree(u);
}
