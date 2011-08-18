/***
  This file is part of PulseAudio.

  Copyright 2006-2008 Lennart Poettering
  Copyright 2011 Colin Guthrie

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

#include <pulse/gccmacro.h>
#include <pulse/xmalloc.h>
#include <pulse/volume.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>
#include <pulse/format.h>
#include <pulse/internal.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/namereg.h>
#include <pulsecore/protocol-native.h>
#include <pulsecore/pstream.h>
#include <pulsecore/pstream-util.h>
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
        "restore_muted=<Save/restore muted states?> "
        "restore_formats=<Save/restore saved formats?>");

#define SAVE_INTERVAL (10 * PA_USEC_PER_SEC)

static const char* const valid_modargs[] = {
    "restore_volume",
    "restore_muted",
    "restore_port",
    "restore_formats",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_subscription *subscription;
    pa_hook_slot
        *sink_new_hook_slot,
        *sink_fixate_hook_slot,
        *sink_put_hook_slot,
        *source_new_hook_slot,
        *source_fixate_hook_slot,
        *connection_unlink_hook_slot;
    pa_time_event *save_time_event;
    pa_database *database;

    pa_native_protocol *protocol;
    pa_idxset *subscribed;

    pa_bool_t restore_volume:1;
    pa_bool_t restore_muted:1;
    pa_bool_t restore_port:1;
    pa_bool_t restore_formats:1;
};

/* Protocol extention commands */
enum {
    SUBCOMMAND_TEST,
    SUBCOMMAND_SUBSCRIBE,
    SUBCOMMAND_EVENT,
    SUBCOMMAND_READ_FORMATS_ALL,
    SUBCOMMAND_READ_FORMATS,
    SUBCOMMAND_SAVE_FORMATS
};


#define ENTRY_VERSION 1

struct entry {
    uint8_t version;
    pa_bool_t muted_valid, volume_valid, port_valid;
    pa_bool_t muted;
    pa_channel_map channel_map;
    pa_cvolume volume;
    char *port;
    pa_idxset *formats;
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

static void trigger_save(struct userdata *u, pa_device_type_t type, uint32_t sink_idx) {
    pa_native_connection *c;
    uint32_t idx;

    if (sink_idx != PA_INVALID_INDEX) {
        for (c = pa_idxset_first(u->subscribed, &idx); c; c = pa_idxset_next(u->subscribed, &idx)) {
            pa_tagstruct *t;

            t = pa_tagstruct_new(NULL, 0);
            pa_tagstruct_putu32(t, PA_COMMAND_EXTENSION);
            pa_tagstruct_putu32(t, 0);
            pa_tagstruct_putu32(t, u->module->index);
            pa_tagstruct_puts(t, u->module->name);
            pa_tagstruct_putu32(t, SUBCOMMAND_EVENT);
            pa_tagstruct_putu32(t, type);
            pa_tagstruct_putu32(t, sink_idx);

            pa_pstream_send_tagstruct(pa_native_connection_get_pstream(c), t);
        }
    }

    if (u->save_time_event)
        return;

    u->save_time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + SAVE_INTERVAL, save_time_callback, u);
}

static struct entry* entry_new(pa_bool_t add_pcm_format) {
    struct entry *r = pa_xnew0(struct entry, 1);
    r->version = ENTRY_VERSION;
    r->formats = pa_idxset_new(NULL, NULL);
    if (add_pcm_format) {
        pa_format_info *f = pa_format_info_new();
        f->encoding = PA_ENCODING_PCM;
        pa_idxset_put(r->formats, f, NULL);
    }
    return r;
}

static void entry_free(struct entry* e) {
    pa_assert(e);

    pa_idxset_free(e->formats, (pa_free2_cb_t) pa_format_info_free2, NULL);
    pa_xfree(e->port);
    pa_xfree(e);
}

static pa_bool_t entry_write(struct userdata *u, const char *name, const struct entry *e) {
    pa_tagstruct *t;
    pa_datum key, data;
    pa_bool_t r;
    uint32_t i;
    pa_format_info *f;
    uint8_t n_formats;

    pa_assert(u);
    pa_assert(name);
    pa_assert(e);

    n_formats = pa_idxset_size(e->formats);
    pa_assert(n_formats > 0);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu8(t, e->version);
    pa_tagstruct_put_boolean(t, e->volume_valid);
    pa_tagstruct_put_channel_map(t, &e->channel_map);
    pa_tagstruct_put_cvolume(t, &e->volume);
    pa_tagstruct_put_boolean(t, e->muted_valid);
    pa_tagstruct_put_boolean(t, e->muted);
    pa_tagstruct_put_boolean(t, e->port_valid);
    pa_tagstruct_puts(t, e->port);
    pa_tagstruct_putu8(t, n_formats);

    PA_IDXSET_FOREACH(f, e->formats, i) {
        pa_tagstruct_put_format_info(t, f);
    }

    key.data = (char *) name;
    key.size = strlen(name);

    data.data = (void*)pa_tagstruct_data(t, &data.size);

    r = (pa_database_set(u->database, &key, &data, TRUE) == 0);

    pa_tagstruct_free(t);

    return r;
}

#ifdef ENABLE_LEGACY_DATABASE_ENTRY_FORMAT

#define LEGACY_ENTRY_VERSION 2
static struct entry* legacy_entry_read(struct userdata *u, pa_datum *data) {
    struct legacy_entry {
        uint8_t version;
        pa_bool_t muted_valid:1, volume_valid:1, port_valid:1;
        pa_bool_t muted:1;
        pa_channel_map channel_map;
        pa_cvolume volume;
        char port[PA_NAME_MAX];
    } PA_GCC_PACKED;
    struct legacy_entry *le;
    struct entry *e;

    pa_assert(u);
    pa_assert(data);

    if (data->size != sizeof(struct legacy_entry)) {
        pa_log_debug("Size does not match.");
        return NULL;
    }

    le = (struct legacy_entry*)data->data;

    if (le->version != LEGACY_ENTRY_VERSION) {
        pa_log_debug("Version mismatch.");
        return NULL;
    }

    if (!memchr(le->port, 0, sizeof(le->port))) {
        pa_log_warn("Port has missing NUL byte.");
        return NULL;
    }

    if (le->volume_valid && !pa_channel_map_valid(&le->channel_map)) {
        pa_log_warn("Invalid channel map.");
        return NULL;
    }

    if (le->volume_valid && (!pa_cvolume_valid(&le->volume) || !pa_cvolume_compatible_with_channel_map(&le->volume, &le->channel_map))) {
        pa_log_warn("Volume and channel map don't match.");
        return NULL;
    }

    e = entry_new(TRUE);
    e->muted_valid = le->muted_valid;
    e->volume_valid = le->volume_valid;
    e->port_valid = le->port_valid;
    e->muted = le->muted;
    e->channel_map = le->channel_map;
    e->volume = le->volume;
    e->port = pa_xstrdup(le->port);
    return e;
}
#endif

static struct entry* entry_read(struct userdata *u, const char *name) {
    pa_datum key, data;
    struct entry *e = NULL;
    pa_tagstruct *t = NULL;
    const char* port;
    uint8_t i, n_formats;

    pa_assert(u);
    pa_assert(name);

    key.data = (char*) name;
    key.size = strlen(name);

    pa_zero(data);

    if (!pa_database_get(u->database, &key, &data))
        goto fail;

    t = pa_tagstruct_new(data.data, data.size);
    e = entry_new(FALSE);

    if (pa_tagstruct_getu8(t, &e->version) < 0 ||
        e->version > ENTRY_VERSION ||
        pa_tagstruct_get_boolean(t, &e->volume_valid) < 0 ||
        pa_tagstruct_get_channel_map(t, &e->channel_map) < 0 ||
        pa_tagstruct_get_cvolume(t, &e->volume) < 0 ||
        pa_tagstruct_get_boolean(t, &e->muted_valid) < 0 ||
        pa_tagstruct_get_boolean(t, &e->muted) < 0 ||
        pa_tagstruct_get_boolean(t, &e->port_valid) < 0 ||
        pa_tagstruct_gets(t, &port) < 0 ||
        pa_tagstruct_getu8(t, &n_formats) < 0 || n_formats < 1) {

        goto fail;
    }

    e->port = pa_xstrdup(port);

    for (i = 0; i < n_formats; ++i) {
        pa_format_info *f = pa_format_info_new();
        if (pa_tagstruct_get_format_info(t, f) < 0) {
            pa_format_info_free(f);
            goto fail;
        }
        pa_idxset_put(e->formats, f, NULL);
    }

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

#ifdef ENABLE_LEGACY_DATABASE_ENTRY_FORMAT
    pa_log_debug("Attempting to load legacy (pre-v1.0) data for key: %s", name);
    if ((e = legacy_entry_read(u, &data))) {
        pa_log_debug("Success. Saving new format for key: %s", name);
        if (entry_write(u, name, e))
            trigger_save(u, PA_DEVICE_TYPE_SINK, PA_INVALID_INDEX);
        pa_datum_free(&data);
        return e;
    } else
        pa_log_debug("Unable to load legacy (pre-v1.0) data for key: %s. Ignoring.", name);
#endif

    pa_datum_free(&data);
    return NULL;
}

static struct entry* entry_copy(const struct entry *e) {
    struct entry* r;
    uint32_t idx;
    pa_format_info *f;

    pa_assert(e);
    r = entry_new(FALSE);
    r->version = e->version;
    r->muted_valid = e->muted_valid;
    r->volume_valid = e->volume_valid;
    r->port_valid = e->port_valid;
    r->muted = e->muted;
    r->channel_map = e->channel_map;
    r->volume = e->volume;
    r->port = pa_xstrdup(e->port);

    PA_IDXSET_FOREACH(f, e->formats, idx) {
        pa_idxset_put(r->formats, pa_format_info_copy(f), NULL);
    }
    return r;
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

    if (pa_idxset_size(a->formats) != pa_idxset_size(b->formats))
        return FALSE;

    /** TODO: Compare a bit better */

    return TRUE;
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry *entry, *old;
    char *name;
    pa_device_type_t type;

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

        type = PA_DEVICE_TYPE_SINK;
        name = pa_sprintf_malloc("sink:%s", sink->name);

        if ((old = entry_read(u, name)))
            entry = entry_copy(old);
        else
            entry = entry_new(TRUE);

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

        type = PA_DEVICE_TYPE_SOURCE;
        name = pa_sprintf_malloc("source:%s", source->name);

        if ((old = entry_read(u, name)))
            entry = entry_copy(old);
        else
            entry = entry_new(TRUE);

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
        trigger_save(u, type, idx);

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

static pa_hook_result_t sink_put_hook_callback(pa_core *c, pa_sink *sink, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);
    pa_assert(u->restore_formats);

    name = pa_sprintf_malloc("sink:%s", sink->name);

    if ((e = entry_read(u, name))) {

        if (!pa_sink_set_formats(sink, e->formats))
            pa_log_debug("Could not set format on sink %s", sink->name);

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

#define EXT_VERSION 1

static void read_sink_format_reply(struct userdata *u, pa_tagstruct *reply, pa_sink *sink) {
    struct entry *e;
    char *name;

    pa_assert(u);
    pa_assert(reply);
    pa_assert(sink);

    pa_tagstruct_putu32(reply, PA_DEVICE_TYPE_SINK);
    pa_tagstruct_putu32(reply, sink->index);

    /* Read or create an entry */
    name = pa_sprintf_malloc("sink:%s", sink->name);
    if (!(e = entry_read(u, name))) {
        /* Fake a reply with PCM encoding supported */
        pa_format_info *f = pa_format_info_new();

        pa_tagstruct_putu8(reply, 1);
        f->encoding = PA_ENCODING_PCM;
        pa_tagstruct_put_format_info(reply, f);

        pa_format_info_free(f);
    } else {
        uint32_t idx;
        pa_format_info *f;

        /* Write all the formats from the entry to the reply */
        pa_tagstruct_putu8(reply, pa_idxset_size(e->formats));
        PA_IDXSET_FOREACH(f, e->formats, idx) {
            pa_tagstruct_put_format_info(reply, f);
        }
    }
    pa_xfree(name);
}

static int extension_cb(pa_native_protocol *p, pa_module *m, pa_native_connection *c, uint32_t tag, pa_tagstruct *t) {
    struct userdata *u;
    uint32_t command;
    pa_tagstruct *reply = NULL;

    pa_assert(p);
    pa_assert(m);
    pa_assert(c);
    pa_assert(t);

    u = m->userdata;

    if (pa_tagstruct_getu32(t, &command) < 0)
        goto fail;

    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);

    switch (command) {
        case SUBCOMMAND_TEST: {
            if (!pa_tagstruct_eof(t))
                goto fail;

            pa_tagstruct_putu32(reply, EXT_VERSION);
            break;
        }

        case SUBCOMMAND_SUBSCRIBE: {

            pa_bool_t enabled;

            if (pa_tagstruct_get_boolean(t, &enabled) < 0 ||
                !pa_tagstruct_eof(t))
                goto fail;

            if (enabled)
                pa_idxset_put(u->subscribed, c, NULL);
            else
                pa_idxset_remove_by_data(u->subscribed, c, NULL);

            break;
        }

        case SUBCOMMAND_READ_FORMATS_ALL: {
            pa_sink *sink;
            uint32_t idx;

            if (!pa_tagstruct_eof(t))
                goto fail;

            PA_IDXSET_FOREACH(sink, u->core->sinks, idx) {
                read_sink_format_reply(u, reply, sink);
            }

            break;
        }
        case SUBCOMMAND_READ_FORMATS: {
            pa_device_type_t type;
            uint32_t sink_index;
            pa_sink *sink;

            pa_assert(reply);

            /* Get the sink index and the number of formats from the tagstruct */
            if (pa_tagstruct_getu32(t, &type) < 0 ||
                pa_tagstruct_getu32(t, &sink_index) < 0)
                goto fail;

            if (type != PA_DEVICE_TYPE_SINK) {
                pa_log("Device format reading is only supported on sinks");
                goto fail;
            }

            if (!pa_tagstruct_eof(t))
                goto fail;

            /* Now find our sink */
            if (!(sink = pa_idxset_get_by_index(u->core->sinks, sink_index)))
                goto fail;

            read_sink_format_reply(u, reply, sink);

            break;
        }

        case SUBCOMMAND_SAVE_FORMATS: {

            struct entry *e;
            pa_device_type_t type;
            uint32_t sink_index;
            char *name;
            pa_sink *sink;
            uint8_t i, n_formats;

            /* Get the sink index and the number of formats from the tagstruct */
            if (pa_tagstruct_getu32(t, &type) < 0 ||
                pa_tagstruct_getu32(t, &sink_index) < 0 ||
                pa_tagstruct_getu8(t, &n_formats) < 0 || n_formats < 1) {

                goto fail;
            }

            if (type != PA_DEVICE_TYPE_SINK) {
                pa_log("Device format saving is only supported on sinks");
                goto fail;
            }

            /* Now find our sink */
            if (!(sink = pa_idxset_get_by_index(u->core->sinks, sink_index))) {
                pa_log("Could not find sink #%d", sink_index);
                goto fail;
            }

            /* Read or create an entry */
            name = pa_sprintf_malloc("sink:%s", sink->name);
            if (!(e = entry_read(u, name)))
                e = entry_new(FALSE);
            else {
                /* Clean out any saved formats */
                pa_idxset_free(e->formats, (pa_free2_cb_t) pa_format_info_free2, NULL);
                e->formats = pa_idxset_new(NULL, NULL);
            }

            /* Read all the formats from our tagstruct */
            for (i = 0; i < n_formats; ++i) {
                pa_format_info *f = pa_format_info_new();
                if (pa_tagstruct_get_format_info(t, f) < 0) {
                    pa_format_info_free(f);
                    pa_xfree(name);
                    goto fail;
                }
                pa_idxset_put(e->formats, f, NULL);
            }

            if (!pa_tagstruct_eof(t)) {
                entry_free(e);
                pa_xfree(name);
                goto fail;
            }

            if (pa_sink_set_formats(sink, e->formats) && entry_write(u, name, e))
                trigger_save(u, type, sink_index);
            else
                pa_log_warn("Could not save format info for sink %s", sink->name);

            pa_xfree(name);
            entry_free(e);

            break;
        }

        default:
            goto fail;
    }

    pa_pstream_send_tagstruct(pa_native_connection_get_pstream(c), reply);
    return 0;

fail:

    if (reply)
        pa_tagstruct_free(reply);

    return -1;
}

static pa_hook_result_t connection_unlink_hook_cb(pa_native_protocol *p, pa_native_connection *c, struct userdata *u) {
    pa_assert(p);
    pa_assert(c);
    pa_assert(u);

    pa_idxset_remove_by_data(u->subscribed, c, NULL);
    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    char *fname;
    pa_sink *sink;
    pa_source *source;
    uint32_t idx;
    pa_bool_t restore_volume = TRUE, restore_muted = TRUE, restore_port = TRUE, restore_formats = TRUE;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "restore_volume", &restore_volume) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_muted", &restore_muted) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_port", &restore_port) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_formats", &restore_formats) < 0) {
        pa_log("restore_port, restore_volume, restore_muted and restore_formats expect boolean arguments");
        goto fail;
    }

    if (!restore_muted && !restore_volume && !restore_port && !restore_formats)
        pa_log_warn("Neither restoring volume, nor restoring muted, nor restoring port enabled!");

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->restore_volume = restore_volume;
    u->restore_muted = restore_muted;
    u->restore_port = restore_port;
    u->restore_formats = restore_formats;

    u->subscribed = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->protocol = pa_native_protocol_get(m->core);
    pa_native_protocol_install_ext(u->protocol, m, extension_cb);

    u->connection_unlink_hook_slot = pa_hook_connect(&pa_native_protocol_hooks(u->protocol)[PA_NATIVE_HOOK_CONNECTION_UNLINK], PA_HOOK_NORMAL, (pa_hook_cb_t) connection_unlink_hook_cb, u);

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SOURCE, subscribe_callback, u);

    if (restore_port) {
        u->sink_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) sink_new_hook_callback, u);
        u->source_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) source_new_hook_callback, u);
    }

    if (restore_muted || restore_volume) {
        u->sink_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) sink_fixate_hook_callback, u);
        u->source_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) source_fixate_hook_callback, u);
    }

    if (restore_formats)
        u->sink_put_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_EARLY, (pa_hook_cb_t) sink_put_hook_callback, u);

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
    if (u->sink_put_hook_slot)
        pa_hook_slot_free(u->sink_put_hook_slot);

    if (u->connection_unlink_hook_slot)
        pa_hook_slot_free(u->connection_unlink_hook_slot);

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    if (u->database)
        pa_database_close(u->database);

    if (u->protocol) {
        pa_native_protocol_remove_ext(u->protocol, m);
        pa_native_protocol_unref(u->protocol);
    }

    if (u->subscribed)
        pa_idxset_free(u->subscribed, NULL, NULL);

    pa_xfree(u);
}
