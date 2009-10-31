/***
  This file is part of PulseAudio.

  Copyright 2008-2009 Joao Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-util.h>
#include <pulsecore/shared.h>
#include <pulsecore/dbus-shared.h>

#include "bluetooth-util.h"

struct pa_bluetooth_discovery {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_dbus_connection *connection;
    PA_LLIST_HEAD(pa_dbus_pending, pending);
    pa_hashmap *devices;
    pa_hook hook;
};

static void get_properties_reply(DBusPendingCall *pending, void *userdata);
static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_discovery *y, pa_bluetooth_device *d, DBusMessage *m, DBusPendingCallNotifyFunction func);

static pa_bt_audio_state_t pa_bt_audio_state_from_string(const char* value) {
    pa_assert(value);

    if (pa_streq(value, "disconnected"))
        return PA_BT_AUDIO_STATE_DISCONNECTED;
    else if (pa_streq(value, "connecting"))
        return PA_BT_AUDIO_STATE_CONNECTING;
    else if (pa_streq(value, "connected"))
        return PA_BT_AUDIO_STATE_CONNECTED;
    else if (pa_streq(value, "playing"))
        return PA_BT_AUDIO_STATE_PLAYING;

    return PA_BT_AUDIO_STATE_INVALID;
}

static pa_bluetooth_uuid *uuid_new(const char *uuid) {
    pa_bluetooth_uuid *u;

    u = pa_xnew(pa_bluetooth_uuid, 1);
    u->uuid = pa_xstrdup(uuid);
    PA_LLIST_INIT(pa_bluetooth_uuid, u);

    return u;
}

static void uuid_free(pa_bluetooth_uuid *u) {
    pa_assert(u);

    pa_xfree(u->uuid);
    pa_xfree(u);
}

static pa_bluetooth_device* device_new(const char *path) {
    pa_bluetooth_device *d;

    d = pa_xnew(pa_bluetooth_device, 1);

    d->dead = FALSE;

    d->device_info_valid = 0;

    d->name = NULL;
    d->path = pa_xstrdup(path);
    d->paired = -1;
    d->alias = NULL;
    d->device_connected = -1;
    PA_LLIST_HEAD_INIT(pa_bluetooth_uuid, d->uuids);
    d->address = NULL;
    d->class = -1;
    d->trusted = -1;

    d->audio_state = PA_BT_AUDIO_STATE_INVALID;
    d->audio_sink_state = PA_BT_AUDIO_STATE_INVALID;
    d->audio_source_state = PA_BT_AUDIO_STATE_INVALID;
    d->headset_state = PA_BT_AUDIO_STATE_INVALID;

    return d;
}

static void device_free(pa_bluetooth_device *d) {
    pa_bluetooth_uuid *u;

    pa_assert(d);

    while ((u = d->uuids)) {
        PA_LLIST_REMOVE(pa_bluetooth_uuid, d->uuids, u);
        uuid_free(u);
    }

    pa_xfree(d->name);
    pa_xfree(d->path);
    pa_xfree(d->alias);
    pa_xfree(d->address);
    pa_xfree(d);
}

static pa_bool_t device_is_audio(pa_bluetooth_device *d) {
    pa_assert(d);

    return
        d->device_info_valid &&
        (d->audio_state != PA_BT_AUDIO_STATE_INVALID &&
         (d->audio_sink_state != PA_BT_AUDIO_STATE_INVALID ||
          d->audio_source_state != PA_BT_AUDIO_STATE_INVALID ||
          d->headset_state != PA_BT_AUDIO_STATE_INVALID));
}

static int parse_device_property(pa_bluetooth_discovery *y, pa_bluetooth_device *d, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(y);
    pa_assert(d);
    pa_assert(i);

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_STRING) {
        pa_log("Property name not a string.");
        return -1;
    }

    dbus_message_iter_get_basic(i, &key);

    if (!dbus_message_iter_next(i))  {
        pa_log("Property value missing");
        return -1;
    }

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_VARIANT) {
        pa_log("Property value not a variant.");
        return -1;
    }

    dbus_message_iter_recurse(i, &variant_i);

/*     pa_log_debug("Parsing property org.bluez.Device.%s", key); */

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_STRING: {

            const char *value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Name")) {
                pa_xfree(d->name);
                d->name = pa_xstrdup(value);
            } else if (pa_streq(key, "Alias")) {
                pa_xfree(d->alias);
                d->alias = pa_xstrdup(value);
            } else if (pa_streq(key, "Address")) {
                pa_xfree(d->address);
                d->address = pa_xstrdup(value);
            }

/*             pa_log_debug("Value %s", value); */

            break;
        }

        case DBUS_TYPE_BOOLEAN: {

            dbus_bool_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Paired"))
                d->paired = !!value;
            else if (pa_streq(key, "Connected"))
                d->device_connected = !!value;
            else if (pa_streq(key, "Trusted"))
                d->trusted = !!value;

/*             pa_log_debug("Value %s", pa_yes_no(value)); */

            break;
        }

        case DBUS_TYPE_UINT32: {

            uint32_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Class"))
                d->class = (int) value;

/*             pa_log_debug("Value %u", (unsigned) value); */

            break;
        }

        case DBUS_TYPE_ARRAY: {

            DBusMessageIter ai;
            dbus_message_iter_recurse(&variant_i, &ai);

            if (dbus_message_iter_get_arg_type(&ai) == DBUS_TYPE_STRING &&
                pa_streq(key, "UUIDs")) {

                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    pa_bluetooth_uuid *node;
                    const char *value;
                    DBusMessage *m;

                    dbus_message_iter_get_basic(&ai, &value);
                    node = uuid_new(value);
                    PA_LLIST_PREPEND(pa_bluetooth_uuid, d->uuids, node);

                    /* Vudentz said the interfaces are here when the UUIDs are announced */
                    if (strcasecmp(HSP_HS_UUID, value) == 0 || strcasecmp(HFP_HS_UUID, value) == 0) {
                        pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.Headset", "GetProperties"));
                        send_and_add_to_pending(y, d, m, get_properties_reply);
                    } else if (strcasecmp(A2DP_SINK_UUID, value) == 0) {
                        pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.AudioSink", "GetProperties"));
                        send_and_add_to_pending(y, d, m, get_properties_reply);
                    } else if (strcasecmp(A2DP_SOURCE_UUID, value) == 0) {
                        pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.AudioSource", "GetProperties"));
                        send_and_add_to_pending(y, d, m, get_properties_reply);
                    }

                    /* this might eventually be racy if .Audio is not there yet, but the State change will come anyway later, so this call is for cold-detection mostly */
                    pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.Audio", "GetProperties"));
                    send_and_add_to_pending(y, d, m, get_properties_reply);

                    if (!dbus_message_iter_next(&ai))
                        break;
                }
            }

            break;
        }
    }

    return 0;
}

static int parse_audio_property(pa_bluetooth_discovery *u, int *state, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(u);
    pa_assert(state);
    pa_assert(i);

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_STRING) {
        pa_log("Property name not a string.");
        return -1;
    }

    dbus_message_iter_get_basic(i, &key);

    if (!dbus_message_iter_next(i))  {
        pa_log("Property value missing");
        return -1;
    }

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_VARIANT) {
        pa_log("Property value not a variant.");
        return -1;
    }

    dbus_message_iter_recurse(i, &variant_i);

/*     pa_log_debug("Parsing property org.bluez.{Audio|AudioSink|AudioSource|Headset}.%s", key); */

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_STRING: {

            const char *value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "State"))
                *state = pa_bt_audio_state_from_string(value);
/*             pa_log_debug("Value %s", value); */

            break;
        }
    }

    return 0;
}

static void run_callback(pa_bluetooth_discovery *y, pa_bluetooth_device *d, pa_bool_t dead) {
    pa_assert(y);
    pa_assert(d);

    if (!device_is_audio(d))
        return;

    d->dead = dead;
    pa_hook_fire(&y->hook, d);
}

static void remove_all_devices(pa_bluetooth_discovery *y) {
    pa_bluetooth_device *d;

    pa_assert(y);

    while ((d = pa_hashmap_steal_first(y->devices))) {
        run_callback(y, d, TRUE);
        device_free(d);
    }
}

static void get_properties_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    DBusMessageIter arg_i, element_i;
    pa_dbus_pending *p;
    pa_bluetooth_device *d;
    pa_bluetooth_discovery *y;
    int valid;

    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

/*     pa_log_debug("Got %s.GetProperties response for %s", */
/*                  dbus_message_get_interface(p->message), */
/*                  dbus_message_get_path(p->message)); */

    d = p->call_data;

    valid = dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR ? -1 : 1;

    if (dbus_message_is_method_call(p->message, "org.bluez.Device", "GetProperties"))
        d->device_info_valid = valid;

    if (dbus_message_is_error(r, DBUS_ERROR_SERVICE_UNKNOWN)) {
        pa_log_debug("Bluetooth daemon is apparently not available.");
        remove_all_devices(y);
        goto finish2;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {

        if (!dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD))
            pa_log("Error from GetProperties reply: %s", dbus_message_get_error_name(r));

        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i)) {
        pa_log("GetProperties reply has no arguments.");
        goto finish;
    }

    if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_ARRAY) {
        pa_log("GetProperties argument is not an array.");
        goto finish;
    }

    dbus_message_iter_recurse(&arg_i, &element_i);
    while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {

        if (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict_i;

            dbus_message_iter_recurse(&element_i, &dict_i);

            if (dbus_message_has_interface(p->message, "org.bluez.Device")) {
                if (parse_device_property(y, d, &dict_i) < 0)
                    goto finish;

            } else if (dbus_message_has_interface(p->message, "org.bluez.Audio")) {
                if (parse_audio_property(y, &d->audio_state, &dict_i) < 0)
                    goto finish;

            } else if (dbus_message_has_interface(p->message, "org.bluez.Headset")) {
                if (parse_audio_property(y, &d->headset_state, &dict_i) < 0)
                    goto finish;

            }  else if (dbus_message_has_interface(p->message, "org.bluez.AudioSink")) {
                if (parse_audio_property(y, &d->audio_sink_state, &dict_i) < 0)
                    goto finish;
            }  else if (dbus_message_has_interface(p->message, "org.bluez.AudioSource")) {
                if (parse_audio_property(y, &d->audio_source_state, &dict_i) < 0)
                    goto finish;
            }
        }

        if (!dbus_message_iter_next(&element_i))
            break;
    }

finish:
    run_callback(y, d, FALSE);

finish2:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_discovery *y, pa_bluetooth_device *d, DBusMessage *m, DBusPendingCallNotifyFunction func) {
    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(y);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(pa_dbus_connection_get(y->connection), m, &call, -1));

    p = pa_dbus_pending_new(pa_dbus_connection_get(y->connection), m, call, y, d);
    PA_LLIST_PREPEND(pa_dbus_pending, y->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static void found_device(pa_bluetooth_discovery *y, const char* path) {
    DBusMessage *m;
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(path);

    if (pa_hashmap_get(y->devices, path))
        return;

    d = device_new(path);

    pa_hashmap_put(y->devices, d->path, d);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Device", "GetProperties"));
    send_and_add_to_pending(y, d, m, get_properties_reply);

    /* Before we read the other properties (Audio, AudioSink, AudioSource,
     * Headset) we wait that the UUID is read */
}

static void list_devices_reply(DBusPendingCall *pending, void *userdata) {
    DBusError e;
    DBusMessage *r;
    char **paths = NULL;
    int num = -1;
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;

    pa_assert(pending);

    dbus_error_init(&e);

    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, DBUS_ERROR_SERVICE_UNKNOWN)) {
        pa_log_debug("Bluetooth daemon is apparently not available.");
        remove_all_devices(y);
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from ListDevices reply: %s", dbus_message_get_error_name(r));
        goto finish;
    }

    if (!dbus_message_get_args(r, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &paths, &num, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.Adapter.ListDevices returned an error: '%s'\n", e.message);
        dbus_error_free(&e);
    } else {
        int i;

        for (i = 0; i < num; ++i)
            found_device(y, paths[i]);
    }

finish:
    if (paths)
        dbus_free_string_array (paths);

    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static void found_adapter(pa_bluetooth_discovery *y, const char *path) {
    DBusMessage *m;

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Adapter", "ListDevices"));
    send_and_add_to_pending(y, NULL, m, list_devices_reply);
}

static void list_adapters_reply(DBusPendingCall *pending, void *userdata) {
    DBusError e;
    DBusMessage *r;
    char **paths = NULL;
    int num = -1;
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;

    pa_assert(pending);

    dbus_error_init(&e);

    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, DBUS_ERROR_SERVICE_UNKNOWN)) {
        pa_log_debug("Bluetooth daemon is apparently not available.");
        remove_all_devices(y);
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from ListAdapters reply: %s", dbus_message_get_error_name(r));
        goto finish;
    }

    if (!dbus_message_get_args(r, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &paths, &num, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.Manager.ListAdapters returned an error: %s", e.message);
        dbus_error_free(&e);
    } else {
        int i;

        for (i = 0; i < num; ++i)
            found_adapter(y, paths[i]);
    }

finish:
    if (paths)
        dbus_free_string_array (paths);

    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static void list_adapters(pa_bluetooth_discovery *y) {
    DBusMessage *m;
    pa_assert(y);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", "/", "org.bluez.Manager", "ListAdapters"));
    send_and_add_to_pending(y, NULL, m, list_adapters_reply);
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *userdata) {
    DBusError err;
    pa_bluetooth_discovery *y;

    pa_assert(bus);
    pa_assert(m);

    pa_assert_se(y = userdata);

    dbus_error_init(&err);

    pa_log_debug("dbus: interface=%s, path=%s, member=%s\n",
            dbus_message_get_interface(m),
            dbus_message_get_path(m),
            dbus_message_get_member(m));

    if (dbus_message_is_signal(m, "org.bluez.Adapter", "DeviceRemoved")) {
        const char *path;
        pa_bluetooth_device *d;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Adapter.DeviceRemoved: %s", err.message);
            goto fail;
        }

        pa_log_debug("Device %s removed", path);

        if ((d = pa_hashmap_remove(y->devices, path))) {
            run_callback(y, d, TRUE);
            device_free(d);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Adapter", "DeviceCreated")) {
        const char *path;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Adapter.DeviceCreated: %s", err.message);
            goto fail;
        }

        pa_log_debug("Device %s created", path);

        found_device(y, path);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Manager", "AdapterAdded")) {
        const char *path;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Manager.AdapterAdded: %s", err.message);
            goto fail;
        }

        pa_log_debug("Adapter %s created", path);

        found_adapter(y, path);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Audio", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.Headset", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.AudioSink", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.AudioSource", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.Device", "PropertyChanged")) {

        pa_bluetooth_device *d;

        if ((d = pa_hashmap_get(y->devices, dbus_message_get_path(m)))) {
            DBusMessageIter arg_i;

            if (!dbus_message_iter_init(m, &arg_i)) {
                pa_log("Failed to parse PropertyChanged: %s", err.message);
                goto fail;
            }

            if (dbus_message_has_interface(m, "org.bluez.Device")) {
                if (parse_device_property(y, d, &arg_i) < 0)
                    goto fail;

            } else if (dbus_message_has_interface(m, "org.bluez.Audio")) {
                if (parse_audio_property(y, &d->audio_state, &arg_i) < 0)
                    goto fail;

            } else if (dbus_message_has_interface(m, "org.bluez.Headset")) {
                if (parse_audio_property(y, &d->headset_state, &arg_i) < 0)
                    goto fail;

            }  else if (dbus_message_has_interface(m, "org.bluez.AudioSink")) {
                if (parse_audio_property(y, &d->audio_sink_state, &arg_i) < 0)
                    goto fail;
            }  else if (dbus_message_has_interface(m, "org.bluez.AudioSource")) {
                if (parse_audio_property(y, &d->audio_source_state, &arg_i) < 0)
                    goto fail;
            }

            run_callback(y, d, FALSE);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Device", "DisconnectRequested")) {
        pa_bluetooth_device *d;

        if ((d = pa_hashmap_get(y->devices, dbus_message_get_path(m)))) {
            /* Device will disconnect in 2 sec */
            d->audio_state = PA_BT_AUDIO_STATE_DISCONNECTED;
            d->audio_sink_state = PA_BT_AUDIO_STATE_DISCONNECTED;
            d->audio_source_state = PA_BT_AUDIO_STATE_DISCONNECTED;
            d->headset_state = PA_BT_AUDIO_STATE_DISCONNECTED;

            run_callback(y, d, FALSE);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;

        if (!dbus_message_get_args(m, &err,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
            goto fail;
        }

        if (pa_streq(name, "org.bluez")) {
            if (old_owner && *old_owner) {
                pa_log_debug("Bluetooth daemon disappeared.");
                remove_all_devices(y);
            }

            if (new_owner && *new_owner) {
                pa_log_debug("Bluetooth daemon appeared.");
                list_adapters(y);
            }
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

fail:
    dbus_error_free(&err);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

const pa_bluetooth_device* pa_bluetooth_discovery_get_by_address(pa_bluetooth_discovery *y, const char* address) {
    pa_bluetooth_device *d;
    void *state = NULL;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(address);

    if (!pa_hook_is_firing(&y->hook))
        pa_bluetooth_discovery_sync(y);

    while ((d = pa_hashmap_iterate(y->devices, &state, NULL)))
        if (pa_streq(d->address, address))
            return device_is_audio(d) ? d : NULL;

    return NULL;
}

const pa_bluetooth_device* pa_bluetooth_discovery_get_by_path(pa_bluetooth_discovery *y, const char* path) {
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(path);

    if (!pa_hook_is_firing(&y->hook))
        pa_bluetooth_discovery_sync(y);

    if ((d = pa_hashmap_get(y->devices, path)))
        if (device_is_audio(d))
            return d;

    return NULL;
}

static int setup_dbus(pa_bluetooth_discovery *y) {
    DBusError err;

    dbus_error_init(&err);

    y->connection = pa_dbus_bus_get(y->core, DBUS_BUS_SYSTEM, &err);

    if (dbus_error_is_set(&err) || !y->connection) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        dbus_error_free(&err);
        return -1;
    }

    return 0;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *c) {
    DBusError err;
    pa_bluetooth_discovery *y;

    pa_assert(c);

    dbus_error_init(&err);

    if ((y = pa_shared_get(c, "bluetooth-discovery")))
        return pa_bluetooth_discovery_ref(y);

    y = pa_xnew0(pa_bluetooth_discovery, 1);
    PA_REFCNT_INIT(y);
    y->core = c;
    y->devices = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    PA_LLIST_HEAD_INIT(pa_dbus_pending, y->pending);
    pa_hook_init(&y->hook, y);
    pa_shared_set(c, "bluetooth-discovery", y);

    if (setup_dbus(y) < 0)
        goto fail;

    /* dynamic detection of bluetooth audio devices */
    if (!dbus_connection_add_filter(pa_dbus_connection_get(y->connection), filter_cb, y, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }

    if (pa_dbus_add_matches(
                pa_dbus_connection_get(y->connection), &err,
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
                "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterAdded'",
                "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'",
                "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceCreated'",
                "type='signal',sender='org.bluez',interface='org.bluez.Device',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.Device',member='DisconnectRequested'",
                "type='signal',sender='org.bluez',interface='org.bluez.Audio',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.AudioSource',member='PropertyChanged'", NULL) < 0) {
        pa_log("Failed to add D-Bus matches: %s", err.message);
        goto fail;
    }

    list_adapters(y);

    return y;

fail:

    if (y)
        pa_bluetooth_discovery_unref(y);

    dbus_error_free(&err);

    return NULL;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_ref(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    PA_REFCNT_INC(y);

    return y;
}

void pa_bluetooth_discovery_unref(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    if (PA_REFCNT_DEC(y) > 0)
        return;

    pa_dbus_free_pending_list(&y->pending);

    if (y->devices) {
        remove_all_devices(y);
        pa_hashmap_free(y->devices, NULL, NULL);
    }

    if (y->connection) {
        pa_dbus_remove_matches(pa_dbus_connection_get(y->connection),
                               "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterAdded'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterRemoved'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceCreated'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Device',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Device',member='DisconnectRequested'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Audio',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.AudioSource',member='PropertyChanged'", NULL);

        dbus_connection_remove_filter(pa_dbus_connection_get(y->connection), filter_cb, y);

        pa_dbus_connection_unref(y->connection);
    }

    pa_hook_done(&y->hook);

    if (y->core)
        pa_shared_remove(y->core, "bluetooth-discovery");

    pa_xfree(y);
}

void pa_bluetooth_discovery_sync(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    pa_dbus_sync_pending_list(&y->pending);
}

pa_hook* pa_bluetooth_discovery_hook(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    return &y->hook;
}

const char*pa_bluetooth_get_form_factor(uint32_t class) {
    unsigned i;
    const char *r;

    static const char * const table[] = {
        [1] = "headset",
        [2] = "hands-free",
        [4] = "microphone",
        [5] = "speaker",
        [6] = "headphone",
        [7] = "portable",
        [8] = "car",
        [10] = "hifi"
    };

    if (((class >> 8) & 31) != 4)
        return NULL;

    if ((i = (class >> 2) & 63) > PA_ELEMENTSOF(table))
        r =  NULL;
    else
        r = table[i];

    if (!r)
        pa_log_debug("Unknown Bluetooth minor device class %u", i);

    return r;
}

char *pa_bluetooth_cleanup_name(const char *name) {
    char *t, *s, *d;
    pa_bool_t space = FALSE;

    pa_assert(name);

    while ((*name >= 1 && *name <= 32) || *name >= 127)
        name++;

    t = pa_xstrdup(name);

    for (s = d = t; *s; s++) {

        if (*s <= 32 || *s >= 127 || *s == '_') {
            space = TRUE;
            continue;
        }

        if (space) {
            *(d++) = ' ';
            space = FALSE;
        }

        *(d++) = *s;
    }

    *d = 0;

    return t;
}

pa_bool_t pa_bluetooth_uuid_has(pa_bluetooth_uuid *uuids, const char *uuid) {
    pa_assert(uuid);

    while (uuids) {
        if (strcasecmp(uuids->uuid, uuid) == 0)
            return TRUE;

        uuids = uuids->next;
    }

    return FALSE;
}
