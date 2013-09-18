/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita

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

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>

#include "bluez5-util.h"

#define BLUEZ_SERVICE "org.bluez"

struct pa_bluetooth_discovery {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_dbus_connection *connection;
    bool filter_added;
    bool matches_added;
};

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y;
    DBusError err;

    pa_assert(bus);
    pa_assert(m);
    pa_assert_se(y = userdata);

    dbus_error_init(&err);

    if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;

        if (!dbus_message_get_args(m, &err,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            pa_log_error("Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
            goto fail;
        }

        if (pa_streq(name, BLUEZ_SERVICE)) {
            if (old_owner && *old_owner) {
                pa_log_debug("Bluetooth daemon disappeared");
                /* TODO: remove all devices */
            }

            if (new_owner && *new_owner) {
                pa_log_debug("Bluetooth daemon appeared");
                /* TODO: get managed objects */
            }
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

fail:
    dbus_error_free(&err);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *c) {
    pa_bluetooth_discovery *y;
    DBusError err;
    DBusConnection *conn;

    if ((y = pa_shared_get(c, "bluetooth-discovery")))
        return pa_bluetooth_discovery_ref(y);

    y = pa_xnew0(pa_bluetooth_discovery, 1);
    PA_REFCNT_INIT(y);
    y->core = c;

    pa_shared_set(c, "bluetooth-discovery", y);

    dbus_error_init(&err);

    if (!(y->connection = pa_dbus_bus_get(y->core, DBUS_BUS_SYSTEM, &err))) {
        pa_log_error("Failed to get D-Bus connection: %s", err.message);
        goto fail;
    }

    conn = pa_dbus_connection_get(y->connection);

    /* dynamic detection of bluetooth audio devices */
    if (!dbus_connection_add_filter(conn, filter_cb, y, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }
    y->filter_added = true;

    if (pa_dbus_add_matches(conn, &err,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'"
            ",arg0='" BLUEZ_SERVICE "'",
            NULL) < 0) {
        pa_log_error("Failed to add D-Bus matches: %s", err.message);
        goto fail;
    }
    y->matches_added = true;

    return y;

fail:
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

    if (y->connection) {

        if (y->matches_added)
            pa_dbus_remove_matches(pa_dbus_connection_get(y->connection),
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                "arg0='" BLUEZ_SERVICE "'",
                NULL);

        if (y->filter_added)
            dbus_connection_remove_filter(pa_dbus_connection_get(y->connection), filter_cb, y);

        pa_dbus_connection_unref(y->connection);
    }

    pa_shared_remove(y->core, "bluetooth-discovery");
    pa_xfree(y);
}
