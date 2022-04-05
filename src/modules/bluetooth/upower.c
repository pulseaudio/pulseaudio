/***
  This file is part of PulseAudio.

  Copyright 2022 Dylan Van Assche <me@dylanvanassche.be>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/log.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include "upower.h"

static pa_dbus_pending* send_and_add_to_pending(pa_upower_backend *backend, DBusMessage *m,
        DBusPendingCallNotifyFunction func, void *call_data) {

    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(backend);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(pa_dbus_connection_get(backend->connection), m, &call, -1));

    p = pa_dbus_pending_new(pa_dbus_connection_get(backend->connection), m, call, backend, call_data);
    PA_LLIST_PREPEND(pa_dbus_pending, backend->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static void parse_percentage(pa_upower_backend *b, DBusMessageIter *i) {
    double percentage;
    unsigned int battery_level;

    pa_assert(i);
    pa_assert(dbus_message_iter_get_arg_type(i) == DBUS_TYPE_DOUBLE);

    dbus_message_iter_get_basic(i, &percentage);
    battery_level = (unsigned int) round(percentage / 20.0);

    if (battery_level != b->battery_level) {
        b->battery_level = battery_level;
        pa_log_debug("AG battery level updated (%d/5)", b->battery_level);
        pa_hook_fire(pa_bluetooth_discovery_hook(b->discovery, PA_BLUETOOTH_HOOK_HOST_BATTERY_LEVEL_CHANGED), b);
    }
}

static void get_percentage_reply(DBusPendingCall *pending, void *userdata) {
    pa_dbus_pending *p;
    pa_upower_backend *b;
    DBusMessage *r;
    DBusMessageIter arg_i, variant_i;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(b = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
        pa_log_warn("UPower D-Bus Display Device not available");
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error("Get() failed: %s: %s", dbus_message_get_error_name(r), pa_dbus_get_error_message(r));
        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i) || !pa_streq(dbus_message_get_signature(r), "v")) {
        pa_log_error("Invalid reply signature for Get()");
        goto finish;
    }

    dbus_message_iter_recurse(&arg_i, &variant_i);
    parse_percentage(b, &variant_i);

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, b->pending, p);
    pa_dbus_pending_free(p);
}

static const char *check_variant_property(DBusMessageIter *i) {
    const char *key;

    pa_assert(i);

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_STRING) {
        pa_log_error("Property name not a string.");
        return NULL;
    }

    dbus_message_iter_get_basic(i, &key);

    if (!dbus_message_iter_next(i)) {
        pa_log_error("Property value missing");
        return NULL;
    }

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_VARIANT) {
        pa_log_error("Property value not a variant.");
        return NULL;
    }

    return key;
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *data) {
    DBusError err;
    DBusMessage *m2;
    static const char* upower_device_interface = UPOWER_SERVICE UPOWER_DEVICE_INTERFACE;
    static const char* percentage_property = "Percentage";
    pa_upower_backend *b = data;
    const char *path, *interface, *member;

    pa_assert(bus);
    pa_assert(m);
    pa_assert(b);

    dbus_error_init(&err);

    path = dbus_message_get_path(m);
    interface = dbus_message_get_interface(m);
    member = dbus_message_get_member(m);

    pa_log_debug("dbus: path=%s, interface=%s, member=%s", path, interface, member);

    /* UPower D-Bus status change */
    if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;

        if (!dbus_message_get_args(m, &err,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            pa_log_error("Failed to parse " DBUS_INTERFACE_DBUS ".NameOwnerChanged: %s", err.message);
            goto fail;
        }

        if (pa_streq(name, UPOWER_SERVICE)) {

            /* UPower disappeared from D-Bus */
            if (old_owner && *old_owner) {
                pa_log_debug("UPower disappeared from D-Bus");
                b->battery_level = 0;
                pa_hook_fire(pa_bluetooth_discovery_hook(b->discovery, PA_BLUETOOTH_HOOK_HOST_BATTERY_LEVEL_CHANGED), b);
            }

            /* UPower appeared on D-Bus */
            if (new_owner && *new_owner) {
                pa_log_debug("UPower appeared on D-Bus");

                /* Update battery level */
                pa_assert_se(m2 = dbus_message_new_method_call(UPOWER_SERVICE, UPOWER_DISPLAY_DEVICE_OBJECT, DBUS_INTERFACE_PROPERTIES, "Get"));
                pa_assert_se(dbus_message_append_args(m2,
                    DBUS_TYPE_STRING, &upower_device_interface,
                    DBUS_TYPE_STRING, &percentage_property,
                    DBUS_TYPE_INVALID));
                send_and_add_to_pending(b, m2, get_percentage_reply, NULL);
            }
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    /* UPower battery level property updates */
    } else if (dbus_message_is_signal(m, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged")) {
        DBusMessageIter arg_i, element_i;

        if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "sa{sv}as")) {
            pa_log_error("Invalid signature found in PropertiesChanged");
            goto fail;
        }

        /* Skip interface name */
        pa_assert_se(dbus_message_iter_next(&arg_i));
        pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_ARRAY);

        dbus_message_iter_recurse(&arg_i, &element_i);

        /* Parse UPower property updates */
        while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict_i, variant_i;
            const char *key;

            dbus_message_iter_recurse(&element_i, &dict_i);

            /* Retrieve property name */
            key = check_variant_property(&dict_i);
            if (key == NULL) {
                pa_log_error("Received invalid property!");
                break;
            }

            dbus_message_iter_recurse(&dict_i, &variant_i);

            if(pa_streq(path, UPOWER_DISPLAY_DEVICE_OBJECT)) {
                pa_log_debug("UPower Device property updated: %s", key);

                if(pa_streq(key, "Percentage"))
                    parse_percentage(b, &variant_i);
            }

            dbus_message_iter_next(&element_i);
        }
    }

fail:
    dbus_error_free(&err);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

unsigned int pa_upower_get_battery_level(pa_upower_backend *backend) {
    return backend->battery_level;
}

pa_upower_backend *pa_upower_backend_new(pa_core *c, pa_bluetooth_discovery *d) {
    pa_upower_backend *backend;
    DBusError err;
    DBusMessage *m;
    static const char* upower_device_interface = UPOWER_SERVICE UPOWER_DEVICE_INTERFACE;
    static const char* percentage_property = "Percentage";

    pa_log_debug("Native backend enabled UPower battery status reporting");

    backend = pa_xnew0(pa_upower_backend, 1);
    backend->core = c;
    backend->discovery = d;

    /* Get DBus connection */
    dbus_error_init(&err);
    if (!(backend->connection = pa_dbus_bus_get(c, DBUS_BUS_SYSTEM, &err))) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        dbus_error_free(&err);
        pa_xfree(backend);
        return NULL;
    }

    /* Add filter callback for DBus connection */
    if (!dbus_connection_add_filter(pa_dbus_connection_get(backend->connection), filter_cb, backend, NULL)) {
        pa_log_error("Failed to add filter function");
        pa_dbus_connection_unref(backend->connection);
        pa_xfree(backend);
        return NULL;
    }

    /* Register for battery level changes from UPower */
    if (pa_dbus_add_matches(pa_dbus_connection_get(backend->connection), &err,
            "type='signal',sender='" DBUS_SERVICE_DBUS "',interface='" DBUS_INTERFACE_DBUS "',member='NameOwnerChanged',"
            "arg0='" UPOWER_SERVICE "'",
            "type='signal',sender='" UPOWER_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',member='PropertiesChanged'",
            NULL) < 0) {
        pa_log("Failed to add UPower D-Bus matches: %s", err.message);
        dbus_connection_remove_filter(pa_dbus_connection_get(backend->connection), filter_cb, backend);
        pa_dbus_connection_unref(backend->connection);
        pa_xfree(backend);
        return NULL;
    }

    /* Initialize battery level by requesting it from UPower */
    pa_assert_se(m = dbus_message_new_method_call(UPOWER_SERVICE, UPOWER_DISPLAY_DEVICE_OBJECT, DBUS_INTERFACE_PROPERTIES, "Get"));
    pa_assert_se(dbus_message_append_args(m,
        DBUS_TYPE_STRING, &upower_device_interface,
        DBUS_TYPE_STRING, &percentage_property,
        DBUS_TYPE_INVALID));
    send_and_add_to_pending(backend, m, get_percentage_reply, NULL);

    return backend;
}

void pa_upower_backend_free(pa_upower_backend *backend) {
    pa_assert(backend);

    pa_dbus_free_pending_list(&backend->pending);

    pa_dbus_connection_unref(backend->connection);

    pa_xfree(backend);
}

