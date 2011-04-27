/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering
  Copyright 2009 Tanu Kaskinen

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

#ifdef HAVE_DBUS
#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>
#endif

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

#ifdef HAVE_DBUS
    pa_dbus_protocol *dbus_protocol;
    pa_hashmap *dbus_entries;
    uint32_t next_index; /* For generating object paths for entries. */
#endif
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

static struct entry *read_entry(struct userdata *u, const char *name);
static void apply_entry(struct userdata *u, const char *name, struct entry *e);
static void trigger_save(struct userdata *u);

#ifdef HAVE_DBUS

#define OBJECT_PATH "/org/pulseaudio/stream_restore1"
#define ENTRY_OBJECT_NAME "entry"
#define INTERFACE_STREAM_RESTORE "org.PulseAudio.Ext.StreamRestore1"
#define INTERFACE_ENTRY INTERFACE_STREAM_RESTORE ".RestoreEntry"

#define DBUS_INTERFACE_REVISION 0

struct dbus_entry {
    struct userdata *userdata;

    char *entry_name;
    uint32_t index;
    char *object_path;
};

static void handle_get_interface_revision(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_entries(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void handle_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void handle_add_entry(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_entry_by_name(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void handle_entry_get_index(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_entry_get_name(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_entry_get_device(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_entry_set_device(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata);
static void handle_entry_get_volume(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_entry_set_volume(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata);
static void handle_entry_get_mute(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_entry_set_mute(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata);

static void handle_entry_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void handle_entry_remove(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum property_handler_index {
    PROPERTY_HANDLER_INTERFACE_REVISION,
    PROPERTY_HANDLER_ENTRIES,
    PROPERTY_HANDLER_MAX
};

enum entry_property_handler_index {
    ENTRY_PROPERTY_HANDLER_INDEX,
    ENTRY_PROPERTY_HANDLER_NAME,
    ENTRY_PROPERTY_HANDLER_DEVICE,
    ENTRY_PROPERTY_HANDLER_VOLUME,
    ENTRY_PROPERTY_HANDLER_MUTE,
    ENTRY_PROPERTY_HANDLER_MAX
};

static pa_dbus_property_handler property_handlers[PROPERTY_HANDLER_MAX] = {
    [PROPERTY_HANDLER_INTERFACE_REVISION] = { .property_name = "InterfaceRevision", .type = "u",  .get_cb = handle_get_interface_revision, .set_cb = NULL },
    [PROPERTY_HANDLER_ENTRIES]            = { .property_name = "Entries",           .type = "ao", .get_cb = handle_get_entries,            .set_cb = NULL }
};

static pa_dbus_property_handler entry_property_handlers[ENTRY_PROPERTY_HANDLER_MAX] = {
    [ENTRY_PROPERTY_HANDLER_INDEX]    = { .property_name = "Index",   .type = "u",     .get_cb = handle_entry_get_index,    .set_cb = NULL },
    [ENTRY_PROPERTY_HANDLER_NAME]     = { .property_name = "Name",    .type = "s",     .get_cb = handle_entry_get_name,     .set_cb = NULL },
    [ENTRY_PROPERTY_HANDLER_DEVICE]   = { .property_name = "Device",  .type = "s",     .get_cb = handle_entry_get_device,   .set_cb = handle_entry_set_device },
    [ENTRY_PROPERTY_HANDLER_VOLUME]   = { .property_name = "Volume",  .type = "a(uu)", .get_cb = handle_entry_get_volume,   .set_cb = handle_entry_set_volume },
    [ENTRY_PROPERTY_HANDLER_MUTE]     = { .property_name = "Mute",    .type = "b",     .get_cb = handle_entry_get_mute,     .set_cb = handle_entry_set_mute }
};

enum method_handler_index {
    METHOD_HANDLER_ADD_ENTRY,
    METHOD_HANDLER_GET_ENTRY_BY_NAME,
    METHOD_HANDLER_MAX
};

enum entry_method_handler_index {
    ENTRY_METHOD_HANDLER_REMOVE,
    ENTRY_METHOD_HANDLER_MAX
};

static pa_dbus_arg_info add_entry_args[] = { { "name",   "s",     "in" },
                                             { "device", "s",     "in" },
                                             { "volume", "a(uu)", "in" },
                                             { "mute",   "b",     "in" },
                                             { "entry",  "o",     "out" } };
static pa_dbus_arg_info get_entry_by_name_args[] = { { "name", "s", "in" }, { "entry", "o", "out" } };

static pa_dbus_method_handler method_handlers[METHOD_HANDLER_MAX] = {
    [METHOD_HANDLER_ADD_ENTRY] = {
        .method_name = "AddEntry",
        .arguments = add_entry_args,
        .n_arguments = sizeof(add_entry_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = handle_add_entry },
    [METHOD_HANDLER_GET_ENTRY_BY_NAME] = {
        .method_name = "GetEntryByName",
        .arguments = get_entry_by_name_args,
        .n_arguments = sizeof(get_entry_by_name_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = handle_get_entry_by_name }
};

static pa_dbus_method_handler entry_method_handlers[ENTRY_METHOD_HANDLER_MAX] = {
    [ENTRY_METHOD_HANDLER_REMOVE] = {
        .method_name = "Remove",
        .arguments = NULL,
        .n_arguments = 0,
        .receive_cb = handle_entry_remove }
};

enum signal_index {
    SIGNAL_NEW_ENTRY,
    SIGNAL_ENTRY_REMOVED,
    SIGNAL_MAX
};

enum entry_signal_index {
    ENTRY_SIGNAL_DEVICE_UPDATED,
    ENTRY_SIGNAL_VOLUME_UPDATED,
    ENTRY_SIGNAL_MUTE_UPDATED,
    ENTRY_SIGNAL_MAX
};

static pa_dbus_arg_info new_entry_args[]     = { { "entry", "o", NULL } };
static pa_dbus_arg_info entry_removed_args[] = { { "entry", "o", NULL } };

static pa_dbus_arg_info entry_device_updated_args[] = { { "device", "s",     NULL } };
static pa_dbus_arg_info entry_volume_updated_args[] = { { "volume", "a(uu)", NULL } };
static pa_dbus_arg_info entry_mute_updated_args[]   = { { "muted",  "b",     NULL } };

static pa_dbus_signal_info signals[SIGNAL_MAX] = {
    [SIGNAL_NEW_ENTRY]     = { .name = "NewEntry",     .arguments = new_entry_args,     .n_arguments = 1 },
    [SIGNAL_ENTRY_REMOVED] = { .name = "EntryRemoved", .arguments = entry_removed_args, .n_arguments = 1 }
};

static pa_dbus_signal_info entry_signals[ENTRY_SIGNAL_MAX] = {
    [ENTRY_SIGNAL_DEVICE_UPDATED] = { .name = "DeviceUpdated", .arguments = entry_device_updated_args, .n_arguments = 1 },
    [ENTRY_SIGNAL_VOLUME_UPDATED] = { .name = "VolumeUpdated", .arguments = entry_volume_updated_args, .n_arguments = 1 },
    [ENTRY_SIGNAL_MUTE_UPDATED]   = { .name = "MuteUpdated",   .arguments = entry_mute_updated_args,   .n_arguments = 1 }
};

static pa_dbus_interface_info stream_restore_interface_info = {
    .name = INTERFACE_STREAM_RESTORE,
    .method_handlers = method_handlers,
    .n_method_handlers = METHOD_HANDLER_MAX,
    .property_handlers = property_handlers,
    .n_property_handlers = PROPERTY_HANDLER_MAX,
    .get_all_properties_cb = handle_get_all,
    .signals = signals,
    .n_signals = SIGNAL_MAX
};

static pa_dbus_interface_info entry_interface_info = {
    .name = INTERFACE_ENTRY,
    .method_handlers = entry_method_handlers,
    .n_method_handlers = ENTRY_METHOD_HANDLER_MAX,
    .property_handlers = entry_property_handlers,
    .n_property_handlers = ENTRY_PROPERTY_HANDLER_MAX,
    .get_all_properties_cb = handle_entry_get_all,
    .signals = entry_signals,
    .n_signals = ENTRY_SIGNAL_MAX
};

static struct dbus_entry *dbus_entry_new(struct userdata *u, const char *entry_name) {
    struct dbus_entry *de;

    pa_assert(u);
    pa_assert(entry_name);
    pa_assert(*entry_name);

    de = pa_xnew(struct dbus_entry, 1);
    de->userdata = u;
    de->entry_name = pa_xstrdup(entry_name);
    de->index = u->next_index++;
    de->object_path = pa_sprintf_malloc("%s/%s%u", OBJECT_PATH, ENTRY_OBJECT_NAME, de->index);

    pa_assert_se(pa_dbus_protocol_add_interface(u->dbus_protocol, de->object_path, &entry_interface_info, de) >= 0);

    return de;
}

static void dbus_entry_free(struct dbus_entry *de) {
    pa_assert(de);

    pa_assert_se(pa_dbus_protocol_remove_interface(de->userdata->dbus_protocol, de->object_path, entry_interface_info.name) >= 0);

    pa_xfree(de->entry_name);
    pa_xfree(de->object_path);
}

/* Reads an array [(UInt32, UInt32)] from the iterator. The struct items are
 * are a channel position and a volume value, respectively. The result is
 * stored in the map and vol arguments. The iterator must point to a "a(uu)"
 * element. If the data is invalid, an error reply is sent and a negative
 * number is returned. In case of a failure we make no guarantees about the
 * state of map and vol. In case of an empty array the channels field of both
 * map and vol are set to 0. This function calls dbus_message_iter_next(iter)
 * before returning. */
static int get_volume_arg(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, pa_channel_map *map, pa_cvolume *vol) {
    DBusMessageIter array_iter;
    DBusMessageIter struct_iter;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(iter);
    pa_assert(pa_streq(dbus_message_iter_get_signature(iter), "a(uu)"));
    pa_assert(map);
    pa_assert(vol);

    pa_channel_map_init(map);
    pa_cvolume_init(vol);

    map->channels = 0;
    vol->channels = 0;

    dbus_message_iter_recurse(iter, &array_iter);

    while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID) {
        dbus_uint32_t chan_pos;
        dbus_uint32_t chan_vol;

        dbus_message_iter_recurse(&array_iter, &struct_iter);

        dbus_message_iter_get_basic(&struct_iter, &chan_pos);

        if (chan_pos >= PA_CHANNEL_POSITION_MAX) {
            pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Invalid channel position: %u", chan_pos);
            return -1;
        }

        pa_assert_se(dbus_message_iter_next(&struct_iter));
        dbus_message_iter_get_basic(&struct_iter, &chan_vol);

        if (!PA_VOLUME_IS_VALID(chan_vol)) {
            pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Invalid volume: %u", chan_vol);
            return -1;
        }

        if (map->channels < PA_CHANNELS_MAX) {
            map->map[map->channels] = chan_pos;
            vol->values[map->channels] = chan_vol;
        }
        ++map->channels;
        ++vol->channels;

        dbus_message_iter_next(&array_iter);
    }

    if (map->channels > PA_CHANNELS_MAX) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Too many channels: %u. The maximum is %u.", map->channels, PA_CHANNELS_MAX);
        return -1;
    }

    dbus_message_iter_next(iter);

    return 0;
}

static void append_volume(DBusMessageIter *iter, struct entry *e) {
    DBusMessageIter array_iter;
    DBusMessageIter struct_iter;
    unsigned i;

    pa_assert(iter);
    pa_assert(e);

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "(uu)", &array_iter));

    if (!e->volume_valid) {
        pa_assert_se(dbus_message_iter_close_container(iter, &array_iter));
        return;
    }

    for (i = 0; i < e->channel_map.channels; ++i) {
        pa_assert_se(dbus_message_iter_open_container(&array_iter, DBUS_TYPE_STRUCT, NULL, &struct_iter));

        pa_assert_se(dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT32, &e->channel_map.map[i]));
        pa_assert_se(dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT32, &e->volume.values[i]));

        pa_assert_se(dbus_message_iter_close_container(&array_iter, &struct_iter));
    }

    pa_assert_se(dbus_message_iter_close_container(iter, &array_iter));
}

static void append_volume_variant(DBusMessageIter *iter, struct entry *e) {
    DBusMessageIter variant_iter;

    pa_assert(iter);
    pa_assert(e);

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a(uu)", &variant_iter));

    append_volume(&variant_iter, e);

    pa_assert_se(dbus_message_iter_close_container(iter, &variant_iter));
}

static void send_new_entry_signal(struct dbus_entry *entry) {
    DBusMessage *signal_msg;

    pa_assert(entry);

    pa_assert_se(signal_msg = dbus_message_new_signal(OBJECT_PATH, INTERFACE_STREAM_RESTORE, signals[SIGNAL_NEW_ENTRY].name));
    pa_assert_se(dbus_message_append_args(signal_msg, DBUS_TYPE_OBJECT_PATH, &entry->object_path, DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(entry->userdata->dbus_protocol, signal_msg);
    dbus_message_unref(signal_msg);
}

static void send_entry_removed_signal(struct dbus_entry *entry) {
    DBusMessage *signal_msg;

    pa_assert(entry);

    pa_assert_se(signal_msg = dbus_message_new_signal(OBJECT_PATH, INTERFACE_STREAM_RESTORE, signals[SIGNAL_ENTRY_REMOVED].name));
    pa_assert_se(dbus_message_append_args(signal_msg, DBUS_TYPE_OBJECT_PATH, &entry->object_path, DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(entry->userdata->dbus_protocol, signal_msg);
    dbus_message_unref(signal_msg);
}

static void send_device_updated_signal(struct dbus_entry *de, struct entry *e) {
    DBusMessage *signal_msg;
    const char *device;

    pa_assert(de);
    pa_assert(e);

    device = e->device_valid ? e->device : "";

    pa_assert_se(signal_msg = dbus_message_new_signal(de->object_path, INTERFACE_ENTRY, entry_signals[ENTRY_SIGNAL_DEVICE_UPDATED].name));
    pa_assert_se(dbus_message_append_args(signal_msg, DBUS_TYPE_STRING, &device, DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(de->userdata->dbus_protocol, signal_msg);
    dbus_message_unref(signal_msg);
}

static void send_volume_updated_signal(struct dbus_entry *de, struct entry *e) {
    DBusMessage *signal_msg;
    DBusMessageIter msg_iter;

    pa_assert(de);
    pa_assert(e);

    pa_assert_se(signal_msg = dbus_message_new_signal(de->object_path, INTERFACE_ENTRY, entry_signals[ENTRY_SIGNAL_VOLUME_UPDATED].name));
    dbus_message_iter_init_append(signal_msg, &msg_iter);
    append_volume(&msg_iter, e);
    pa_dbus_protocol_send_signal(de->userdata->dbus_protocol, signal_msg);
    dbus_message_unref(signal_msg);
}

static void send_mute_updated_signal(struct dbus_entry *de, struct entry *e) {
    DBusMessage *signal_msg;
    dbus_bool_t muted;

    pa_assert(de);
    pa_assert(e);

    pa_assert(e->muted_valid);

    muted = e->muted;

    pa_assert_se(signal_msg = dbus_message_new_signal(de->object_path, INTERFACE_ENTRY, entry_signals[ENTRY_SIGNAL_MUTE_UPDATED].name));
    pa_assert_se(dbus_message_append_args(signal_msg, DBUS_TYPE_BOOLEAN, &muted, DBUS_TYPE_INVALID));
    pa_dbus_protocol_send_signal(de->userdata->dbus_protocol, signal_msg);
    dbus_message_unref(signal_msg);
}

static void handle_get_interface_revision(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    dbus_uint32_t interface_revision = DBUS_INTERFACE_REVISION;

    pa_assert(conn);
    pa_assert(msg);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &interface_revision);
}

/* The caller frees the array, but not the strings. */
static const char **get_entries(struct userdata *u, unsigned *n) {
    const char **entries;
    unsigned i = 0;
    void *state = NULL;
    struct dbus_entry *de;

    pa_assert(u);
    pa_assert(n);

    *n = pa_hashmap_size(u->dbus_entries);

    if (*n == 0)
        return NULL;

    entries = pa_xnew(const char *, *n);

    PA_HASHMAP_FOREACH(de, u->dbus_entries, state)
        entries[i++] = de->object_path;

    return entries;
}

static void handle_get_entries(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct userdata *u = userdata;
    const char **entries;
    unsigned n;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    entries = get_entries(u, &n);

    pa_dbus_send_basic_array_variant_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, entries, n);

    pa_xfree(entries);
}

static void handle_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct userdata *u = userdata;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter;
    DBusMessageIter dict_iter;
    dbus_uint32_t interface_revision;
    const char **entries;
    unsigned n_entries;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    interface_revision = DBUS_INTERFACE_REVISION;
    entries = get_entries(u, &n_entries);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));

    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    pa_dbus_append_basic_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_INTERFACE_REVISION].property_name, DBUS_TYPE_UINT32, &interface_revision);
    pa_dbus_append_basic_array_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_ENTRIES].property_name, DBUS_TYPE_OBJECT_PATH, entries, n_entries);

    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));

    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_message_unref(reply);

    pa_xfree(entries);
}

static void handle_add_entry(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct userdata *u = userdata;
    DBusMessageIter msg_iter;
    const char *name = NULL;
    const char *device = NULL;
    pa_channel_map map;
    pa_cvolume vol;
    dbus_bool_t muted = FALSE;
    dbus_bool_t apply_immediately = FALSE;
    pa_datum key;
    pa_datum value;
    struct dbus_entry *dbus_entry = NULL;
    struct entry *e = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    pa_assert_se(dbus_message_iter_init(msg, &msg_iter));
    dbus_message_iter_get_basic(&msg_iter, &name);

    pa_assert_se(dbus_message_iter_next(&msg_iter));
    dbus_message_iter_get_basic(&msg_iter, &device);

    pa_assert_se(dbus_message_iter_next(&msg_iter));
    if (get_volume_arg(conn, msg, &msg_iter, &map, &vol) < 0)
        return;

    dbus_message_iter_get_basic(&msg_iter, &muted);

    pa_assert_se(dbus_message_iter_next(&msg_iter));
    dbus_message_iter_get_basic(&msg_iter, &apply_immediately);

    if (!*name) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "An empty string was given as the entry name.");
        return;
    }

    if ((dbus_entry = pa_hashmap_get(u->dbus_entries, name))) {
        pa_bool_t mute_updated = FALSE;
        pa_bool_t volume_updated = FALSE;
        pa_bool_t device_updated = FALSE;

        pa_assert_se(e = read_entry(u, name));
        mute_updated = e->muted != muted;
        e->muted = muted;
        e->muted_valid = TRUE;

        volume_updated = (e->volume_valid != !!map.channels) || !pa_cvolume_equal(&e->volume, &vol);
        e->volume = vol;
        e->channel_map = map;
        e->volume_valid = !!map.channels;

        device_updated = (e->device_valid != !!device[0]) || !pa_streq(e->device, device);
        pa_strlcpy(e->device, device, sizeof(e->device));
        e->device_valid = !!device[0];

        if (mute_updated)
            send_mute_updated_signal(dbus_entry, e);
        if (volume_updated)
            send_volume_updated_signal(dbus_entry, e);
        if (device_updated)
            send_device_updated_signal(dbus_entry, e);

    } else {
        dbus_entry = dbus_entry_new(u, name);
        pa_assert_se(pa_hashmap_put(u->dbus_entries, dbus_entry->entry_name, dbus_entry) == 0);

        e = pa_xnew0(struct entry, 1);
        e->version = ENTRY_VERSION;
        e->muted_valid = TRUE;
        e->volume_valid = !!map.channels;
        e->device_valid = !!device[0];
        e->muted = muted;
        e->volume = vol;
        e->channel_map = map;
        pa_strlcpy(e->device, device, sizeof(e->device));

        send_new_entry_signal(dbus_entry);
    }

    key.data = (char *) name;
    key.size = strlen(name);

    value.data = e;
    value.size = sizeof(struct entry);

    pa_assert_se(pa_database_set(u->database, &key, &value, TRUE) == 0);
    if (apply_immediately)
        apply_entry(u, name, e);

    trigger_save(u);

    pa_dbus_send_empty_reply(conn, msg);

    pa_xfree(e);
}

static void handle_get_entry_by_name(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct userdata *u = userdata;
    const char *name;
    struct dbus_entry *de;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    pa_assert_se(dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID));

    if (!(de = pa_hashmap_get(u->dbus_entries, name))) {
        pa_dbus_send_error(conn, msg, PA_DBUS_ERROR_NOT_FOUND, "No such stream restore entry.");
        return;
    }

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &de->object_path);
}

static void handle_entry_get_index(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct dbus_entry *de = userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(de);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &de->index);
}

static void handle_entry_get_name(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct dbus_entry *de = userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(de);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_STRING, &de->entry_name);
}

static void handle_entry_get_device(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct dbus_entry *de = userdata;
    struct entry *e;
    const char *device;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(de);

    pa_assert_se(e = read_entry(de->userdata, de->entry_name));

    device = e->device_valid ? e->device : "";

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_STRING, &device);

    pa_xfree(e);
}

static void handle_entry_set_device(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata) {
    struct dbus_entry *de = userdata;
    const char *device;
    struct entry *e;
    pa_bool_t updated;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(iter);
    pa_assert(de);

    dbus_message_iter_get_basic(iter, &device);

    pa_assert_se(e = read_entry(de->userdata, de->entry_name));

    updated = (e->device_valid != !!device[0]) || !pa_streq(e->device, device);

    if (updated) {
        pa_datum key;
        pa_datum value;

        pa_strlcpy(e->device, device, sizeof(e->device));
        e->device_valid = !!device[0];

        key.data = de->entry_name;
        key.size = strlen(de->entry_name);
        value.data = e;
        value.size = sizeof(struct entry);
        pa_assert_se(pa_database_set(de->userdata->database, &key, &value, TRUE) == 0);

        apply_entry(de->userdata, de->entry_name, e);
        send_device_updated_signal(de, e);
        trigger_save(de->userdata);
    }

    pa_dbus_send_empty_reply(conn, msg);

    pa_xfree(e);
}

static void handle_entry_get_volume(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct dbus_entry *de = userdata;
    DBusMessage *reply;
    DBusMessageIter msg_iter;
    struct entry *e;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(de);

    pa_assert_se(e = read_entry(de->userdata, de->entry_name));

    pa_assert_se(reply = dbus_message_new_method_return(msg));

    dbus_message_iter_init_append(reply, &msg_iter);
    append_volume_variant(&msg_iter, e);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    pa_xfree(e);
}

static void handle_entry_set_volume(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata) {
    struct dbus_entry *de = userdata;
    pa_channel_map map;
    pa_cvolume vol;
    struct entry *e = NULL;
    pa_bool_t updated = FALSE;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(iter);
    pa_assert(de);

    if (get_volume_arg(conn, msg, iter, &map, &vol) < 0)
        return;

    pa_assert_se(e = read_entry(de->userdata, de->entry_name));

    updated = (e->volume_valid != !!map.channels) || !pa_cvolume_equal(&e->volume, &vol);

    if (updated) {
        pa_datum key;
        pa_datum value;

        e->volume = vol;
        e->channel_map = map;
        e->volume_valid = !!map.channels;

        key.data = de->entry_name;
        key.size = strlen(de->entry_name);
        value.data = e;
        value.size = sizeof(struct entry);
        pa_assert_se(pa_database_set(de->userdata->database, &key, &value, TRUE) == 0);

        apply_entry(de->userdata, de->entry_name, e);
        send_volume_updated_signal(de, e);
        trigger_save(de->userdata);
    }

    pa_dbus_send_empty_reply(conn, msg);

    pa_xfree(e);
}

static void handle_entry_get_mute(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct dbus_entry *de = userdata;
    struct entry *e;
    dbus_bool_t mute;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(de);

    pa_assert_se(e = read_entry(de->userdata, de->entry_name));

    mute = e->muted_valid ? e->muted : FALSE;

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_BOOLEAN, &mute);

    pa_xfree(e);
}

static void handle_entry_set_mute(DBusConnection *conn, DBusMessage *msg, DBusMessageIter *iter, void *userdata) {
    struct dbus_entry *de = userdata;
    dbus_bool_t mute;
    struct entry *e;
    pa_bool_t updated;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(iter);
    pa_assert(de);

    dbus_message_iter_get_basic(iter, &mute);

    pa_assert_se(e = read_entry(de->userdata, de->entry_name));

    updated = !e->muted_valid || e->muted != mute;

    if (updated) {
        pa_datum key;
        pa_datum value;

        e->muted = mute;
        e->muted_valid = TRUE;

        key.data = de->entry_name;
        key.size = strlen(de->entry_name);
        value.data = e;
        value.size = sizeof(struct entry);
        pa_assert_se(pa_database_set(de->userdata->database, &key, &value, TRUE) == 0);

        apply_entry(de->userdata, de->entry_name, e);
        send_mute_updated_signal(de, e);
        trigger_save(de->userdata);
    }

    pa_dbus_send_empty_reply(conn, msg);

    pa_xfree(e);
}

static void handle_entry_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct dbus_entry *de = userdata;
    struct entry *e;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter;
    DBusMessageIter dict_iter;
    DBusMessageIter dict_entry_iter;
    const char *device;
    dbus_bool_t mute;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(de);

    pa_assert_se(e = read_entry(de->userdata, de->entry_name));

    device = e->device_valid ? e->device : "";
    mute = e->muted_valid ? e->muted : FALSE;

    pa_assert_se((reply = dbus_message_new_method_return(msg)));

    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    pa_dbus_append_basic_variant_dict_entry(&dict_iter, entry_property_handlers[ENTRY_PROPERTY_HANDLER_INDEX].property_name, DBUS_TYPE_UINT32, &de->index);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, entry_property_handlers[ENTRY_PROPERTY_HANDLER_NAME].property_name, DBUS_TYPE_STRING, &de->entry_name);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, entry_property_handlers[ENTRY_PROPERTY_HANDLER_DEVICE].property_name, DBUS_TYPE_STRING, &device);

    pa_assert_se(dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry_iter));

    pa_assert_se(dbus_message_iter_append_basic(&dict_entry_iter, DBUS_TYPE_STRING, &entry_property_handlers[ENTRY_PROPERTY_HANDLER_VOLUME].property_name));
    append_volume_variant(&dict_entry_iter, e);

    pa_assert_se(dbus_message_iter_close_container(&dict_iter, &dict_entry_iter));

    pa_dbus_append_basic_variant_dict_entry(&dict_iter, entry_property_handlers[ENTRY_PROPERTY_HANDLER_MUTE].property_name, DBUS_TYPE_BOOLEAN, &mute);

    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));

    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_message_unref(reply);

    pa_xfree(e);
}

static void handle_entry_remove(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct dbus_entry *de = userdata;
    pa_datum key;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(de);

    key.data = de->entry_name;
    key.size = strlen(de->entry_name);

    pa_assert_se(pa_database_unset(de->userdata->database, &key) == 0);

    send_entry_removed_signal(de);
    trigger_save(de->userdata);

    pa_assert_se(pa_hashmap_remove(de->userdata->dbus_entries, de->entry_name));
    dbus_entry_free(de);

    pa_dbus_send_empty_reply(conn, msg);
}

#endif /* HAVE_DBUS */

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

static struct entry *read_entry(struct userdata *u, const char *name) {
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
    struct entry entry, *old = NULL;
    char *name = NULL;
    pa_datum key, data;

    /* These are only used when D-Bus is enabled, but in order to reduce ifdef
     * clutter these are defined here unconditionally. */
    pa_bool_t created_new_entry = TRUE;
    pa_bool_t device_updated = FALSE;
    pa_bool_t volume_updated = FALSE;
    pa_bool_t mute_updated = FALSE;

#ifdef HAVE_DBUS
    struct dbus_entry *de = NULL;
#endif

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

        if ((old = read_entry(u, name))) {
            entry = *old;
            created_new_entry = FALSE;
        }

        if (sink_input->save_volume && pa_sink_input_is_volume_readable(sink_input)) {
            pa_assert(sink_input->volume_writable);

            entry.channel_map = sink_input->channel_map;
            pa_sink_input_get_volume(sink_input, &entry.volume, FALSE);
            entry.volume_valid = TRUE;

            volume_updated = !created_new_entry
                             && (!old->volume_valid
                                 || !pa_channel_map_equal(&entry.channel_map, &old->channel_map)
                                 || !pa_cvolume_equal(&entry.volume, &old->volume));
        }

        if (sink_input->save_muted) {
            entry.muted = pa_sink_input_get_mute(sink_input);
            entry.muted_valid = TRUE;

            mute_updated = !created_new_entry && (!old->muted_valid || entry.muted != old->muted);
        }

        if (sink_input->save_sink) {
            pa_strlcpy(entry.device, sink_input->sink->name, sizeof(entry.device));
            entry.device_valid = TRUE;

            device_updated = !created_new_entry && (!old->device_valid || !pa_streq(entry.device, old->device));
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

        if ((old = read_entry(u, name))) {
            entry = *old;
            created_new_entry = FALSE;
        }

        if (source_output->save_source) {
            pa_strlcpy(entry.device, source_output->source->name, sizeof(entry.device));
            entry.device_valid = source_output->save_source;

            device_updated = !created_new_entry && (!old->device_valid || !pa_streq(entry.device, old->device));

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

#ifdef HAVE_DBUS
    if (created_new_entry) {
        de = dbus_entry_new(u, name);
        pa_assert_se(pa_hashmap_put(u->dbus_entries, de->entry_name, de) == 0);
        send_new_entry_signal(de);
    } else {
        pa_assert_se(de = pa_hashmap_get(u->dbus_entries, name));

        if (device_updated)
            send_device_updated_signal(de, &entry);
        if (volume_updated)
            send_volume_updated_signal(de, &entry);
        if (mute_updated)
            send_mute_updated_signal(de, &entry);
    }
#endif

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
        pa_log_debug("Not restoring device for stream %s, because already set to '%s'.", name, new_data->sink->name);
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
            if (!new_data->volume_writable)
                pa_log_debug("Not restoring volume for sink input %s, because its volume can't be changed.", name);
            else if (new_data->volume_is_set)
                pa_log_debug("Not restoring volume for sink input %s, because already set.", name);
            else {
                pa_cvolume v;

                pa_log_info("Restoring volume for sink input %s.", name);

                v = e->volume;
                pa_cvolume_remap(&v, &e->channel_map, &new_data->channel_map);
                pa_sink_input_new_data_set_volume(new_data, &v);

                new_data->volume_is_absolute = FALSE;
                new_data->save_volume = TRUE;
            }
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

        if (!(name = get_name(so->proplist, "source-output")))
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

        if (u->restore_volume && e->volume_valid && si->volume_writable) {
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

        if (u->restore_device) {
            if (!e->device_valid) {
                if (si->save_sink) {
                    pa_log_info("Ensuring device is not saved for stream %s.", name);
                    /* If the device is not valid we should make sure the
                       save flag is cleared as the user may have specifically
                       removed the sink element from the rule. */
                    si->save_sink = FALSE;
                    /* This is cheating a bit. The sink input itself has not changed
                       but the rules governing it's routing have, so we fire this event
                       such that other routing modules (e.g. module-device-manager)
                       will pick up the change and reapply their routing */
                    pa_subscription_post(si->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, si->index);
                }
            } else if ((s = pa_namereg_get(u->core, e->device, PA_NAMEREG_SINK))) {
                pa_log_info("Restoring device for stream %s.", name);
                pa_sink_input_move_to(si, s, TRUE);
            }
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

        if (u->restore_device) {
            if (!e->device_valid) {
                if (so->save_source) {
                    pa_log_info("Ensuring device is not saved for stream %s.", name);
                    /* If the device is not valid we should make sure the
                       save flag is cleared as the user may have specifically
                       removed the source element from the rule. */
                    so->save_source = FALSE;
                    /* This is cheating a bit. The source output itself has not changed
                       but the rules governing it's routing have, so we fire this event
                       such that other routing modules (e.g. module-device-manager)
                       will pick up the change and reapply their routing */
                    pa_subscription_post(so->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, so->index);
                }
            } else if ((s = pa_namereg_get(u->core, e->device, PA_NAMEREG_SOURCE))) {
                pa_log_info("Restoring device for stream %s.", name);
                pa_source_output_move_to(so, s, TRUE);
            }
        }
    }
}

#ifdef DEBUG_VOLUME
PA_GCC_UNUSED static void stream_restore_dump_database(struct userdata *u) {
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

            if (mode == PA_UPDATE_SET) {
#ifdef HAVE_DBUS
                struct dbus_entry *de;
                void *state = NULL;

                PA_HASHMAP_FOREACH(de, u->dbus_entries, state) {
                    send_entry_removed_signal(de);
                    dbus_entry_free(pa_hashmap_remove(u->dbus_entries, de->entry_name));
                }
#endif
                pa_database_clear(u->database);
            }

            while (!pa_tagstruct_eof(t)) {
                const char *name, *device;
                pa_bool_t muted;
                struct entry entry;
                pa_datum key, data;
#ifdef HAVE_DBUS
                struct entry *old;
#endif

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

#ifdef HAVE_DBUS
                old = read_entry(u, name);
#endif

                key.data = (char*) name;
                key.size = strlen(name);

                data.data = &entry;
                data.size = sizeof(entry);

                pa_log_debug("Client %s changes entry %s.",
                             pa_strnull(pa_proplist_gets(pa_native_connection_get_client(c)->proplist, PA_PROP_APPLICATION_PROCESS_BINARY)),
                             name);

                if (pa_database_set(u->database, &key, &data, mode == PA_UPDATE_REPLACE) == 0) {
#ifdef HAVE_DBUS
                    struct dbus_entry *de;

                    if (old) {
                        pa_assert_se((de = pa_hashmap_get(u->dbus_entries, name)));

                        if ((old->device_valid != entry.device_valid)
                            || (entry.device_valid && !pa_streq(entry.device, old->device)))
                            send_device_updated_signal(de, &entry);

                        if ((old->volume_valid != entry.volume_valid)
                            || (entry.volume_valid && (!pa_cvolume_equal(&entry.volume, &old->volume)
                                                       || !pa_channel_map_equal(&entry.channel_map, &old->channel_map))))
                            send_volume_updated_signal(de, &entry);

                        if (!old->muted_valid || (entry.muted != old->muted))
                            send_mute_updated_signal(de, &entry);

                    } else {
                        de = dbus_entry_new(u, name);
                        pa_assert_se(pa_hashmap_put(u->dbus_entries, de->entry_name, de) == 0);
                        send_new_entry_signal(de);
                    }
#endif

                    if (apply_immediately)
                        apply_entry(u, name, &entry);
                }

#ifdef HAVE_DBUS
                if (old)
                    pa_xfree(old);
#endif
            }

            trigger_save(u);

            break;
        }

        case SUBCOMMAND_DELETE:

            while (!pa_tagstruct_eof(t)) {
                const char *name;
                pa_datum key;
#ifdef HAVE_DBUS
                struct dbus_entry *de;
#endif

                if (pa_tagstruct_gets(t, &name) < 0)
                    goto fail;

#ifdef HAVE_DBUS
                if ((de = pa_hashmap_get(u->dbus_entries, name))) {
                    send_entry_removed_signal(de);
                    dbus_entry_free(pa_hashmap_remove(u->dbus_entries, name));
                }
#endif

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
#ifdef HAVE_DBUS
    pa_datum key;
    pa_bool_t done;
#endif

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

    pa_log_info("Successfully opened database file '%s'.", fname);
    pa_xfree(fname);

#ifdef HAVE_DBUS
    u->dbus_protocol = pa_dbus_protocol_get(u->core);
    u->dbus_entries = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    pa_assert_se(pa_dbus_protocol_add_interface(u->dbus_protocol, OBJECT_PATH, &stream_restore_interface_info, u) >= 0);
    pa_assert_se(pa_dbus_protocol_register_extension(u->dbus_protocol, INTERFACE_STREAM_RESTORE) >= 0);

    /* Create the initial dbus entries. */
    done = !pa_database_first(u->database, &key, NULL);
    while (!done) {
        pa_datum next_key;
        char *name;
        struct dbus_entry *de;
        struct entry *e;

        done = !pa_database_next(u->database, &key, &next_key, NULL);

        name = pa_xstrndup(key.data, key.size);
        pa_datum_free(&key);

        /* Use read_entry() for checking that the entry is valid. */
        if ((e = read_entry(u, name))) {
            de = dbus_entry_new(u, name);
            pa_assert_se(pa_hashmap_put(u->dbus_entries, de->entry_name, de) == 0);
            pa_xfree(e);
        }

        pa_xfree(name);

        key = next_key;
    }
#endif

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

    return -1;
}

#ifdef HAVE_DBUS
static void free_dbus_entry_cb(void *p, void *userdata) {
    struct dbus_entry *de = p;

    pa_assert(de);

    dbus_entry_free(de);
}
#endif

void pa__done(pa_module*m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

#ifdef HAVE_DBUS
    if (u->dbus_protocol) {
        pa_assert(u->dbus_entries);

        pa_assert_se(pa_dbus_protocol_unregister_extension(u->dbus_protocol, INTERFACE_STREAM_RESTORE) >= 0);
        pa_assert_se(pa_dbus_protocol_remove_interface(u->dbus_protocol, OBJECT_PATH, stream_restore_interface_info.name) >= 0);

        pa_hashmap_free(u->dbus_entries, free_dbus_entry_cb, NULL);

        pa_dbus_protocol_unref(u->dbus_protocol);
    }
#endif

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
