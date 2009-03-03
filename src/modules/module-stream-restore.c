/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering

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
#include <pulsecore/protocol-native.h>
#include <pulsecore/pstream.h>
#include <pulsecore/pstream-util.h>

#include "module-stream-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore the volume/mute/device state of streams");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "restore_device=<Save/restore sinks/sources?> "
        "restore_volume=<Save/restore volumes?> "
        "restore_muted=<Save/restore muted states?>");

#define SAVE_INTERVAL 10
#define IDENTIFICATION_PROPERTY "module-stream-restore.id"

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
        *source_output_new_hook_slot,
        *connection_unlink_hook_slot;
    pa_time_event *save_time_event;
    GDBM_FILE gdbm_file;

    pa_bool_t restore_device:1;
    pa_bool_t restore_volume:1;
    pa_bool_t restore_muted:1;

    pa_native_protocol *protocol;
    pa_idxset *subscribed;
};

#define ENTRY_VERSION 1

struct entry {
    uint8_t version;
    pa_bool_t muted_valid:1, relative_volume_valid:1, absolute_volume_valid:1, device_valid:1;
    pa_bool_t muted:1;
    pa_channel_map channel_map;
    pa_cvolume relative_volume;
    pa_cvolume absolute_volume;
    char device[PA_NAME_MAX];
} PA_GCC_PACKED;

enum {
    SUBCOMMAND_TEST,
    SUBCOMMAND_READ,
    SUBCOMMAND_WRITE,
    SUBCOMMAND_DELETE,
    SUBCOMMAND_SUBSCRIBE,
    SUBCOMMAND_EVENT
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
    char *t;

    if (!p)
        return NULL;

    if ((r = pa_proplist_gets(p, IDENTIFICATION_PROPERTY)))
        return pa_xstrdup(r);

    if ((r = pa_proplist_gets(p, PA_PROP_MEDIA_ROLE)))
        t = pa_sprintf_malloc("%s-by-media-role:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_APPLICATION_ID)))
        t = pa_sprintf_malloc("%s-by-application-id:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_APPLICATION_NAME)))
        t = pa_sprintf_malloc("%s-by-application-name:%s", prefix, r);
    else if ((r = pa_proplist_gets(p, PA_PROP_MEDIA_NAME)))
        t = pa_sprintf_malloc("%s-by-media-name:%s", prefix, r);
    else
        t = pa_sprintf_malloc("%s-fallback:%s", prefix, r);

    pa_proplist_sets(p, IDENTIFICATION_PROPERTY, t);
    return t;
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
        /* This is probably just a database upgrade, hence let's not
         * consider this more than a debug message */
        pa_log_debug("Database contains entry for stream %s of wrong size %lu != %lu. Probably due to uprade, ignoring.", name, (unsigned long) data.dsize, (unsigned long) sizeof(struct entry));
        goto fail;
    }

    e = (struct entry*) data.dptr;

    if (e->version != ENTRY_VERSION) {
        pa_log_debug("Version of database entry for stream %s doesn't match our version. Probably due to upgrade, ignoring.", name);
        goto fail;
    }

    if (!memchr(e->device, 0, sizeof(e->device))) {
        pa_log_warn("Database contains entry for stream %s with missing NUL byte in device name", name);
        goto fail;
    }

    if (e->device_valid && !pa_namereg_is_valid_name(e->device)) {
        pa_log_warn("Invalid device name stored in database for stream %s", name);
        goto fail;
    }

    if ((e->relative_volume_valid || e->absolute_volume_valid) && !(pa_channel_map_valid(&e->channel_map))) {
        pa_log_warn("Invalid channel map stored in database for stream %s", name);
        goto fail;
    }

    if ((e->relative_volume_valid && (!pa_cvolume_valid(&e->relative_volume) || e->relative_volume.channels != e->channel_map.channels)) ||
        (e->absolute_volume_valid && (!pa_cvolume_valid(&e->absolute_volume) || e->absolute_volume.channels != e->channel_map.channels))) {
        pa_log_warn("Invalid volume stored in database for stream %s", name);
        goto fail;
    }

    return e;

fail:

    pa_xfree(data.dptr);
    return NULL;
}

static void trigger_save(struct userdata *u) {
    struct timeval tv;
    pa_native_connection *c;
    uint32_t idx;

    for (c = pa_idxset_first(u->subscribed, &idx); c; c = pa_idxset_next(u->subscribed, &idx)) {
        pa_tagstruct *t;

        t = pa_tagstruct_new(NULL, 0);
        pa_tagstruct_putu32(t, PA_COMMAND_EXTENSION);
        pa_tagstruct_putu32(t, 0);
        pa_tagstruct_putu32(t, u->module->index);
        pa_tagstruct_puts(t, u->module->name);
        pa_tagstruct_putu32(t, SUBCOMMAND_EVENT);

        pa_pstream_send_tagstruct(pa_native_connection_get_pstream(c), t);
    }

    if (u->save_time_event)
        return;

    pa_gettimeofday(&tv);
    tv.tv_sec += SAVE_INTERVAL;
    u->save_time_event = u->core->mainloop->time_new(u->core->mainloop, &tv, save_time_callback, u);
}

static pa_bool_t entries_equal(const struct entry *a, const struct entry *b) {
    pa_cvolume t;

    pa_assert(a);
    pa_assert(b);

    if (a->device_valid != b->device_valid ||
        (a->device_valid && strncmp(a->device, b->device, sizeof(a->device))))
        return FALSE;

    if (a->muted_valid != b->muted_valid ||
        (a->muted_valid && (a->muted != b->muted)))
        return FALSE;

    t = b->relative_volume;
    if (a->relative_volume_valid != b->relative_volume_valid ||
        (a->relative_volume_valid && !pa_cvolume_equal(pa_cvolume_remap(&t, &b->channel_map, &a->channel_map), &a->relative_volume)))
        return FALSE;

    t = b->absolute_volume;
    if (a->absolute_volume_valid != b->absolute_volume_valid ||
        (a->absolute_volume_valid && !pa_cvolume_equal(pa_cvolume_remap(&t, &b->channel_map, &a->channel_map), &a->absolute_volume)))
        return FALSE;

    return TRUE;
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

    memset(&entry, 0, sizeof(entry));
    entry.version = ENTRY_VERSION;

    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        pa_sink_input *sink_input;

        if (!(sink_input = pa_idxset_get_by_index(c->sink_inputs, idx)))
            return;

        if (!(name = get_name(sink_input->proplist, "sink-input")))
            return;

        entry.channel_map = sink_input->channel_map;

        if (sink_input->sink->flags & PA_SINK_FLAT_VOLUME) {
            entry.absolute_volume = *pa_sink_input_get_volume(sink_input);
            entry.absolute_volume_valid = sink_input->save_volume;

            pa_sw_cvolume_divide(&entry.relative_volume, &entry.absolute_volume, pa_sink_get_volume(sink_input->sink, FALSE));
            entry.relative_volume_valid = sink_input->save_volume;
        } else {
            entry.relative_volume = *pa_sink_input_get_volume(sink_input);
            entry.relative_volume_valid = sink_input->save_volume;
        }

        entry.muted = pa_sink_input_get_mute(sink_input);
        entry.muted_valid = sink_input->save_muted;

        pa_strlcpy(entry.device, sink_input->sink->name, sizeof(entry.device));
        entry.device_valid = sink_input->save_sink;

    } else {
        pa_source_output *source_output;

        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT);

        if (!(source_output = pa_idxset_get_by_index(c->source_outputs, idx)))
            return;

        if (!(name = get_name(source_output->proplist, "source-output")))
            return;

        entry.channel_map = source_output->channel_map;

        pa_strlcpy(entry.device, source_output->source->name, sizeof(entry.device));
        entry.device_valid = source_output->save_source;
    }

    if ((old = read_entry(u, name))) {

        if (entries_equal(old, &entry)) {
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

    pa_log_info("Storing volume/mute/device for stream %s.", name);

    gdbm_store(u->gdbm_file, key, data, GDBM_REPLACE);

    pa_xfree(name);

    trigger_save(u);
}

static pa_hook_result_t sink_input_new_hook_callback(pa_core *c, pa_sink_input_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(new_data);

    if (!u->restore_device)
        return PA_HOOK_OK;

    if (!(name = get_name(new_data->proplist, "sink-input")))
        return PA_HOOK_OK;

    if ((e = read_entry(u, name))) {
        pa_sink *s;

        if (e->device_valid) {

            if ((s = pa_namereg_get(c, e->device, PA_NAMEREG_SINK))) {
                if (!new_data->sink) {
                    pa_log_info("Restoring device for stream %s.", name);
                    new_data->sink = s;
                    new_data->save_sink = TRUE;
                } else
                    pa_log_info("Not restore device for stream %s, because already set.", name);
            }
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

    if (!u->restore_volume && !u->restore_muted)
        return PA_HOOK_OK;

    if (!(name = get_name(new_data->proplist, "sink-input")))
        return PA_HOOK_OK;

    if ((e = read_entry(u, name))) {

        if (u->restore_volume) {

            if (!new_data->volume_is_set) {
                pa_cvolume v;
                pa_cvolume_init(&v);

                if (new_data->sink->flags & PA_SINK_FLAT_VOLUME) {

                    /* We don't check for e->device_valid here because
                    that bit marks whether it is a good choice for
                    restoring, not just if the data is filled in. */
                    if (e->absolute_volume_valid &&
                        (e->device[0] == 0 || pa_streq(new_data->sink->name, e->device))) {

                        v = e->absolute_volume;
                        new_data->volume_is_absolute = TRUE;
                    } else if (e->relative_volume_valid) {
                        v = e->relative_volume;
                        new_data->volume_is_absolute = FALSE;
                    }

                } else if (e->relative_volume_valid) {
                    v = e->relative_volume;
                    new_data->volume_is_absolute = FALSE;
                }

                if (v.channels > 0) {
                    pa_log_info("Restoring volume for sink input %s.", name);
                    pa_sink_input_new_data_set_volume(new_data, pa_cvolume_remap(&v, &e->channel_map, &new_data->channel_map));
                    new_data->save_volume = TRUE;
                }
            } else
                pa_log_debug("Not restoring volume for sink input %s, because already set.", name);
        }

        if (u->restore_muted && e->muted_valid) {
            if (!new_data->muted_is_set) {
                pa_log_info("Restoring mute state for sink input %s.", name);
                pa_sink_input_new_data_set_muted(new_data, e->muted);
                new_data->save_muted = TRUE;
            } else
                pa_log_debug("Not restoring mute state for sink input %s, because already set.", name);
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

    if (!u->restore_device)
        return PA_HOOK_OK;

    if (new_data->direct_on_input)
        return PA_HOOK_OK;

    if (!(name = get_name(new_data->proplist, "source-output")))
        return PA_HOOK_OK;

    if ((e = read_entry(u, name))) {
        pa_source *s;

        if (e->device_valid) {
            if ((s = pa_namereg_get(c, e->device, PA_NAMEREG_SOURCE))) {
                if (!new_data->source) {
                    pa_log_info("Restoring device for stream %s.", name);
                    new_data->source = s;
                    new_data->save_source = TRUE;
                } else
                    pa_log_info("Not restoring device for stream %s, because already set", name);
            }
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

#define EXT_VERSION 1

static void clear_db(struct userdata *u) {
    datum key;

    pa_assert(u);

    key = gdbm_firstkey(u->gdbm_file);
    while (key.dptr) {
        datum next_key;
        next_key = gdbm_nextkey(u->gdbm_file, key);

        gdbm_delete(u->gdbm_file, key);
        pa_xfree(key.dptr);

        key = next_key;
    }

    gdbm_reorganize(u->gdbm_file);
}

static void apply_entry(struct userdata *u, const char *name, struct entry *e) {
    pa_sink_input *si;
    pa_source_output *so;
    uint32_t idx;

    pa_assert(u);
    pa_assert(name);
    pa_assert(e);

    for (si = pa_idxset_first(u->core->sink_inputs, &idx); si; si = pa_idxset_next(u->core->sink_inputs, &idx)) {
        char *n;
        pa_sink *s;

        if (!(n = get_name(si->proplist, "sink-input")))
            continue;

        if (strcmp(name, n)) {
            pa_xfree(n);
            continue;
        }
	pa_xfree(n);

        if (u->restore_volume) {
            pa_cvolume v;
            pa_cvolume_init(&v);

            if (si->sink->flags & PA_SINK_FLAT_VOLUME) {

                if (e->absolute_volume_valid &&
                    (e->device[0] == 0 || pa_streq(e->device, si->sink->name)))
                    v = e->absolute_volume;
                else if (e->relative_volume_valid) {
                    pa_cvolume t = *pa_sink_get_volume(si->sink, FALSE);
                    pa_sw_cvolume_multiply(&v, &e->relative_volume, pa_cvolume_remap(&t, &si->sink->channel_map, &e->channel_map));
                }
            } else if (e->relative_volume_valid)
                v = e->relative_volume;

            if (v.channels > 0) {
                pa_log_info("Restoring volume for sink input %s.", name);
                pa_sink_input_set_volume(si, pa_cvolume_remap(&v, &e->channel_map, &si->channel_map), TRUE);
            }
        }

        if (u->restore_muted &&
            e->muted_valid) {
            pa_log_info("Restoring mute state for sink input %s.", name);
            pa_sink_input_set_mute(si, e->muted, TRUE);
        }

        if (u->restore_device &&
            e->device_valid &&
            (s = pa_namereg_get(u->core, e->device, PA_NAMEREG_SINK))) {

            pa_log_info("Restoring device for stream %s.", name);
            pa_sink_input_move_to(si, s, TRUE);
        }
    }

    for (so = pa_idxset_first(u->core->source_outputs, &idx); so; so = pa_idxset_next(u->core->source_outputs, &idx)) {
        char *n;
        pa_source *s;

        if (!(n = get_name(so->proplist, "source-output")))
            continue;

        if (strcmp(name, n)) {
            pa_xfree(n);
            continue;
        }
	pa_xfree(n);

        if (u->restore_device &&
            e->device_valid &&
            (s = pa_namereg_get(u->core, e->device, PA_NAMEREG_SOURCE))) {

            pa_log_info("Restoring device for stream %s.", name);
            pa_source_output_move_to(so, s, TRUE);
        }
    }
}

#if 0
static void dump_database(struct userdata *u) {
    datum key;

    key = gdbm_firstkey(u->gdbm_file);
    while (key.dptr) {
        datum next_key;
        struct entry *e;
        char *name;

        next_key = gdbm_nextkey(u->gdbm_file, key);

        name = pa_xstrndup(key.dptr, key.dsize);
        pa_xfree(key.dptr);

        if ((e = read_entry(u, name))) {
            char t[256];
            pa_log("name=%s", name);
            pa_log("device=%s", e->device);
            pa_log("channel_map=%s", pa_channel_map_snprint(t, sizeof(t), &e->channel_map));
            pa_log("volume=%s", pa_cvolume_snprint(t, sizeof(t), &e->volume));
            pa_log("mute=%s", pa_yes_no(e->muted));
            pa_xfree(e);
        }

        pa_xfree(name);

        key = next_key;
    }
}
#endif

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

        case SUBCOMMAND_READ: {
            datum key;

            if (!pa_tagstruct_eof(t))
                goto fail;

            key = gdbm_firstkey(u->gdbm_file);
            while (key.dptr) {
                datum next_key;
                struct entry *e;
                char *name;

                next_key = gdbm_nextkey(u->gdbm_file, key);

                name = pa_xstrndup(key.dptr, (size_t) key.dsize);
                pa_xfree(key.dptr);

                if ((e = read_entry(u, name))) {
                    pa_cvolume r;
                    pa_channel_map cm;

                    pa_tagstruct_puts(reply, name);
                    pa_tagstruct_put_channel_map(reply, (e->relative_volume_valid || e->absolute_volume_valid) ? &e->channel_map : pa_channel_map_init(&cm));
                    pa_tagstruct_put_cvolume(reply, e->absolute_volume_valid ? &e->absolute_volume : (e->relative_volume_valid ? &e->relative_volume : pa_cvolume_init(&r)));
                    pa_tagstruct_puts(reply, e->device_valid ? e->device : NULL);
                    pa_tagstruct_put_boolean(reply, e->muted_valid ? e->muted : FALSE);

                    pa_xfree(e);
                }

                pa_xfree(name);

                key = next_key;
            }

            break;
        }

        case SUBCOMMAND_WRITE: {
            uint32_t mode;
            pa_bool_t apply_immediately = FALSE;

            if (pa_tagstruct_getu32(t, &mode) < 0 ||
                pa_tagstruct_get_boolean(t, &apply_immediately) < 0)
                goto fail;

            if (mode != PA_UPDATE_MERGE &&
                mode != PA_UPDATE_REPLACE &&
                mode != PA_UPDATE_SET)
                goto fail;

            if (mode == PA_UPDATE_SET)
                clear_db(u);

            while (!pa_tagstruct_eof(t)) {
                const char *name, *device;
                pa_bool_t muted;
                struct entry entry;
                datum key, data;
                int k;

                memset(&entry, 0, sizeof(entry));
                entry.version = ENTRY_VERSION;

                if (pa_tagstruct_gets(t, &name) < 0 ||
                    pa_tagstruct_get_channel_map(t, &entry.channel_map) ||
                    pa_tagstruct_get_cvolume(t, &entry.absolute_volume) < 0 ||
                    pa_tagstruct_gets(t, &device) < 0 ||
                    pa_tagstruct_get_boolean(t, &muted) < 0)
                    goto fail;

                if (!name || !*name)
                    goto fail;

                entry.relative_volume = entry.absolute_volume;
                entry.absolute_volume_valid = entry.relative_volume_valid = entry.relative_volume.channels > 0;

                if (entry.relative_volume_valid)
                    if (!pa_cvolume_compatible_with_channel_map(&entry.relative_volume, &entry.channel_map))
                        goto fail;

                entry.muted = muted;
                entry.muted_valid = TRUE;

                if (device)
                    pa_strlcpy(entry.device, device, sizeof(entry.device));
                entry.device_valid = !!entry.device[0];

                if (entry.device_valid &&
                    !pa_namereg_is_valid_name(entry.device))
                    goto fail;

                key.dptr = (void*) name;
                key.dsize = (int) strlen(name);

                data.dptr = (void*) &entry;
                data.dsize = sizeof(entry);

                if ((k = gdbm_store(u->gdbm_file, key, data, mode == PA_UPDATE_REPLACE ? GDBM_REPLACE : GDBM_INSERT)) == 0)
                    if (apply_immediately)
                        apply_entry(u, name, &entry);
            }

            trigger_save(u);

            break;
        }

        case SUBCOMMAND_DELETE:

            while (!pa_tagstruct_eof(t)) {
                const char *name;
                datum key;

                if (pa_tagstruct_gets(t, &name) < 0)
                    goto fail;

                key.dptr = (void*) name;
                key.dsize = (int) strlen(name);

                gdbm_delete(u->gdbm_file, key);
            }

            trigger_save(u);

            break;

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
    char *fname, *fn;
    pa_sink_input *si;
    pa_source_output *so;
    uint32_t idx;
    pa_bool_t restore_device = TRUE, restore_volume = TRUE, restore_muted = TRUE;
    int gdbm_cache_size;

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
    u->gdbm_file = NULL;
    u->subscribed = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->protocol = pa_native_protocol_get(m->core);
    pa_native_protocol_install_ext(u->protocol, m, extension_cb);

    u->connection_unlink_hook_slot = pa_hook_connect(&pa_native_protocol_hooks(u->protocol)[PA_NATIVE_HOOK_CONNECTION_UNLINK], PA_HOOK_NORMAL, (pa_hook_cb_t) connection_unlink_hook_cb, u);

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK_INPUT|PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, subscribe_callback, u);

    if (restore_device) {
        u->sink_input_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_new_hook_callback, u);
        u->source_output_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) source_output_new_hook_callback, u);
    }

    if (restore_volume || restore_muted)
        u->sink_input_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_fixate_hook_callback, u);

    /* We include the host identifier in the file name because gdbm
     * files are CPU dependant, and we don't want things to go wrong
     * if we are on a multiarch system. */

    fn = pa_sprintf_malloc("stream-volumes."CANONICAL_HOST".gdbm");
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

    if (u->connection_unlink_hook_slot)
        pa_hook_slot_free(u->connection_unlink_hook_slot);

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    if (u->gdbm_file)
        gdbm_close(u->gdbm_file);

    if (u->protocol) {
        pa_native_protocol_remove_ext(u->protocol, m);
        pa_native_protocol_unref(u->protocol);
    }

    if (u->subscribed)
        pa_idxset_free(u->subscribed, NULL, NULL);

    pa_xfree(u);
}
