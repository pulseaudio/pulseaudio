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

#include <pulse/xmalloc.h>
#include <pulse/volume.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/rtclock.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/namereg.h>
#include <pulsecore/database.h>
#include <pulsecore/tagstruct.h>

#include "module-device-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore the volume/mute state of devices");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "restore_port=<Save/restore port?> "
        "restore_volume=<Save/restore volumes?> "
        "restore_muted=<Save/restore muted states?>");

#define SAVE_INTERVAL (10 * PA_USEC_PER_SEC)

static const char* const valid_modargs[] = {
    "restore_volume",
    "restore_muted",
    "restore_port",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_subscription *subscription;
    pa_hook_slot
        *sink_new_hook_slot,
        *sink_fixate_hook_slot,
        *source_new_hook_slot,
        *source_fixate_hook_slot;
    pa_time_event *save_time_event;
    pa_database *database;

    pa_bool_t restore_volume:1;
    pa_bool_t restore_muted:1;
    pa_bool_t restore_port:1;
};

#define ENTRY_VERSION 3

struct entry {
    uint8_t version;
    pa_bool_t muted_valid, volume_valid, port_valid;
    pa_bool_t muted;
    pa_channel_map channel_map;
    pa_cvolume volume;
    char *port;
};

static void save_time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *t, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(u);

    pa_assert(e == u->save_time_event);
    u->core->mainloop->time_free(u->save_time_event);
    u->save_time_event = NULL;

    pa_database_sync(u->database);
    pa_log_info("Synced.");
}

static struct entry* entry_new(void) {
    struct entry *r = pa_xnew0(struct entry, 1);
    r->version = ENTRY_VERSION;
    return r;
}

static void entry_free(struct entry* e) {
    pa_assert(e);

    pa_xfree(e->port);
    pa_xfree(e);
}

static struct entry* entry_read(struct userdata *u, const char *name) {
    pa_datum key, data;
    struct entry *e = NULL;
    pa_tagstruct *t = NULL;
    const char* port;

    pa_assert(u);
    pa_assert(name);

    key.data = (char*) name;
    key.size = strlen(name);

    pa_zero(data);

    if (!pa_database_get(u->database, &key, &data))
        goto fail;

    t = pa_tagstruct_new(data.data, data.size);
    e = entry_new();

    if (pa_tagstruct_getu8(t, &e->version) < 0 ||
        e->version > ENTRY_VERSION ||
        pa_tagstruct_get_boolean(t, &e->volume_valid) < 0 ||
        pa_tagstruct_get_channel_map(t, &e->channel_map) < 0 ||
        pa_tagstruct_get_cvolume(t, &e->volume) < 0 ||
        pa_tagstruct_get_boolean(t, &e->muted_valid) < 0 ||
        pa_tagstruct_get_boolean(t, &e->muted) < 0 ||
        pa_tagstruct_get_boolean(t, &e->port_valid) < 0 ||
        pa_tagstruct_gets(t, &port) < 0) {

        goto fail;
    }

    e->port = pa_xstrdup(port);

    if (!pa_tagstruct_eof(t))
        goto fail;

    if (e->volume_valid && !pa_channel_map_valid(&e->channel_map)) {
        pa_log_warn("Invalid channel map stored in database for device %s", name);
        goto fail;
    }

    if (e->volume_valid && (!pa_cvolume_valid(&e->volume) || !pa_cvolume_compatible_with_channel_map(&e->volume, &e->channel_map))) {
        pa_log_warn("Volume and channel map don't match in database entry for device %s", name);
        goto fail;
    }

    pa_tagstruct_free(t);
    pa_datum_free(&data);

    return e;

fail:

    pa_log_debug("Database contains invalid data for key: %s (probably pre-v1.0 data)", name);

    if (e)
        entry_free(e);
    if (t)
        pa_tagstruct_free(t);
    pa_datum_free(&data);
    return NULL;
}

static pa_bool_t entry_write(struct userdata *u, const char *name, const struct entry *e) {
    pa_tagstruct *t;
    pa_datum key, data;
    pa_bool_t r;

    pa_assert(u);
    pa_assert(name);
    pa_assert(e);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu8(t, e->version);
    pa_tagstruct_put_boolean(t, e->volume_valid);
    pa_tagstruct_put_channel_map(t, &e->channel_map);
    pa_tagstruct_put_cvolume(t, &e->volume);
    pa_tagstruct_put_boolean(t, e->muted_valid);
    pa_tagstruct_put_boolean(t, e->muted);
    pa_tagstruct_put_boolean(t, e->port_valid);
    pa_tagstruct_puts(t, e->port);

    key.data = (char *) name;
    key.size = strlen(name);

    data.data = (void*)pa_tagstruct_data(t, &data.size);

    r = (pa_database_set(u->database, &key, &data, TRUE) == 0);

    pa_tagstruct_free(t);

    return r;
}

static struct entry* entry_copy(const struct entry *e) {
    struct entry* r;

    pa_assert(e);
    r = entry_new();
    *r = *e;
    r->port = pa_xstrdup(e->port);
    return r;
}

static void trigger_save(struct userdata *u) {
    if (u->save_time_event)
        return;

    u->save_time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + SAVE_INTERVAL, save_time_callback, u);
}

static pa_bool_t entries_equal(const struct entry *a, const struct entry *b) {
    pa_cvolume t;

    if (a->port_valid != b->port_valid ||
        (a->port_valid && !pa_streq(a->port, b->port)))
        return FALSE;

    if (a->muted_valid != b->muted_valid ||
        (a->muted_valid && (a->muted != b->muted)))
        return FALSE;

    t = b->volume;
    if (a->volume_valid != b->volume_valid ||
        (a->volume_valid && !pa_cvolume_equal(pa_cvolume_remap(&t, &b->channel_map, &a->channel_map), &a->volume)))
        return FALSE;

    return TRUE;
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry *entry, *old;
    char *name;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK) {
        pa_sink *sink;

        if (!(sink = pa_idxset_get_by_index(c->sinks, idx)))
            return;

        name = pa_sprintf_malloc("sink:%s", sink->name);

        if ((old = entry_read(u, name)))
            entry = entry_copy(old);
        else
            entry = entry_new();

        if (sink->save_volume) {
            entry->channel_map = sink->channel_map;
            entry->volume = *pa_sink_get_volume(sink, FALSE);
            entry->volume_valid = TRUE;
        }

        if (sink->save_muted) {
            entry->muted = pa_sink_get_mute(sink, FALSE);
            entry->muted_valid = TRUE;
        }

        if (sink->save_port) {
            pa_xfree(entry->port);
            entry->port = pa_xstrdup(sink->active_port ? sink->active_port->name : "");
            entry->port_valid = TRUE;
        }

    } else {
        pa_source *source;

        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE);

        if (!(source = pa_idxset_get_by_index(c->sources, idx)))
            return;

        name = pa_sprintf_malloc("source:%s", source->name);

        if ((old = entry_read(u, name)))
            entry = entry_copy(old);
        else
            entry = entry_new();

        if (source->save_volume) {
            entry->channel_map = source->channel_map;
            entry->volume = *pa_source_get_volume(source, FALSE);
            entry->volume_valid = TRUE;
        }

        if (source->save_muted) {
            entry->muted = pa_source_get_mute(source, FALSE);
            entry->muted_valid = TRUE;
        }

        if (source->save_port) {
            pa_xfree(entry->port);
            entry->port = pa_xstrdup(source->active_port ? source->active_port->name : "");
            entry->port_valid = TRUE;
        }
    }

    pa_assert(entry);

    if (old) {

        if (entries_equal(old, entry)) {
            entry_free(old);
            entry_free(entry);
            pa_xfree(name);
            return;
        }

        entry_free(old);
    }

    pa_log_info("Storing volume/mute/port for device %s.", name);

    if (entry_write(u, name, entry))
        trigger_save(u);

    entry_free(entry);
    pa_xfree(name);
}

static pa_hook_result_t sink_new_hook_callback(pa_core *c, pa_sink_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);
    pa_assert(u->restore_port);

    name = pa_sprintf_malloc("sink:%s", new_data->name);

    if ((e = entry_read(u, name))) {

        if (e->port_valid) {
            if (!new_data->active_port) {
                pa_log_info("Restoring port for sink %s.", name);
                pa_sink_new_data_set_port(new_data, e->port);
                new_data->save_port = TRUE;
            } else
                pa_log_debug("Not restoring port for sink %s, because already set.", name);
        }

        entry_free(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_fixate_hook_callback(pa_core *c, pa_sink_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);
    pa_assert(u->restore_volume || u->restore_muted);

    name = pa_sprintf_malloc("sink:%s", new_data->name);

    if ((e = entry_read(u, name))) {

        if (u->restore_volume && e->volume_valid) {

            if (!new_data->volume_is_set) {
                pa_cvolume v;

                pa_log_info("Restoring volume for sink %s.", new_data->name);

                v = e->volume;
                pa_cvolume_remap(&v, &e->channel_map, &new_data->channel_map);
                pa_sink_new_data_set_volume(new_data, &v);

                new_data->save_volume = TRUE;
            } else
                pa_log_debug("Not restoring volume for sink %s, because already set.", new_data->name);
        }

        if (u->restore_muted && e->muted_valid) {

            if (!new_data->muted_is_set) {
                pa_log_info("Restoring mute state for sink %s.", new_data->name);
                pa_sink_new_data_set_muted(new_data, e->muted);
                new_data->save_muted = TRUE;
            } else
                pa_log_debug("Not restoring mute state for sink %s, because already set.", new_data->name);
        }

        entry_free(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_new_hook_callback(pa_core *c, pa_source_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);
    pa_assert(u->restore_port);

    name = pa_sprintf_malloc("source:%s", new_data->name);

    if ((e = entry_read(u, name))) {

        if (e->port_valid) {
            if (!new_data->active_port) {
                pa_log_info("Restoring port for source %s.", name);
                pa_source_new_data_set_port(new_data, e->port);
                new_data->save_port = TRUE;
            } else
                pa_log_debug("Not restoring port for source %s, because already set.", name);
        }

        entry_free(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_fixate_hook_callback(pa_core *c, pa_source_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);
    pa_assert(u->restore_volume || u->restore_muted);

    name = pa_sprintf_malloc("source:%s", new_data->name);

    if ((e = entry_read(u, name))) {

        if (u->restore_volume && e->volume_valid) {

            if (!new_data->volume_is_set) {
                pa_cvolume v;

                pa_log_info("Restoring volume for source %s.", new_data->name);

                v = e->volume;
                pa_cvolume_remap(&v, &e->channel_map, &new_data->channel_map);
                pa_source_new_data_set_volume(new_data, &v);

                new_data->save_volume = TRUE;
            } else
                pa_log_debug("Not restoring volume for source %s, because already set.", new_data->name);
        }

        if (u->restore_muted && e->muted_valid) {

            if (!new_data->muted_is_set) {
                pa_log_info("Restoring mute state for source %s.", new_data->name);
                pa_source_new_data_set_muted(new_data, e->muted);
                new_data->save_muted = TRUE;
            } else
                pa_log_debug("Not restoring mute state for source %s, because already set.", new_data->name);
        }

        entry_free(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    char *fname;
    pa_sink *sink;
    pa_source *source;
    uint32_t idx;
    pa_bool_t restore_volume = TRUE, restore_muted = TRUE, restore_port = TRUE;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "restore_volume", &restore_volume) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_muted", &restore_muted) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_port", &restore_port) < 0) {
        pa_log("restore_port=, restore_volume= and restore_muted= expect boolean arguments");
        goto fail;
    }

    if (!restore_muted && !restore_volume && !restore_port)
        pa_log_warn("Neither restoring volume, nor restoring muted, nor restoring port enabled!");

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->restore_volume = restore_volume;
    u->restore_muted = restore_muted;
    u->restore_port = restore_port;

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SOURCE, subscribe_callback, u);

    if (restore_port) {
        u->sink_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) sink_new_hook_callback, u);
        u->source_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) source_new_hook_callback, u);
    }

    if (restore_muted || restore_volume) {
        u->sink_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) sink_fixate_hook_callback, u);
        u->source_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) source_fixate_hook_callback, u);
    }

    if (!(fname = pa_state_path("device-volumes", TRUE)))
        goto fail;

    if (!(u->database = pa_database_open(fname, TRUE))) {
        pa_log("Failed to open volume database '%s': %s", fname, pa_cstrerror(errno));
        pa_xfree(fname);
        goto fail;
    }

    pa_log_info("Successfully opened database file '%s'.", fname);
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

    return -1;
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
    if (u->sink_new_hook_slot)
        pa_hook_slot_free(u->sink_new_hook_slot);
    if (u->source_new_hook_slot)
        pa_hook_slot_free(u->source_new_hook_slot);

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    if (u->database)
        pa_database_close(u->database);

    pa_xfree(u);
}
