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

#include <pulse/xmalloc.h>

#include <pulsecore/dbus-common.h>
#include <pulsecore/macro.h>

#include "core.h"

#define OBJECT_NAME "/org/pulseaudio/core"
#define INTERFACE_NAME "org.pulseaudio.Core"

struct pa_dbusobj_core {
    pa_core *core;
};

static const char *introspection_snippet =
    " <interface name=\""INTERFACE_NAME"\">"
    "  <method name=\"Test\">"
    "   <arg name=\"result\" type=\"s\" direction=\"out\"/>"
    "  </method>"
    " </interface>";

static const char *methods[] = {
    "Test",
    NULL
};

static DBusHandlerResult handle_test(DBusConnection *conn, DBusMessage *msg, pa_dbusobj_core *c) {
    DBusMessage *reply = NULL;
    const char *reply_message = "Hello!";

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    if (!(reply = dbus_message_new_method_return(msg)))
        goto oom;

    if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &reply_message, DBUS_TYPE_INVALID))
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

static DBusHandlerResult receive_cb(DBusConnection *connection, DBusMessage *message, void *user_data) {
    pa_dbusobj_core *c = user_data;

    pa_assert(connection);
    pa_assert(message);
    pa_assert(c);

    if (dbus_message_is_method_call(message, INTERFACE_NAME, "Test"))
        return handle_test(connection, message, c);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

pa_dbusobj_core *pa_dbusobj_core_new(pa_core *core) {
    pa_dbusobj_core *c;

    pa_assert(core);

    c = pa_xnew(pa_dbusobj_core, 1);
    c->core = core;

    pa_dbus_add_interface(core, OBJECT_NAME, INTERFACE_NAME, methods, introspection_snippet, receive_cb, c);

    return c;
}

void pa_dbusobj_core_free(pa_dbusobj_core *c) {
    pa_assert(c);

    pa_dbus_remove_interface(c->core, OBJECT_NAME, INTERFACE_NAME);

    pa_xfree(c);
}
