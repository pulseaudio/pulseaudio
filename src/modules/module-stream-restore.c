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
#include <pulsecore/protocol-native.h>
#include <pulsecore/pstream.h>
#include <pulsecore/pstream-util.h>
#include <pulsecore/database.h>

#include "module-stream-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore the volume/mute/device state of streams");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "restore_device=<Save/restore sinks/sources?> "
        "restore_volume=<Save/restore volumes?> "
        "restore_muted=<Save/restore muted states?> "
        "on_hotplug=<When new device becomes available, recheck streams?> "
        "on_rescue=<When device becomes unavailable, recheck streams?>");

#define SAVE_INTERVAL (10 * PA_USEC_PER_SEC)
#define IDENTIFICATION_PROPERTY "module-stream-restore.id"

static const char* const valid_modargs[] = {
    "restore_device",
    "restore_volume",
    "restore_muted",
    "on_hotplug",
    "on_rescue",
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
        *sink_put_hook_slot,
        *source_put_hook_slot,
        *sink_unlink_hook_slot,
        *source_unlink_hook_slot,
        *connection_unlink_hook_slot;
    pa_time_event *save_time_event;
    pa_database* database;

    pa_bool_t restore_device:1;
    pa_bool_t restore_volume:1;
    pa_bool_t restore_muted:1;
    pa_bool_t on_hotplug:1;
    pa_bool_t on_rescue:1;

    pa_native_protocol *protocol;
    pa_idxset *subscribed;
};

#define ENTRY_VERSION 3

struct entry {
    uint8_t version;
    pa_bool_t muted_valid:1, volume_valid:1, device_valid:1, card_valid:1;
    pa_bool_t muted:1;
    pa_channel_map channel_map;
    pa_cvolume volume;
    char device[PA_NAME_MAX];
    char card[PA_NAME_MAX];
} PA_GCC_PACKED;

enum {
    SUBCOMMAND_TEST,
    SUBCOMMAND_READ,
    SUBCOMMAND_WRITE,
    SUBCOMMAND_DELETE,
    SUBCOMMAND_SUBSCRIBE,
    SUBCOMMAND_EVENT
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
    pa_datum key, data;
    struct entry *e;

    pa_assert(u);
    pa_assert(name);

    key.data = (char*) name;
    key.size = strlen(name);

    pa_zero(data);

    if (!pa_database_get(u->database, &key, &data))
        goto fail;

    if (data.size != sizeof(struct entry)) {
        /* This is probably just a database upgrade, hence let's not
         * consider this more than a debug message */
        pa_log_debug("Database contains entry for stream %s of wrong size %lu != %lu. Probably due to uprade, ignoring.", name, (unsigned long) data.size, (unsigned long) sizeof(struct entry));
        goto fail;
    }

    e = (struct entry*) data.data;

    if (e->version != ENTRY_VERSION) {
        pa_log_debug("Version of database entry for stream %s doesn't match our version. Probably due to upgrade, ignoring.", name);
        goto fail;
    }

    if (!memchr(e->device, 0, sizeof(e->device))) {
        pa_log_warn("Database contains entry for stream %s with missing NUL byte in device name", name);
        goto fail;
    }

    if (!memchr(e->card, 0, sizeof(e->card))) {
        pa_log_warn("Database contains entry for stream %s with missing NUL byte in card name", name);
        goto fail;
    }

    if (e->device_valid && !pa_namereg_is_valid_name(e->device)) {
        pa_log_warn("Invalid device name stored in database for stream %s", name);
        goto fail;
    }

    if (e->card_valid && !pa_namereg_is_valid_name(e->card)) {
        pa_log_warn("Invalid card name stored in database for stream %s", name);
        goto fail;
    }

    if (e->volume_valid && !pa_channel_map_valid(&e->channel_map)) {
        pa_log_warn("Invalid channel map stored in database for stream %s", name);
        goto fail;
    }

    if (e->volume_valid && (!pa_cvolume_valid(&e->volume) || !pa_cvolume_compatible_with_channel_map(&e->volume, &e->channel_map))) {
        pa_log_warn("Invalid volume stored in database for stream %s", name);
        goto fail;
    }

    return e;

fail:

    pa_datum_free(&data);
    return NULL;
}

static void trigger_save(struct userdata *u) {
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

    u->save_time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + SAVE_INTERVAL, save_time_callback, u);
}

static pa_bool_t entries_equal(const struct entry *a, const struct entry *b) {
    pa_cvolume t;

    pa_assert(a);
    pa_assert(b);

    if (a->device_valid != b->device_valid ||
        (a->device_valid && strncmp(a->device, b->device, sizeof(a->device))))
        return FALSE;

    if (a->card_valid != b->card_valid ||
        (a->card_valid && strncmp(a->card, b->card, sizeof(a->card))))
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
    struct entry entry, *old;
    char *name;
    pa_datum key, data;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    pa_zero(entry);
    entry.version = ENTRY_VERSION;

    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        pa_sink_input *sink_input;

        if (!(sink_input = pa_idxset_get_by_index(c->sink_inputs, idx)))
            return;

        if (!(name = get_name(sink_input->proplist, "sink-input")))
            return;

        if ((old = read_entry(u, name)))
            entry = *old;

        if (sink_input->save_volume) {
            entry.channel_map = sink_input->channel_map;
            pa_sink_input_get_volume(sink_input, &entry.volume, FALSE);
            entry.volume_valid = TRUE;
        }

        if (sink_input->save_muted) {
            entry.muted = pa_sink_input_get_mute(sink_input);
            entry.muted_valid = TRUE;
        }

        if (sink_input->save_sink) {
            pa_strlcpy(entry.device, sink_input->sink->name, sizeof(entry.device));
            entry.device_valid = TRUE;

            if (sink_input->sink->card) {
                pa_strlcpy(entry.card, sink_input->sink->card->name, sizeof(entry.card));
                entry.card_valid = TRUE;
            }
        }

    } else {
        pa_source_output *source_output;

        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT);

        if (!(source_output = pa_idxset_get_by_index(c->source_outputs, idx)))
            return;

        if (!(name = get_name(source_output->proplist, "source-output")))
            return;

        if ((old = read_entry(u, name)))
            entry = *old;

        if (source_output->save_source) {
            pa_strlcpy(entry.device, source_output->source->name, sizeof(entry.device));
            entry.device_valid = source_output->save_source;

            if (source_output->source->card) {
                pa_strlcpy(entry.card, source_output->source->card->name, sizeof(entry.card));
                entry.card_valid = TRUE;
            }
        }
    }

    if (old) {

        if (entries_equal(old, &entry)) {
            pa_xfree(old);
            pa_xfree(name);
            return;
        }

        pa_xfree(old);
    }

    key.data = name;
    key.size = strlen(name);

    data.data = &entry;
    data.size = sizeof(entry);

    pa_log_info("Storing volume/mute/device for stream %s.", name);

    pa_database_set(u->database, &key, &data, TRUE);

    pa_xfree(name);

    trigger_save(u);
}

static pa_hook_result_t sink_input_new_hook_callback(pa_core *c, pa_sink_input_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);
    pa_assert(u->restore_device);

    if (!(name = get_name(new_data->proplist, "sink-input")))
        return PA_HOOK_OK;

    if (new_data->sink)
        pa_log_debug("Not restoring device for stream %s, because already set.", name);
    else if ((e = read_entry(u, name))) {
        pa_sink *s = NULL;

        if (e->device_valid)
            s = pa_namereg_get(c, e->device, PA_NAMEREG_SINK);

        if (!s && e->card_valid) {
            pa_card *card;

            if ((card = pa_namereg_get(c, e->card, PA_NAMEREG_CARD)))
                s = pa_idxset_first(card->sinks, NULL);
        }

        /* It might happen that a stream and a sink are set up at the
           same time, in which case we want to make sure we don't
           interfere with that */
        if (s && PA_SINK_IS_LINKED(pa_sink_get_state(s))) {
            pa_log_info("Restoring device for stream %s.", name);
            new_data->sink = s;
            new_data->save_sink = TRUE;
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_fixate_hook_callback(pa_core *c, pa_sink_input_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);
    pa_assert(u->restore_volume || u->restore_muted);

    if (!(name = get_name(new_data->proplist, "sink-input")))
        return PA_HOOK_OK;

    if ((e = read_entry(u, name))) {

        if (u->restore_volume && e->volume_valid) {

            if (!new_data->volume_is_set) {
                pa_cvolume v;

                pa_log_info("Restoring volume for sink input %s.", name);

                v = e->volume;
                pa_cvolume_remap(&v, &e->channel_map, &new_data->channel_map);
                pa_sink_input_new_data_set_volume(new_data, &v);

                new_data->volume_is_absolute = FALSE;
                new_data->save_volume = TRUE;
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

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);
    pa_assert(u->restore_device);

    if (new_data->direct_on_input)
        return PA_HOOK_OK;

    if (!(name = get_name(new_data->proplist, "source-output")))
        return PA_HOOK_OK;

    if (new_data->source)
        pa_log_debug("Not restoring device for stream %s, because already set", name);
    else if ((e = read_entry(u, name))) {
        pa_source *s = NULL;

        if (e->device_valid)
            s = pa_namereg_get(c, e->device, PA_NAMEREG_SOURCE);

        if (!s && e->card_valid) {
            pa_card *card;

            if ((card = pa_namereg_get(c, e->card, PA_NAMEREG_CARD)))
                s = pa_idxset_first(card->sources, NULL);
        }

        /* It might happen that a stream and a sink are set up at the
           same time, in which case we want to make sure we don't
           interfere with that */
        if (s && PA_SOURCE_IS_LINKED(pa_source_get_state(s))) {
            pa_log_info("Restoring device for stream %s.", name);
            new_data->source = s;
            new_data->save_source = TRUE;
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_put_hook_callback(pa_core *c, pa_sink *sink, struct userdata *u) {
    pa_sink_input *si;
    uint32_t idx;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);
    pa_assert(u->on_hotplug && u->restore_device);

    PA_IDXSET_FOREACH(si, c->sink_inputs, idx) {
        char *name;
        struct entry *e;

        if (si->sink == sink)
            continue;

        if (si->save_sink)
            continue;

        /* Skip this if it is already in the process of being moved
         * anyway */
        if (!si->sink)
            continue;

        /* It might happen that a stream and a sink are set up at the
           same time, in which case we want to make sure we don't
           interfere with that */
        if (!PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(si)))
            continue;

        if (!(name = get_name(si->proplist, "sink-input")))
            continue;

        if ((e = read_entry(u, name))) {
            if (e->device_valid && pa_streq(e->device, sink->name))
                pa_sink_input_move_to(si, sink, TRUE);

            pa_xfree(e);
        }

        pa_xfree(name);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t source_put_hook_callback(pa_core *c, pa_source *source, struct userdata *u) {
    pa_source_output *so;
    uint32_t idx;

    pa_assert(c);
    pa_assert(source);
    pa_assert(u);
    pa_assert(u->on_hotplug && u->restore_device);

    PA_IDXSET_FOREACH(so, c->source_outputs, idx) {
        char *name;
        struct entry *e;

        if (so->source == source)
            continue;

        if (so->save_source)
            continue;

        if (so->direct_on_input)
            continue;

        /* Skip this if it is already in the process of being moved anyway */
        if (!so->source)
            continue;

        /* It might happen that a stream and a source are set up at the
           same time, in which case we want to make sure we don't
           interfere with that */
        if (!PA_SOURCE_OUTPUT_IS_LINKED(pa_source_output_get_state(so)))
            continue;

        if (!(name = get_name(so->proplist, "source-input")))
            continue;

        if ((e = read_entry(u, name))) {
            if (e->device_valid && pa_streq(e->device, source->name))
                pa_source_output_move_to(so, source, TRUE);

            pa_xfree(e);
        }

        pa_xfree(name);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_unlink_hook_callback(pa_core *c, pa_sink *sink, struct userdata *u) {
    pa_sink_input *si;
    uint32_t idx;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);
    pa_assert(u->on_rescue && u->restore_device);

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    PA_IDXSET_FOREACH(si, sink->inputs, idx) {
        char *name;
        struct entry *e;

        if (!si->sink)
            continue;

        if (!(name = get_name(si->proplist, "sink-input")))
            continue;

        if ((e = read_entry(u, name))) {

            if (e->device_valid) {
                pa_sink *d;

                if ((d = pa_namereg_get(c, e->device, PA_NAMEREG_SINK)) &&
                    d != sink &&
                    PA_SINK_IS_LINKED(pa_sink_get_state(d)))
                    pa_sink_input_move_to(si, d, TRUE);
            }

            pa_xfree(e);
        }

        pa_xfree(name);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t source_unlink_hook_callback(pa_core *c, pa_source *source, struct userdata *u) {
    pa_source_output *so;
    uint32_t idx;

    pa_assert(c);
    pa_assert(source);
    pa_assert(u);
    pa_assert(u->on_rescue && u->restore_device);

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    PA_IDXSET_FOREACH(so, source->outputs, idx) {
        char *name;
        struct entry *e;

        if (so->direct_on_input)
            continue;

        if (!so->source)
            continue;

        if (!(name = get_name(so->proplist, "source-output")))
            continue;

        if ((e = read_entry(u, name))) {

            if (e->device_valid) {
                pa_source *d;

                if ((d = pa_namereg_get(c, e->device, PA_NAMEREG_SOURCE)) &&
                    d != source &&
                    PA_SOURCE_IS_LINKED(pa_source_get_state(d)))
                    pa_source_output_move_to(so, d, TRUE);
            }

            pa_xfree(e);
        }

        pa_xfree(name);
    }

    return PA_HOOK_OK;
}

#define EXT_VERSION 1

static void apply_entry(struct userdata *u, const char *name, struct entry *e) {
    pa_sink_input *si;
    pa_source_output *so;
    uint32_t idx;

    pa_assert(u);
    pa_assert(name);
    pa_assert(e);

    PA_IDXSET_FOREACH(si, u->core->sink_inputs, idx) {
        char *n;
        pa_sink *s;

        if (!(n = get_name(si->proplist, "sink-input")))
            continue;

        if (!pa_streq(name, n)) {
            pa_xfree(n);
            continue;
        }
        pa_xfree(n);

        if (u->restore_volume && e->volume_valid) {
            pa_cvolume v;

            v = e->volume;
            pa_log_info("Restoring volume for sink input %s.", name);
            pa_cvolume_remap(&v, &e->channel_map, &si->channel_map);
            pa_sink_input_set_volume(si, &v, TRUE, FALSE);
        }

        if (u->restore_muted && e->muted_valid) {
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

    PA_IDXSET_FOREACH(so, u->core->source_outputs, idx) {
        char *n;
        pa_source *s;

        if (!(n = get_name(so->proplist, "source-output")))
            continue;

        if (!pa_streq(name, n)) {
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
    pa_datum key;
    pa_bool_t done;

    done = !pa_database_first(u->database, &key, NULL);

    while (!done) {
        pa_datum next_key;
        struct entry *e;
        char *name;

        done = !pa_database_next(u->database, &key, &next_key, NULL);

        name = pa_xstrndup(key.data, key.size);
        pa_datum_free(&key);

        if ((e = read_entry(u, name))) {
            char t[256];
            pa_log("name=%s", name);
            pa_log("device=%s %s", e->device, pa_yes_no(e->device_valid));
            pa_log("channel_map=%s", pa_channel_map_snprint(t, sizeof(t), &e->channel_map));
            pa_log("volume=%s %s", pa_cvolume_snprint(t, sizeof(t), &e->volume), pa_yes_no(e->volume_valid));
            pa_log("mute=%s %s", pa_yes_no(e->muted), pa_yes_no(e->volume_valid));
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
            pa_datum key;
            pa_bool_t done;

            if (!pa_tagstruct_eof(t))
                goto fail;

            done = !pa_database_first(u->database, &key, NULL);

            while (!done) {
                pa_datum next_key;
                struct entry *e;
                char *name;

                done = !pa_database_next(u->database, &key, &next_key, NULL);

                name = pa_xstrndup(key.data, key.size);
                pa_datum_free(&key);

                if ((e = read_entry(u, name))) {
                    pa_cvolume r;
                    pa_channel_map cm;

                    pa_tagstruct_puts(reply, name);
                    pa_tagstruct_put_channel_map(reply, e->volume_valid ? &e->channel_map : pa_channel_map_init(&cm));
                    pa_tagstruct_put_cvolume(reply, e->volume_valid ? &e->volume : pa_cvolume_init(&r));
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
                pa_database_clear(u->database);

            while (!pa_tagstruct_eof(t)) {
                const char *name, *device;
                pa_bool_t muted;
                struct entry entry;
                pa_datum key, data;

                pa_zero(entry);
                entry.version = ENTRY_VERSION;

                if (pa_tagstruct_gets(t, &name) < 0 ||
                    pa_tagstruct_get_channel_map(t, &entry.channel_map) ||
                    pa_tagstruct_get_cvolume(t, &entry.volume) < 0 ||
                    pa_tagstruct_gets(t, &device) < 0 ||
                    pa_tagstruct_get_boolean(t, &muted) < 0)
                    goto fail;

                if (!name || !*name)
                    goto fail;

                entry.volume_valid = entry.volume.channels > 0;

                if (entry.volume_valid)
                    if (!pa_cvolume_compatible_with_channel_map(&entry.volume, &entry.channel_map))
                        goto fail;

                entry.muted = muted;
                entry.muted_valid = TRUE;

                if (device)
                    pa_strlcpy(entry.device, device, sizeof(entry.device));
                entry.device_valid = !!entry.device[0];

                if (entry.device_valid &&
                    !pa_namereg_is_valid_name(entry.device))
                    goto fail;

                key.data = (char*) name;
                key.size = strlen(name);

                data.data = &entry;
                data.size = sizeof(entry);

                pa_log_debug("Client %s changes entry %s.",
                             pa_strnull(pa_proplist_gets(pa_native_connection_get_client(c)->proplist, PA_PROP_APPLICATION_PROCESS_BINARY)),
                             name);

                if (pa_database_set(u->database, &key, &data, mode == PA_UPDATE_REPLACE) == 0)
                    if (apply_immediately)
                        apply_entry(u, name, &entry);
            }

            trigger_save(u);

            break;
        }

        case SUBCOMMAND_DELETE:

            while (!pa_tagstruct_eof(t)) {
                const char *name;
                pa_datum key;

                if (pa_tagstruct_gets(t, &name) < 0)
                    goto fail;

                key.data = (char*) name;
                key.size = strlen(name);

                pa_database_unset(u->database, &key);
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
    char *fname;
    pa_sink_input *si;
    pa_source_output *so;
    uint32_t idx;
    pa_bool_t restore_device = TRUE, restore_volume = TRUE, restore_muted = TRUE, on_hotplug = TRUE, on_rescue = TRUE;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "restore_device", &restore_device) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_volume", &restore_volume) < 0 ||
        pa_modargs_get_value_boolean(ma, "restore_muted", &restore_muted) < 0 ||
        pa_modargs_get_value_boolean(ma, "on_hotplug", &on_hotplug) < 0 ||
        pa_modargs_get_value_boolean(ma, "on_rescue", &on_rescue) < 0) {
        pa_log("restore_device=, restore_volume=, restore_muted=, on_hotplug= and on_rescue= expect boolean arguments");
        goto fail;
    }

    if (!restore_muted && !restore_volume && !restore_device)
        pa_log_warn("Neither restoring volume, nor restoring muted, nor restoring device enabled!");

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->restore_device = restore_device;
    u->restore_volume = restore_volume;
    u->restore_muted = restore_muted;
    u->on_hotplug = on_hotplug;
    u->on_rescue = on_rescue;
    u->subscribed = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->protocol = pa_native_protocol_get(m->core);
    pa_native_protocol_install_ext(u->protocol, m, extension_cb);

    u->connection_unlink_hook_slot = pa_hook_connect(&pa_native_protocol_hooks(u->protocol)[PA_NATIVE_HOOK_CONNECTION_UNLINK], PA_HOOK_NORMAL, (pa_hook_cb_t) connection_unlink_hook_cb, u);

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK_INPUT|PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, subscribe_callback, u);

    if (restore_device) {
        /* A little bit earlier than module-intended-roles ... */
        u->sink_input_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_new_hook_callback, u);
        u->source_output_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) source_output_new_hook_callback, u);
    }

    if (restore_device && on_hotplug) {
        /* A little bit earlier than module-intended-roles ... */
        u->sink_put_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_put_hook_callback, u);
        u->source_put_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_LATE, (pa_hook_cb_t) source_put_hook_callback, u);
    }

    if (restore_device && on_rescue) {
        /* A little bit earlier than module-intended-roles, module-rescue-streams, ... */
        u->sink_unlink_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_unlink_hook_callback, u);
        u->source_unlink_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) source_unlink_hook_callback, u);
    }

    if (restore_volume || restore_muted)
        u->sink_input_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_fixate_hook_callback, u);

    if (!(fname = pa_state_path("stream-volumes", TRUE)))
        goto fail;

    if (!(u->database = pa_database_open(fname, TRUE))) {
        pa_log("Failed to open volume database '%s': %s", fname, pa_cstrerror(errno));
        pa_xfree(fname);
        goto fail;
    }

    pa_log_info("Sucessfully opened database file '%s'.", fname);
    pa_xfree(fname);

    PA_IDXSET_FOREACH(si, m->core->sink_inputs, idx)
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, si->index, u);

    PA_IDXSET_FOREACH(so, m->core->source_outputs, idx)
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

    if (u->sink_put_hook_slot)
        pa_hook_slot_free(u->sink_put_hook_slot);
    if (u->source_put_hook_slot)
        pa_hook_slot_free(u->source_put_hook_slot);

    if (u->sink_unlink_hook_slot)
        pa_hook_slot_free(u->sink_unlink_hook_slot);
    if (u->source_unlink_hook_slot)
        pa_hook_slot_free(u->source_unlink_hook_slot);

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
