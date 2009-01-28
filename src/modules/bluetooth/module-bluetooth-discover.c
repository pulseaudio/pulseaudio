/***
    This file is part of PulseAudio.

    Copyright 2008 Joao Paulo Rechi Vita

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/macro.h>
#include <pulsecore/llist.h>
#include <pulsecore/core-util.h>

#include "../dbus-util.h"
#include "module-bluetooth-discover-symdef.h"

PA_MODULE_AUTHOR("Joao Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Detect available bluetooth audio devices and load bluetooth audio drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE("");

struct module {
    char *profile;
    uint32_t index;
    PA_LLIST_FIELDS(struct module);
};

struct uuid {
    char *uuid;
    PA_LLIST_FIELDS(struct uuid);
};

struct dbus_pending {
    char *path;
    char *profile;
    DBusPendingCall *pending;
    PA_LLIST_FIELDS(struct dbus_pending);
};

struct device {
    char *name;
    char *object_path;
    int paired;
    char *alias;
    int connected;
    PA_LLIST_HEAD(struct uuid, uuid_list);
    char *address;
    int class;
    int trusted;
    PA_LLIST_HEAD(struct module, module_list);
    PA_LLIST_FIELDS(struct device);
};

struct userdata {
    pa_module *module;
    pa_dbus_connection *conn;
    dbus_int32_t dbus_data_slot;
    PA_LLIST_HEAD(struct device, device_list);
    PA_LLIST_HEAD(struct dbus_pending, dbus_pending_list);
};

static struct module *module_new(const char *profile, pa_module *pa_m) {
    struct module *m;

    m = pa_xnew(struct module, 1);
    m->profile = pa_xstrdup(profile);
    m->index = pa_m->index;
    PA_LLIST_INIT(struct module, m);

    return m;
}

static void module_free(struct module *m) {
    pa_assert(m);

    pa_xfree(m->profile);
    pa_xfree(m);
}

static struct module* module_find(struct device *d, const char *profile) {
    struct module *m;

    for (m = d->module_list; m; m = m->next)
        if (pa_streq(m->profile, profile))
            return m;

    return NULL;
}

static struct uuid *uuid_new(const char *uuid) {
    struct uuid *node;

    node = pa_xnew(struct uuid, 1);
    node->uuid = pa_xstrdup(uuid);
    PA_LLIST_INIT(struct uuid, node);

    return node;
}

static void uuid_free(struct uuid *uuid) {
    pa_assert(uuid);

    pa_xfree(uuid->uuid);
    pa_xfree(uuid);
}

static struct dbus_pending *dbus_pending_new(struct userdata *u, DBusPendingCall *pending, const char *path, const char *profile) {
    struct dbus_pending *node;

    pa_assert(pending);

    node = pa_xnew(struct dbus_pending, 1);
    node->pending = pending;
    node->path = pa_xstrdup(path);
    node->profile = pa_xstrdup(profile);
    PA_LLIST_INIT(struct dbus_pending, node);
    dbus_pending_call_set_data(pending, u->dbus_data_slot, node, NULL);

    return node;
}

static void dbus_pending_free(struct dbus_pending *pending) {
    pa_assert(pending);

    pa_xfree(pending->path);
    pa_xfree(pending->profile);
    dbus_pending_call_cancel(pending->pending);
    dbus_pending_call_unref(pending->pending);
    pa_xfree(pending);
}

static struct device *device_new(const char *object_path) {
    struct device *node;

    node = pa_xnew(struct device, 1);
    node->name = NULL;
    node->object_path = pa_xstrdup(object_path);
    node->paired = -1;
    node->alias = NULL;
    node->connected = -1;
    PA_LLIST_HEAD_INIT(struct uuid, node->uuid_list);
    node->address = NULL;
    node->class = -1;
    node->trusted = -1;
    PA_LLIST_HEAD_INIT(struct module, node->module_list);
    PA_LLIST_INIT(struct device, node);

    return node;
}

static void device_free(struct device *device) {
    struct module *m;
    struct uuid *i;

    pa_assert(device);

    while ((m = device->module_list)) {
        PA_LLIST_REMOVE(struct module, device->module_list, m);
        module_free(m);
    }

    while ((i = device->uuid_list)) {
        PA_LLIST_REMOVE(struct uuid, device->uuid_list, i);
        uuid_free(i);
    }

    pa_xfree(device->name);
    pa_xfree(device->object_path);
    pa_xfree(device->alias);
    pa_xfree(device->address);
    pa_xfree(device);
}

static struct device* device_find(struct userdata *u, const char *path) {
    struct device *i;

    for (i = u->device_list; i; i = i->next)
        if (pa_streq(i->object_path, path))
            return i;

    return NULL;
}

static int parse_device_property(struct userdata *u, struct device *d, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(u);
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

    pa_log_debug("Parsing device property %s", key);

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

            break;
        }

        case DBUS_TYPE_BOOLEAN: {

            dbus_bool_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Paired"))
                d->paired = !!value;
            else if (pa_streq(key, "Connected"))
                d->connected = !!value;
            else if (pa_streq(key, "Trusted"))
                d->trusted = !!value;

            break;
        }

        case DBUS_TYPE_UINT32: {

            uint32_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Class"))
                d->class = (int) value;

            break;
        }

        case DBUS_TYPE_ARRAY: {

            DBusMessageIter ai;
            dbus_message_iter_recurse(&variant_i, &ai);

            if (dbus_message_iter_get_arg_type(&ai) == DBUS_TYPE_STRING &&
                pa_streq(key, "UUIDs")) {

                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    struct uuid *node;
                    const char *value;

                    dbus_message_iter_get_basic(&ai, &value);
                    node = uuid_new(value);
                    PA_LLIST_PREPEND(struct uuid, d->uuid_list, node);

                    if (!dbus_message_iter_next(&ai))
                        break;
                }
            }

            break;
        }
    }

    return 0;
}

static int get_device_properties(struct userdata *u, struct device *d) {
    DBusError e;
    DBusMessage *m = NULL, *r = NULL;
    DBusMessageIter arg_i, element_i;
    int ret = -1;

    pa_assert(u);
    pa_assert(d);

    dbus_error_init(&e);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->object_path, "org.bluez.Device", "GetProperties"));

    r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->conn), m, -1, &e);

    if (!r) {
        pa_log("org.bluez.Device.GetProperties failed: %s", e.message);
        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i)) {
        pa_log("org.bluez.Device.GetProperties reply has no arguments");
        goto finish;
    }

    if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_ARRAY) {
        pa_log("org.bluez.Device.GetProperties argument is not an array");
        goto finish;
    }

    dbus_message_iter_recurse(&arg_i, &element_i);
    while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {

        if (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict_i;

            dbus_message_iter_recurse(&element_i, &dict_i);

            if (parse_device_property(u, d, &dict_i) < 0)
                goto finish;
        }

        if (!dbus_message_iter_next(&element_i))
            break;
    }

    ret = 0;

finish:
    if (m)
        dbus_message_unref(m);
    if (r)
        dbus_message_unref(r);

    dbus_error_free(&e);

    return ret;
}

static void load_module_for_device(struct userdata *u, struct device *d, const char *profile) {
    char *args;
    pa_module *pa_m;
    struct module *m;

    pa_assert(u);
    pa_assert(d);

    get_device_properties(u, d);
    args = pa_sprintf_malloc("sink_name=\"%s\" address=\"%s\" profile=\"%s\" path=\"%s\"", d->name, d->address, profile, d->object_path);
    pa_m = pa_module_load(u->module->core, "module-bluetooth-device", args);
    pa_xfree(args);

    if (!pa_m) {
        pa_log_debug("Failed to load module for device %s", d->object_path);
        return;
    }

    m = module_new(profile, pa_m);
    PA_LLIST_PREPEND(struct module, d->module_list, m);
}

static void unload_module_for_device(struct userdata *u, struct device *d, const char *profile) {
    struct module *m;

    pa_assert(u);
    pa_assert(d);

    if (!(m = module_find(d, profile)))
        return;

    pa_module_unload_request_by_index(u->module->core, m->index, TRUE);

    PA_LLIST_REMOVE(struct module, d->module_list, m);
    module_free(m);
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *msg, void *userdata) {
    DBusMessageIter arg_i;
    DBusError err;
    const char *value;
    struct userdata *u;

    pa_assert(bus);
    pa_assert(msg);
    pa_assert(userdata);
    u = userdata;

    dbus_error_init(&err);

    pa_log_debug("dbus: interface=%s, path=%s, member=%s\n",
            dbus_message_get_interface(msg),
            dbus_message_get_path(msg),
            dbus_message_get_member(msg));

    if (dbus_message_is_signal(msg, "org.bluez.Adapter", "DeviceRemoved")) {

        if (!dbus_message_iter_init(msg, &arg_i))
            pa_log("dbus: message has no parameters");
        else if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_OBJECT_PATH)
            pa_log("dbus: argument is not object path");
        else {
            struct device *d;

            dbus_message_iter_get_basic(&arg_i, &value);
            pa_log_debug("hcid: device %s removed", value);

            if ((d = device_find(u, value))) {
                PA_LLIST_REMOVE(struct device, u->device_list, d);
                device_free(d);
            }
        }

    } else if (dbus_message_is_signal(msg, "org.bluez.Headset", "PropertyChanged") ||
               dbus_message_is_signal(msg, "org.bluez.AudioSink", "PropertyChanged")) {

        struct device *d;
        const char *profile;
        DBusMessageIter variant_i;
        dbus_bool_t connected;

        if (!dbus_message_iter_init(msg, &arg_i)) {
            pa_log("dbus: message has no parameters");
            goto done;
        }

        if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_STRING) {
            pa_log("Property name not a string.");
            goto done;
        }

        dbus_message_iter_get_basic(&arg_i, &value);

        if (!pa_streq(value, "Connected"))
            goto done;

        if (!dbus_message_iter_next(&arg_i)) {
            pa_log("Property value missing");
            goto done;
        }

        if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_VARIANT) {
            pa_log("Property value not a variant.");
            goto done;
        }

        dbus_message_iter_recurse(&arg_i, &variant_i);

        if (dbus_message_iter_get_arg_type(&variant_i) != DBUS_TYPE_BOOLEAN) {
            pa_log("Property value not a boolean.");
            goto done;
        }

        dbus_message_iter_get_basic(&variant_i, &connected);

        if (dbus_message_is_signal(msg, "org.bluez.Headset", "PropertyChanged"))
            profile = "hsp";
        else
            profile = "a2dp";

        d = device_find(u, dbus_message_get_path(msg));

        if (connected) {
            if (!d) {
                    d = device_new(dbus_message_get_path(msg));
                    PA_LLIST_PREPEND(struct device, u->device_list, d);
            }

            load_module_for_device(u, d, profile);
        } else if (d)
            unload_module_for_device(u, d, profile);
    }

done:
    dbus_error_free(&err);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}



static void get_properties_reply(DBusPendingCall *pending, void *user_data) {
    struct userdata *u;
    DBusMessage *r;
    dbus_bool_t connected;
    DBusMessageIter arg_i, element_i;
    DBusMessageIter variant_i;
    struct device *d;
    struct dbus_pending *p;

    pa_assert(u = user_data);

    r = dbus_pending_call_steal_reply(pending);
    if (!r)
        goto end;

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from GetProperties reply: %s", dbus_message_get_error_name(r));
        goto end;
    }

    if (!dbus_message_iter_init(r, &arg_i)) {
        pa_log("%s GetProperties reply has no arguments", p->profile);
        goto end;
    }

    if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_ARRAY) {
        pa_log("%s GetProperties argument is not an array", p->profile);
        goto end;
    }

    connected = FALSE;
    dbus_message_iter_recurse(&arg_i, &element_i);
    while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {

        if (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict_i;
            const char *key;

            dbus_message_iter_recurse(&element_i, &dict_i);

            if (dbus_message_iter_get_arg_type(&dict_i) != DBUS_TYPE_STRING) {
                pa_log("Property name not a string.");
                goto end;
            }

            dbus_message_iter_get_basic(&dict_i, &key);

            if (!dbus_message_iter_next(&dict_i))  {
                pa_log("Property value missing");
                goto end;
            }

            if (dbus_message_iter_get_arg_type(&dict_i) != DBUS_TYPE_VARIANT) {
                pa_log("Property value not a variant.");
                goto end;
            }

            dbus_message_iter_recurse(&dict_i, &variant_i);

            switch (dbus_message_iter_get_arg_type(&variant_i)) {

                case DBUS_TYPE_BOOLEAN: {

                    dbus_bool_t value;
                    dbus_message_iter_get_basic(&variant_i, &value);

                    if (pa_streq(key, "Connected")) {
                        connected = value;
                        goto endloop;
                    }

                    break;
                }
            }
        }

        if (!dbus_message_iter_next(&element_i))
            break;
    }

endloop:
    if (connected) {
        p = dbus_pending_call_get_data(pending, u->dbus_data_slot);
        pa_log_debug("%s: %s connected", p->path, p->profile);
        d = device_find(u, p->path);

        if (!d) {
            d = device_new(p->path);
            PA_LLIST_PREPEND(struct device, u->device_list, d);
        }

        load_module_for_device(u, d, p->profile);
    }

    dbus_message_unref(r);

end:
    p = dbus_pending_call_get_data(pending, u->dbus_data_slot);
    PA_LLIST_REMOVE(struct dbus_pending, u->dbus_pending_list, p);
    dbus_pending_free(p);
}

static void list_devices_reply(DBusPendingCall *pending, void *user_data) {
    DBusMessage *r, *m;
    DBusPendingCall *call;
    DBusError e;
    char **paths = NULL;
    int i, num = -1;
    struct dbus_pending *p;
    struct userdata *u;

    pa_assert(u = user_data);
    dbus_error_init(&e);

    r = dbus_pending_call_steal_reply(pending);
    if (!r) {
        pa_log("Failed to get ListDevices reply");
        goto end;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from ListDevices reply: %s", dbus_message_get_error_name(r));
        goto end;
    }

    if (!dbus_message_get_args(r, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &paths, &num, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.Adapter.ListDevices returned an error: '%s'\n", e.message);
        dbus_error_free(&e);
    } else {
        for (i = 0; i < num; ++i) {
            pa_assert_se(m = dbus_message_new_method_call("org.bluez", paths[i], "org.bluez.Headset", "GetProperties"));
            if (dbus_connection_send_with_reply(pa_dbus_connection_get(u->conn), m, &call, -1)) {
                p = dbus_pending_new(u, call, paths[i], "hsp");
                PA_LLIST_PREPEND(struct dbus_pending, u->dbus_pending_list, p);
                dbus_pending_call_set_notify(call, get_properties_reply, u, NULL);
            } else {
                pa_log("Failed to send GetProperties");
            }

            dbus_message_unref(m);

            pa_assert_se(m = dbus_message_new_method_call("org.bluez", paths[i], "org.bluez.AudioSink", "GetProperties"));
            if (dbus_connection_send_with_reply(pa_dbus_connection_get(u->conn), m, &call, -1)) {
                p = dbus_pending_new(u, call, paths[i], "a2dp");
                PA_LLIST_PREPEND(struct dbus_pending, u->dbus_pending_list, p);
                dbus_pending_call_set_notify(call, get_properties_reply, u, NULL);
            } else {
                pa_log("Failed to send GetProperties");
            }

            dbus_message_unref(m);
        }
    }

    if (paths)
        dbus_free_string_array (paths);
    dbus_message_unref(r);

end:
    p = dbus_pending_call_get_data(pending, u->dbus_data_slot);
    PA_LLIST_REMOVE(struct dbus_pending, u->dbus_pending_list, p);
    dbus_pending_free(p);
}

static void list_adapters_reply(DBusPendingCall *pending, void *user_data) {
    DBusMessage *r, *m;
    DBusPendingCall *call;
    DBusError e;
    char **paths = NULL;
    int i, num = -1;
    struct dbus_pending *p;
    struct userdata *u;

    pa_assert(u = user_data);
    dbus_error_init(&e);

    r = dbus_pending_call_steal_reply(pending);
    if (!r) {
        pa_log("Failed to get ListAdapters reply");
        goto end;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("Error from ListAdapters reply: %s", dbus_message_get_error_name(r));
        goto end;
    }

    if (!dbus_message_get_args(r, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &paths, &num, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.Manager.ListAdapters returned an error: '%s'\n", e.message);
        dbus_error_free(&e);
    } else {
        for (i = 0; i < num; ++i) {
            pa_assert_se(m = dbus_message_new_method_call("org.bluez", paths[i], "org.bluez.Adapter", "ListDevices"));
            if (dbus_connection_send_with_reply(pa_dbus_connection_get(u->conn), m, &call, -1)) {
                p = dbus_pending_new(u, call, NULL, NULL);
                PA_LLIST_PREPEND(struct dbus_pending, u->dbus_pending_list, p);
                dbus_pending_call_set_notify(call, list_devices_reply, u, NULL);
            } else {
                pa_log("Failed to send ListDevices");
            }

            dbus_message_unref(m);
        }
    }

    if (paths)
        dbus_free_string_array (paths);
    dbus_message_unref(r);

end:
    p = dbus_pending_call_get_data(pending, u->dbus_data_slot);
    PA_LLIST_REMOVE(struct dbus_pending, u->dbus_pending_list, p);
    dbus_pending_free(p);
}

static void lookup_devices(struct userdata *u) {
    DBusMessage *m;
    DBusPendingCall *call;
    struct dbus_pending *p;

    pa_assert(u);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", "/", "org.bluez.Manager", "ListAdapters"));
    if (dbus_connection_send_with_reply(pa_dbus_connection_get(u->conn), m, &call, -1)) {
        p = dbus_pending_new(u, call, NULL, NULL);
        PA_LLIST_PREPEND(struct dbus_pending, u->dbus_pending_list, p);
        dbus_pending_call_set_notify(call, list_adapters_reply, u, NULL);
    } else {
        pa_log("Failed to send ListAdapters");
    }

    dbus_message_unref(m);
}

void pa__done(pa_module* m) {
    struct userdata *u;
    struct device *i;
    struct dbus_pending *p;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    while ((p = u->dbus_pending_list)) {
        PA_LLIST_REMOVE(struct dbus_pending, u->dbus_pending_list, p);
        dbus_pending_free(p);
    }

    while ((i = u->device_list)) {
        PA_LLIST_REMOVE(struct device, u->device_list, i);
        device_free(i);
    }

    if (u->dbus_data_slot != -1) {
        dbus_pending_call_free_data_slot(&u->dbus_data_slot);
    }

    if (u->conn) {
        DBusError error;
        dbus_error_init(&error);

        dbus_bus_remove_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'", &error);
        dbus_error_free(&error);

        dbus_bus_remove_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'", &error);
        dbus_error_free(&error);

        dbus_bus_remove_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'", &error);
        dbus_error_free(&error);

        dbus_connection_remove_filter(pa_dbus_connection_get(u->conn), filter_cb, u);

        pa_dbus_connection_unref(u->conn);
    }

    pa_xfree(u);
}

int pa__init(pa_module* m) {
    DBusError err;
    struct userdata *u;

    pa_assert(m);
    dbus_error_init(&err);

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->dbus_data_slot = -1;
    u->module = m;
    PA_LLIST_HEAD_INIT(struct device, u->device_list);
    PA_LLIST_HEAD_INIT(DBusPendingCall, u->dbus_pending_list);

    /* connect to the bus */
    u->conn = pa_dbus_bus_get(m->core, DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err) || (u->conn == NULL) ) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        goto fail;
    }

    if (!dbus_pending_call_allocate_data_slot(&u->dbus_data_slot))
        goto fail;

    /* dynamic detection of bluetooth audio devices */
    if (!dbus_connection_add_filter(pa_dbus_connection_get(u->conn), filter_cb, u, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.Adapter signals: %s: %s", err.name, err.message);
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.Headset signals: %s: %s", err.name, err.message);
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.AudioSink signals: %s: %s", err.name, err.message);
        goto fail;
    }

    lookup_devices(u);

    return 0;

fail:
    dbus_error_free(&err);
    pa__done(m);

    return -1;
}
