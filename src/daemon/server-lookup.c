/***
  This file is part of PulseAudio.

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

#include <dbus/dbus.h>

#include <pulse/client-conf.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-common.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/macro.h>

#include "server-lookup.h"

struct pa_dbusobj_server_lookup {
    pa_core *core;
    pa_dbus_connection *conn;
    pa_bool_t path_registered;
};

static const char introspection[] =
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE
    "<node>"
    " <!-- If you are looking for documentation make sure to check out\n"
    "      http://pulseaudio.org/wiki/DBusInterface -->\n"
    " <interface name=\"org.pulseaudio.ServerLookup\">"
    "  <method name=\"GetAddress\">"
    "   <arg name=\"result\" type=\"s\" direction=\"out\"/>"
    "  </method>"
    " </interface>"
    " <interface name=\"org.freedesktop.DBus.Introspectable\">"
    "  <method name=\"Introspect\">"
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"
    "  </method>"
    " </interface>"
    "</node>";

static void unregister_cb(DBusConnection *conn, void *user_data) {
    pa_dbusobj_server_lookup *sl = user_data;

    pa_assert(sl);
    pa_assert(sl->path_registered);

    sl->path_registered = FALSE;
}

static DBusHandlerResult handle_introspect(DBusConnection *conn, DBusMessage *msg, pa_dbusobj_server_lookup *sl) {
    const char *i = introspection;
    DBusMessage *reply = NULL;

    pa_assert(conn);
    pa_assert(msg);

    if (!(reply = dbus_message_new_method_return(msg)))
        goto fail;

    if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &i, DBUS_TYPE_INVALID))
        goto fail;

    if (!dbus_connection_send(conn, reply, NULL))
        goto oom;

    dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_HANDLED;

fail:
    if (reply)
        dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

oom:
    if (reply)
        dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult handle_get_address(DBusConnection *conn, DBusMessage *msg, pa_dbusobj_server_lookup *sl) {
    DBusMessage *reply = NULL;
    pa_client_conf *conf = NULL;
    char *address = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(sl);

    conf = pa_client_conf_new();

    if (pa_client_conf_load(conf, NULL) < 0) {
        if (!(reply = dbus_message_new_error(msg, "org.pulseaudio.ClientConfLoadError", "Failed to load client.conf.")))
            goto fail;
        if (!dbus_connection_send(conn, reply, NULL))
            goto oom;
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (conf->default_dbus_server) {
        address = pa_xstrdup(conf->default_dbus_server);
    } else {
        if (!(address = pa_get_dbus_address_from_server_type(sl->core->server_type))) {
            if (!(reply = dbus_message_new_error(msg, DBUS_ERROR_FAILED, "PulseAudio internal error: get_dbus_server_from_type() failed.")))
                goto fail;
            if (!dbus_connection_send(conn, reply, NULL))
                goto oom;
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    if (!(reply = dbus_message_new_method_return(msg)))
        goto oom;

    if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &address, DBUS_TYPE_INVALID))
        goto fail;

    if (!dbus_connection_send(conn, reply, NULL))
        goto oom;

    pa_client_conf_free(conf);
    pa_xfree(address);
    dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_HANDLED;

fail:
    if (conf)
        pa_client_conf_free(conf);

    pa_xfree(address);

    if (reply)
        dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

oom:
    if (conf)
        pa_client_conf_free(conf);

    pa_xfree(address);

    if (reply)
        dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult message_cb(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    pa_dbusobj_server_lookup *sl = user_data;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(sl);

    /* pa_log("Got message! type = %s   path = %s   iface = %s   member = %s   dest = %s", dbus_message_type_to_string(dbus_message_get_type(msg)), dbus_message_get_path(msg), dbus_message_get_interface(msg), dbus_message_get_member(msg), dbus_message_get_destination(msg)); */

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect"))
        return handle_introspect(conn, msg, sl);

    if (dbus_message_is_method_call(msg, "org.pulseaudio.ServerLookup", "GetAddress"))
        return handle_get_address(conn, msg, sl);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable vtable = {
    .unregister_function = unregister_cb,
    .message_function = message_cb,
    .dbus_internal_pad1 = NULL,
    .dbus_internal_pad2 = NULL,
    .dbus_internal_pad3 = NULL,
    .dbus_internal_pad4 = NULL
};

pa_dbusobj_server_lookup *pa_dbusobj_server_lookup_new(pa_core *c) {
    pa_dbusobj_server_lookup *sl;
    DBusError error;

    dbus_error_init(&error);

    sl = pa_xnew(pa_dbusobj_server_lookup, 1);
    sl->core = c;
    sl->path_registered = FALSE;

    if (!(sl->conn = pa_dbus_bus_get(c, DBUS_BUS_SESSION, &error)) || dbus_error_is_set(&error)) {
        pa_log("Unable to contact D-Bus: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!dbus_connection_register_object_path(pa_dbus_connection_get(sl->conn), "/org/pulseaudio/server_lookup", &vtable, sl)) {
        pa_log("dbus_connection_register_object_path() failed for /org/pulseaudio/server_lookup.");
        goto fail;
    }

    sl->path_registered = TRUE;

    return sl;

fail:
    dbus_error_free(&error);

    pa_dbusobj_server_lookup_free(sl);

    return NULL;
}

void pa_dbusobj_server_lookup_free(pa_dbusobj_server_lookup *sl) {
    pa_assert(sl);

    if (sl->path_registered) {
        pa_assert(sl->conn);
        if (!dbus_connection_unregister_object_path(pa_dbus_connection_get(sl->conn), "/org/pulseaudio/server_lookup"))
            pa_log_debug("dbus_connection_unregister_object_path() failed for /org/pulseaudio/server_lookup.");
    }

    if (sl->conn)
        pa_dbus_connection_unref(sl->conn);

    pa_xfree(sl);
}
