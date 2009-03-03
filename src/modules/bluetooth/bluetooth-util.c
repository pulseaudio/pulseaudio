/***
  This file is part of PulseAudio.

  Copyright 2008 Joao Paulo Rechi Vita

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
#include <modules/dbus-util.h>

#include "bluetooth-util.h"

enum mode {
    MODE_FIND,
    MODE_GET,
    MODE_DISCOVER
};

struct pa_bluetooth_discovery {
    DBusConnection *connection;
    PA_LLIST_HEAD(pa_dbus_pending, pending);

    enum mode mode;

    /* If mode == MODE_FIND look for a specific device by its address.
       If mode == MODE_GET look for a specific device by its path. */
    const char *looking_for;
    pa_bluetooth_device *found_device;

    /* If looking_for is NULL we do long-time discovery */
    pa_hashmap *devices;
    pa_bluetooth_device_callback_t callback;
    struct userdata *userdata;
};

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

    d->device_info_valid = d->audio_sink_info_valid = d->headset_info_valid = 0;

    d->data = NULL;

    d->name = NULL;
    d->path = pa_xstrdup(path);
    d->paired = -1;
    d->alias = NULL;
    d->device_connected = -1;
    PA_LLIST_HEAD_INIT(pa_bluetooth_uuid, d->uuids);
    d->address = NULL;
    d->class = -1;
    d->trusted = -1;

    d->audio_sink_connected = -1;

    d->headset_connected = -1;

    return d;
}

void pa_bluetooth_device_free(pa_bluetooth_device *d) {
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

static pa_bool_t device_is_loaded(pa_bluetooth_device *d) {
    pa_assert(d);

    return d->device_info_valid && d->audio_sink_info_valid && d->headset_info_valid;
}

static pa_bool_t device_is_audio(pa_bluetooth_device *d) {
    pa_assert(d);

    pa_assert(d->device_info_valid);
    pa_assert(d->audio_sink_info_valid);
    pa_assert(d->headset_info_valid);

    return d->device_info_valid > 0 &&
        (d->audio_sink_info_valid > 0 || d->headset_info_valid > 0);
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

                    dbus_message_iter_get_basic(&ai, &value);
                    node = uuid_new(value);
                    PA_LLIST_PREPEND(pa_bluetooth_uuid, d->uuids, node);

                    if (!dbus_message_iter_next(&ai))
                        break;
                }
            }

            break;
        }
    }

    return 0;
}

static int parse_audio_property(pa_bluetooth_discovery *u, int *connected, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(u);
    pa_assert(connected);
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

/*     pa_log_debug("Parsing property org.bluez.{AudioSink|Headset}.%s", key); */

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_BOOLEAN: {

            dbus_bool_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Connected"))
                *connected = !!value;

/*             pa_log_debug("Value %s", pa_yes_no(value)); */

            break;
        }
    }

    return 0;
}

static void run_callback(pa_bluetooth_discovery *y, pa_bluetooth_device *d, pa_bool_t good) {
    pa_assert(y);
    pa_assert(d);

    if (y->mode != MODE_DISCOVER)
        return;

    if (!device_is_loaded(d))
        return;

    if (!device_is_audio(d))
        return;

    y->callback(y->userdata, d, good);

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
    else if (dbus_message_is_method_call(p->message, "org.bluez.Headset", "GetProperties"))
        d->headset_info_valid = valid;
    else if (dbus_message_is_method_call(p->message, "org.bluez.AudioSink", "GetProperties"))
        d->audio_sink_info_valid = valid;

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

            } else if (dbus_message_has_interface(p->message, "org.bluez.Headset")) {
                if (parse_audio_property(y, &d->headset_connected, &dict_i) < 0)
                    goto finish;

            }  else if (dbus_message_has_interface(p->message, "org.bluez.AudioSink")) {
                if (parse_audio_property(y, &d->audio_sink_connected, &dict_i) < 0)
                    goto finish;
            }
        }

        if (!dbus_message_iter_next(&element_i))
            break;
    }

finish:
    run_callback(y, d, TRUE);

    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_discovery *y, pa_bluetooth_device *d, DBusMessage *m, DBusPendingCallNotifyFunction func) {
    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(y);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(y->connection, m, &call, -1));

    p = pa_dbus_pending_new(m, call, y, d);
    PA_LLIST_PREPEND(pa_dbus_pending, y->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static void found_device(pa_bluetooth_discovery *y, const char* path) {
    DBusMessage *m;
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(path);

    d = device_new(path);

    if (y->mode == MODE_DISCOVER) {
        pa_assert(y->devices);
        pa_hashmap_put(y->devices, d->path, d);
    } else {
        pa_assert(!y->found_device);
        y->found_device = d;
    }

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Device", "GetProperties"));
    send_and_add_to_pending(y, d, m, get_properties_reply);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Headset", "GetProperties"));
    send_and_add_to_pending(y, d, m, get_properties_reply);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.AudioSink", "GetProperties"));
    send_and_add_to_pending(y, d, m, get_properties_reply);
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

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from ListDevices reply: %s", dbus_message_get_error_name(r));
        goto end;
    }

    if (!dbus_message_get_args(r, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &paths, &num, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.Adapter.ListDevices returned an error: '%s'\n", e.message);
        dbus_error_free(&e);
    } else {
        int i;

        for (i = 0; i < num; ++i)
            found_device(y, paths[i]);
    }

end:
    if (paths)
        dbus_free_string_array (paths);

    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static void find_device_reply(DBusPendingCall *pending, void *userdata) {
    DBusError e;
    DBusMessage *r;
    char *path = NULL;
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;

    pa_assert(pending);

    dbus_error_init(&e);

    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from FindDevice reply: %s", dbus_message_get_error_name(r));
        goto end;
    }

    if (!dbus_message_get_args(r, &e, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.Adapter.FindDevice returned an error: '%s'\n", e.message);
        dbus_error_free(&e);
    } else
        found_device(y, path);

end:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static void found_adapter(pa_bluetooth_discovery *y, const char *path) {
    DBusMessage *m;

    if (y->mode == MODE_FIND) {
        pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Adapter", "FindDevice"));

        pa_assert_se(dbus_message_append_args(m,
                                              DBUS_TYPE_STRING, &y->looking_for,
                                              DBUS_TYPE_INVALID));

        send_and_add_to_pending(y, NULL, m, find_device_reply);

    } else {
        pa_assert(y->mode == MODE_DISCOVER);

        pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Adapter", "ListDevices"));
        send_and_add_to_pending(y, NULL, m, list_devices_reply);
    }
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

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from ListAdapters reply: %s", dbus_message_get_error_name(r));
        goto end;
    }

    if (!dbus_message_get_args(r, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &paths, &num, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.Manager.ListAdapters returned an error: %s", e.message);
        dbus_error_free(&e);
    } else {
        int i;

        for (i = 0; i < num; ++i)
            found_adapter(y, paths[i]);
    }

end:
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

            pa_assert_se(y->mode == MODE_DISCOVER);
            run_callback(y, d, FALSE);

            pa_bluetooth_device_free(d);
        }

        return DBUS_HANDLER_RESULT_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Adapter", "DeviceCreated")) {
        const char *path;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Adapter.DeviceCreated: %s", err.message);
            goto fail;
        }

        pa_log_debug("Device %s created", path);

        found_device(y, path);
        return DBUS_HANDLER_RESULT_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Manager", "AdapterAdded")) {
        const char *path;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Manager.AdapterAdded: %s", err.message);
            goto fail;
        }

        pa_log_debug("Adapter %s created", path);

        found_adapter(y, path);
        return DBUS_HANDLER_RESULT_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Headset", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.AudioSink", "PropertyChanged") ||
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

            } else if (dbus_message_has_interface(m, "org.bluez.Headset")) {
                if (parse_audio_property(y, &d->headset_connected, &arg_i) < 0)
                    goto fail;

            }  else if (dbus_message_has_interface(m, "org.bluez.AudioSink")) {
                if (parse_audio_property(y, &d->audio_sink_connected, &arg_i) < 0)
                    goto fail;
            }

            pa_assert_se(y->mode == MODE_DISCOVER);
            run_callback(y, d, TRUE);
        }

        return DBUS_HANDLER_RESULT_HANDLED;
    }

fail:
    dbus_error_free(&err);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

pa_bluetooth_device* pa_bluetooth_find_device(DBusConnection *c, const char* address) {
    pa_bluetooth_discovery y;

    memset(&y, 0, sizeof(y));
    y.mode = MODE_FIND;
    y.looking_for = address;
    y.connection = c;
    PA_LLIST_HEAD_INIT(pa_dbus_pending, y.pending);

    list_adapters(&y);

    pa_dbus_sync_pending_list(&y.pending);
    pa_assert(!y.pending);

    if (y.found_device) {
        pa_assert(device_is_loaded(y.found_device));

        if (!device_is_audio(y.found_device)) {
            pa_bluetooth_device_free(y.found_device);
            return NULL;
        }
    }

    return y.found_device;
}

pa_bluetooth_device* pa_bluetooth_get_device(DBusConnection *c, const char* path) {
    pa_bluetooth_discovery y;

    memset(&y, 0, sizeof(y));
    y.mode = MODE_GET;
    y.connection = c;
    PA_LLIST_HEAD_INIT(pa_dbus_pending, y.pending);

    found_device(&y, path);

    pa_dbus_sync_pending_list(&y.pending);
    pa_assert(!y.pending);

    if (y.found_device) {
        pa_assert(device_is_loaded(y.found_device));

        if (!device_is_audio(y.found_device)) {
            pa_bluetooth_device_free(y.found_device);
            return NULL;
        }
    }

    return y.found_device;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_new(DBusConnection *c, pa_bluetooth_device_callback_t cb, struct userdata *u) {
    DBusError err;
    pa_bluetooth_discovery *y;

    pa_assert(c);
    pa_assert(cb);

    dbus_error_init(&err);

    y = pa_xnew0(pa_bluetooth_discovery, 1);
    y->mode = MODE_DISCOVER;
    y->connection = c;
    y->callback = cb;
    y->userdata = u;
    y->devices = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    PA_LLIST_HEAD_INIT(pa_dbus_pending, y->pending);

    /* dynamic detection of bluetooth audio devices */
    if (!dbus_connection_add_filter(c, filter_cb, y, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }

    if (pa_dbus_add_matches(
                c, &err,
                "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterAdded'",
                "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'",
                "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceCreated'",
                "type='signal',sender='org.bluez',interface='org.bluez.Device',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'", NULL) < 0) {
        pa_log("Failed to add D-Bus matches: %s", err.message);
        goto fail;
    }

    list_adapters(y);

    return y;

fail:
    dbus_error_free(&err);
    return NULL;
}

void pa_bluetooth_discovery_free(pa_bluetooth_discovery *y) {
    pa_bluetooth_device *d;

    pa_assert(y);

    pa_dbus_free_pending_list(&y->pending);

    if (y->devices) {
        while ((d = pa_hashmap_steal_first(y->devices))) {
            run_callback(y, d, FALSE);
            pa_bluetooth_device_free(d);
        }

        pa_hashmap_free(y->devices, NULL, NULL);
    }

    if (y->connection) {
        pa_dbus_remove_matches(y->connection,
                               "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterAdded'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterRemoved'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceCreated'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Device',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'", NULL);

        dbus_connection_remove_filter(y->connection, filter_cb, y);
    }
}

void pa_bluetooth_discovery_sync(pa_bluetooth_discovery *y) {
    pa_assert(y);

    pa_dbus_sync_pending_list(&y->pending);
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
