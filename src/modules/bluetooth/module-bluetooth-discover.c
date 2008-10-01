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

#include "dbus-util.h"
#include "module-bluetooth-discover-symdef.h"

PA_MODULE_AUTHOR("Joao Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Detect available bluetooth audio devices and load bluetooth audio drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE("");

#define HSP_HS_UUID             "00001108-0000-1000-8000-00805F9B34FB"
#define HFP_HS_UUID             "0000111E-0000-1000-8000-00805F9B34FB"
#define A2DP_SOURCE_UUID        "0000110A-0000-1000-8000-00805F9B34FB"
#define A2DP_SINK_UUID          "0000110B-0000-1000-8000-00805F9B34FB"

struct uuid {
    char *uuid;
    PA_LLIST_FIELDS(struct uuid);
};

struct device {
    char *name;
    char *object_path;
    int paired;
    struct adapter *adapter;
    char *alias;
    int connected;
    PA_LLIST_HEAD(struct uuid, uuid_list);
    char *address;
    int class;
    int trusted;
    const char *audio_profile;
    uint32_t module_index;
    PA_LLIST_FIELDS(struct device);
};

struct adapter {
    char *object_path;
    char *name;
    char *mode;
    char *address;
    PA_LLIST_HEAD(struct device, device_list);
    PA_LLIST_FIELDS(struct adapter);
};

struct userdata {
    pa_module *module;
    pa_dbus_connection *conn;
    PA_LLIST_HEAD(struct adapter, adapter_list);
};

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

static struct device *device_new(struct adapter *adapter, const char *object_path) {
    struct device *node;

    node = pa_xnew(struct device, 1);
    node->name = NULL;
    node->object_path = pa_xstrdup(object_path);
    node->paired = -1;
    node->adapter = adapter;
    node->alias = NULL;
    node->connected = -1;
    PA_LLIST_HEAD_INIT(struct uuid, node->uuid_list);
    node->address = NULL;
    node->class = -1;
    node->trusted = -1;
    node->audio_profile = NULL;
    node->module_index = PA_INVALID_INDEX;
    PA_LLIST_INIT(struct device, node);

    return node;
}

static void device_free(struct device *device) {
    struct uuid *i;

    pa_assert(device);

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

static struct adapter *adapter_new(const char *object_path) {
    struct adapter *node;

    node = pa_xnew(struct adapter, 1);
    node->object_path = pa_xstrdup(object_path);
    node->mode = NULL;
    node->address = NULL;
    node->name = NULL;

    PA_LLIST_HEAD_INIT(struct device, node->device_list);
    PA_LLIST_INIT(struct adapter, node);

    return node;
}

static void adapter_free(struct adapter *adapter) {
    struct device *i;

    pa_assert(adapter);

    while ((i = adapter->device_list)) {
        PA_LLIST_REMOVE(struct device, adapter->device_list, i);
        device_free(i);
    }

    pa_xfree(adapter->object_path);
    pa_xfree(adapter->mode);
    pa_xfree(adapter->address);
    pa_xfree(adapter->name);
    pa_xfree(adapter);
}

static struct adapter* adapter_find(struct userdata *u, const char *path) {
    struct adapter *i;

    for (i = u->adapter_list; i; i = i->next)
        if (pa_streq(i->object_path, path))
            return i;

    return NULL;
}

static struct device* device_find(struct userdata *u, const char *path) {
    struct adapter *j;
    struct device *i;

    for (j = u->adapter_list; j; j = j->next)
        for (i = j->device_list; i; i = i->next)
            if (pa_streq(i->object_path, path))
                return i;

    return NULL;
}

static const char *yes_no_na(int b) {
    if (b < 0)
        return "n/a";

    return pa_yes_no(b);
}

static void print_devices(struct adapter *a) {
    struct device *i;

    pa_assert(a);

    for (i = a->device_list; i; i = i->next) {
        struct uuid *j;

        if (pa_streq(i->object_path, "/DEVICE_HEAD"))
            continue;

        pa_log_debug("\t[ %s ]\n"
                     "\t\tName = %s\n"
                     "\t\tPaired = %s\n"
                     "\t\tAdapter = %s\n"
                     "\t\tAlias = %s\n"
                     "\t\tConnected = %s\n"
                     "\t\tAudio = %s\n",
                     i->object_path,
                     pa_strnull(i->name),
                     yes_no_na(i->paired),
                     i->adapter->object_path,
                     pa_strnull(i->alias),
                     yes_no_na(i->connected),
                     pa_strnull(i->audio_profile));

        pa_log_debug("\t\tUUIDs = ");
        for (j = i->uuid_list; j; j = j->next) {

            if (pa_streq(j->uuid, "UUID_HEAD"))
                continue;

            pa_log_debug("\t\t         %s", j->uuid);
        }

        pa_log_debug("\t\tAddress = %s\n"
                     "\t\tClass = 0x%x\n"
                     "\t\tTrusted = %s",
                     i->address,
                     i->class,
                     yes_no_na(i->trusted));
    }
}

static void print_adapters(struct userdata *u) {
    struct adapter *i;

    pa_assert(u);

    for (i = u->adapter_list; i; i = i->next) {

        if (pa_streq(i->object_path, "/ADAPTER_HEAD"))
            continue;

        pa_log_debug(
                "[ %s ]\n"
                "\tName = %s\n"
                "\tMode = %s\n"
                "\tAddress = %s\n",
                i->object_path,
                pa_strnull(i->name),
                pa_strnull(i->mode),
                pa_strnull(i->address));

        print_devices(i);
    }
}

static const char *strip_object_path(const char *op) {
    const char *slash;

    if ((slash = strrchr(op, '/')))
        return slash+1;

    return op;
}

static void load_module_for_device(struct userdata *u, struct device *d) {
    char *args;
    pa_module *m;

    pa_assert(u);
    pa_assert(d);

    /* Check whether we already loaded a module for this device */
    if (d->module_index != PA_INVALID_INDEX &&
        pa_idxset_get_by_index(u->module->core->modules, d->module_index))
        return;

    /* Check whether this is an audio device */
    if (!d->audio_profile) {
        pa_log_debug("Ignoring %s since it is not an audio device.", d->object_path);
        return;
    }

    args = pa_sprintf_malloc("sink_name=%s address=%s profile=%s", strip_object_path(d->object_path), d->address, d->audio_profile);
    m = pa_module_load(u->module->core, "module-bluetooth-device", args);
    pa_xfree(args);

    if (!m) {
        pa_log_debug("Failed to load module for device %s", d->object_path);
        return;
    }

    d->module_index = m->index;
}

static void load_modules(struct userdata *u) {
    struct device *d;
    struct adapter *a;

    pa_assert(u);

    for (a = u->adapter_list; a; a = a->next)
        for (d = a->device_list; d; d = d->next)
            load_module_for_device(u, d);
}

static int parse_adapter_property(struct userdata *u, struct adapter *a, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(u);
    pa_assert(a);
    pa_assert(i);

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_STRING) {
        pa_log("Property name not a string.");
        return -1;
    }

    dbus_message_iter_get_basic(i, &key);

    if (!dbus_message_iter_next(i)) {
        pa_log("Property value missing");
        return -1;
    }

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_VARIANT) {
        pa_log("Property value not a variant.");
        return -1;
    }

    dbus_message_iter_recurse(i, &variant_i);

    if (dbus_message_iter_get_arg_type(&variant_i) == DBUS_TYPE_STRING) {
        const char *value;
        dbus_message_iter_get_basic(&variant_i, &value);

        if (pa_streq(key, "Mode")) {
            pa_xfree(a->mode);
            a->mode = pa_xstrdup(value);
        } else if (pa_streq(key, "Address")) {
            pa_xstrdup(a->address);
            a->address = pa_xstrdup(value);
        } else if (pa_streq(key, "Name")) {
            pa_xfree(a->name);
            a->name = pa_xstrdup(value);
        }
    }

    return 0;
}

static int get_adapter_properties(struct userdata *u, struct adapter *a) {
    DBusError e;
    DBusMessage *m = NULL, *r = NULL;
    DBusMessageIter arg_i, element_i;
    int ret = -1;

    pa_assert(u);
    pa_assert(a);
    dbus_error_init(&e);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", a->object_path, "org.bluez.Adapter", "GetProperties"));

    r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->conn), m, -1, &e);

    if (!r) {
        pa_log("org.bluez.Adapter.GetProperties failed: %s", e.message);
        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i)) {
        pa_log("org.bluez.Adapter.GetProperties reply has no arguments");
        goto finish;
    }

    if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_ARRAY) {
        pa_log("org.bluez.Adapter.GetProperties argument is not an array");
        goto finish;
    }

    dbus_message_iter_recurse(&arg_i, &element_i);
    while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {

        if (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict_i;

            dbus_message_iter_recurse(&element_i, &dict_i);

            if (parse_adapter_property(u, a, &dict_i) < 0)
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

static int detect_adapters(struct userdata *u) {
    DBusError e;
    DBusMessage *m = NULL, *r = NULL;
    DBusMessageIter arg_i, element_i;
    struct adapter *adapter_list_i;
    int ret = -1;

    pa_assert(u);
    dbus_error_init(&e);

    /* get adapters */
    pa_assert_se(m = dbus_message_new_method_call("org.bluez", "/", "org.bluez.Manager", "ListAdapters"));
    r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->conn), m, -1, &e);

    if (!r) {
        pa_log("org.bluez.Manager.ListAdapters failed: %s", e.message);
        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i)) {
        pa_log("org.bluez.Manager.ListAdapters reply has no arguments");
        goto finish;
    }

    if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_ARRAY) {
        pa_log("org.bluez.Manager.ListAdapters argument is not an array");
        goto finish;
    }

    dbus_message_iter_recurse(&arg_i, &element_i);
    while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_OBJECT_PATH) {

            struct adapter *node;
            const char *value;

            dbus_message_iter_get_basic(&element_i, &value);
            node = adapter_new(value);
            PA_LLIST_PREPEND(struct adapter, u->adapter_list, node);
        }

        if (!dbus_message_iter_next(&element_i))
            break;
    }

    ret = 0;

    /* get adapter properties */
    for (adapter_list_i = u->adapter_list; adapter_list_i; adapter_list_i = adapter_list_i->next)
        get_adapter_properties(u, adapter_list_i);

finish:
    if (m)
        dbus_message_unref(m);
    if (r)
        dbus_message_unref(r);

    dbus_error_free(&e);
    return ret;
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

                d->audio_profile = NULL;

                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    struct uuid *node;
                    const char *value;

                    dbus_message_iter_get_basic(&ai, &value);
                    node = uuid_new(value);
                    PA_LLIST_PREPEND(struct uuid, d->uuid_list, node);

                    if ((strcasecmp(value, A2DP_SOURCE_UUID) == 0) ||
                        (strcasecmp(value, A2DP_SINK_UUID) == 0))
                        d->audio_profile = "a2dp";
                    else if (((strcasecmp(value, HSP_HS_UUID) == 0) ||
                              (strcasecmp(value, HFP_HS_UUID) == 0)) &&
                             !d->audio_profile)
                        d->audio_profile = "hsp";

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

static int detect_devices(struct userdata *u) {
    DBusError e;
    DBusMessage *m = NULL, *r = NULL;
    DBusMessageIter arg_i, element_i;
    struct adapter *adapter_list_i;
    struct device *device_list_i;
    const char *value;
    int ret = -1;

    pa_assert(u);
    dbus_error_init(&e);

    /* get devices of each adapter */
    for (adapter_list_i = u->adapter_list; adapter_list_i; adapter_list_i = adapter_list_i->next) {

        pa_assert_se(m = dbus_message_new_method_call("org.bluez", adapter_list_i->object_path, "org.bluez.Adapter", "ListDevices"));

        r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->conn), m, -1, &e);

        if (!r) {
            pa_log("org.bluez.Adapter.ListDevices failed: %s", e.message);
            goto finish;
        }

        if (!dbus_message_iter_init(r, &arg_i)) {
            pa_log("org.bluez.Adapter.ListDevices reply has no arguments");
            goto finish;
        }

        if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_ARRAY) {
            pa_log("org.bluez.Adapter.ListDevices argument is not an array");
            goto finish;
        }

        dbus_message_iter_recurse(&arg_i, &element_i);
        while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {
            if (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_OBJECT_PATH) {
                struct device *node;
                dbus_message_iter_get_basic(&element_i, &value);
                node = device_new(adapter_list_i, value);
                PA_LLIST_PREPEND(struct device, adapter_list_i->device_list, node);
            }

            if (!dbus_message_iter_next(&element_i))
                break;
        }
    }

    /* get device properties */
    for (adapter_list_i = u->adapter_list; adapter_list_i; adapter_list_i = adapter_list_i->next)
        for (device_list_i = adapter_list_i->device_list; device_list_i; device_list_i = device_list_i->next)
            get_device_properties(u, device_list_i);

    ret = 0;

finish:
    if (m)
        dbus_message_unref(m);
    if (r)
        dbus_message_unref(r);

    dbus_error_free(&e);

    return ret;
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

    if (dbus_message_is_signal(msg, "org.bluez.Manager", "AdapterAdded")) {

        if (!dbus_message_iter_init(msg, &arg_i))
            pa_log("dbus: message has no parameters");
        else if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_OBJECT_PATH)
            pa_log("dbus: argument is not object path");
        else {
            struct adapter *node;

            dbus_message_iter_get_basic(&arg_i, &value);
            pa_log_debug("hcid: adapter %s added", value);

            node = adapter_new(value);
            PA_LLIST_PREPEND(struct adapter, u->adapter_list, node);

            get_adapter_properties(u, node);
        }

    } else if (dbus_message_is_signal(msg, "org.bluez.Manager", "AdapterRemoved")) {
        if (!dbus_message_iter_init(msg, &arg_i))
            pa_log("dbus: message has no parameters");
        else if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_OBJECT_PATH)
            pa_log("dbus: argument is not object path");
        else {
            struct adapter *a;

            dbus_message_iter_get_basic(&arg_i, &value);
            pa_log_debug("hcid: adapter %s removed", value);

            if ((a = adapter_find(u, value))) {
                PA_LLIST_REMOVE(struct adapter, u->adapter_list, a);
                adapter_free(a);
            }
        }

    } else if (dbus_message_is_signal(msg, "org.bluez.Adapter", "PropertyChanged")) {

        if (!dbus_message_iter_init(msg, &arg_i))
            pa_log("dbus: message has no parameters");
        else {
            struct adapter *a;

            if ((a = adapter_find(u, dbus_message_get_path(msg))))
                parse_adapter_property(u, a, &arg_i);
        }

    } else if (dbus_message_is_signal(msg, "org.bluez.Adapter", "DeviceCreated")) {

        if (!dbus_message_iter_init(msg, &arg_i))
            pa_log("dbus: message has no parameters");
        else if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_OBJECT_PATH)
            pa_log("dbus: argument is not object path");
        else {
            struct adapter *adapter;

            if (!(adapter = adapter_find(u, dbus_message_get_path(msg))))
                pa_log("dbus: failed to find adapter for object path");
            else {
                struct device *node;

                dbus_message_iter_get_basic(&arg_i, &value);
                pa_log_debug("hcid: device %s created", value);

                node = device_new(adapter, value);
                PA_LLIST_PREPEND(struct device, adapter->device_list, node);

                get_device_properties(u, node);
                load_module_for_device(u, node);
            }
        }

    } else if (dbus_message_is_signal(msg, "org.bluez.Adapter", "DeviceRemoved")) {

        if (!dbus_message_iter_init(msg, &arg_i))
            pa_log("dbus: message has no parameters");
        else if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_OBJECT_PATH)
            pa_log("dbus: argument is not object path");
        else {
            struct device *d;

            dbus_message_iter_get_basic(&arg_i, &value);
            pa_log_debug("hcid: device %s removed", value);

            if ((d = device_find(u, value))) {
                PA_LLIST_REMOVE(struct device, d->adapter->device_list, d);
                device_free(d);
            }
        }

    } else if (dbus_message_is_signal(msg, "org.bluez.Headset", "Connected") ||
               dbus_message_is_signal(msg, "org.bluez.AudioSink", "Connected")) {

        struct device *d;

        if ((d = device_find(u, dbus_message_get_path(msg))))
                load_module_for_device(u, d);
    }

    dbus_error_free(&err);
    return DBUS_HANDLER_RESULT_HANDLED;
}

void pa__done(pa_module* m) {
    struct userdata *u;
    struct adapter *i;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    while ((i = u->adapter_list)) {
        PA_LLIST_REMOVE(struct adapter, u->adapter_list, i);
        adapter_free(i);
    }

    if (u->conn)
        pa_dbus_connection_unref(u->conn);

    pa_xfree(u);
}

int pa__init(pa_module* m) {
    DBusError err;
    struct userdata *u;

    pa_assert(m);
    dbus_error_init(&err);

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->module = m;
    PA_LLIST_HEAD_INIT(struct adapter, u->adapter_list);

    /* connect to the bus */
    u->conn = pa_dbus_bus_get(m->core, DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err) || (u->conn == NULL) ) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        goto fail;
    }

    /* static detection of bluetooth audio devices */
    detect_adapters(u);
    detect_devices(u);

    print_adapters(u);
    load_modules(u);

    /* dynamic detection of bluetooth audio devices */
    if (!dbus_connection_add_filter(pa_dbus_connection_get(u->conn), filter_cb, u, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Manager'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.Manager signals: %s: %s", err.name, err.message);
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Adapter'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.Adapter signals: %s: %s", err.name, err.message);
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Device'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.Device signals: %s: %s", err.name, err.message);
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='Connected'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.Headset signals: %s: %s", err.name, err.message);
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(u->conn), "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='Connected'", &err);
    if (dbus_error_is_set(&err)) {
        pa_log_error("Unable to subscribe to org.bluez.AudioSink signals: %s: %s", err.name, err.message);
        goto fail;
    }

    return 0;

fail:
    dbus_error_free(&err);
    pa__done(m);

    return -1;
}
