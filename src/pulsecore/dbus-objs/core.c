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

#include <pulsecore/core-util.h>
#include <pulsecore/dbus-common.h>
#include <pulsecore/macro.h>

#include "core.h"

#define OBJECT_PATH "/org/pulseaudio1"
#define INTERFACE_CORE "org.PulseAudio.Core1"

struct pa_dbusobj_core {
    pa_core *core;
};

static const char *introspection_snippet =
    " <interface name=\"" INTERFACE_CORE "\">\n"
    "  <method name=\"GetCardByName\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Card\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"GetSinkByName\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Sink\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"GetSourceByName\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Source\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"GetSampleByName\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Sample\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"UploadSample\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"SampleFormat\" type=\"y\" direction=\"in\"/>\n"
    "   <arg name=\"SampleRate\" type=\"u\" direction=\"in\"/>\n"
    "   <arg name=\"Channels\" type=\"ay\" direction=\"in\"/>\n"
    "   <arg name=\"DefaultVolume\" type=\"au\" direction=\"in\"/>\n"
    "   <arg name=\"Proplist\" type=\"a{say}\" direction=\"in\"/>\n"
    "   <arg name=\"Data\" type=\"ay\" direction=\"in\"/>\n"
    "   <arg name=\"Sample\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"LoadSampleFromFile\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Filepath\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Sample\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"AddLazySample\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Filepath\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Sample\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"AddLazySamplesFromDirectory\">\n"
    "   <arg name=\"Dirpath\" type=\"s\" direction=\"in\"/>\n"
    "  </method>\n"
    "  <method name=\"LoadModule\">\n"
    "   <arg name=\"Name\" type=\"s\" direction=\"in\"/>\n"
    "   <arg name=\"Arguments\" type=\"a{ss}\" direction=\"in\"/>\n"
    "   <arg name=\"Module\" type=\"o\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"Exit\"/>\n"
    "  <signal name=\"NewCard\">\n"
    "   <arg name=\"Card\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"CardRemoved\">\n"
    "   <arg name=\"Card\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"NewSink\">\n"
    "   <arg name=\"Sink\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"SinkRemoved\">\n"
    "   <arg name=\"Sink\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"FallbackSinkUpdated\">\n"
    "   <arg name=\"Sink\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"NewSource\">\n"
    "   <arg name=\"Source\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"SourceRemoved\">\n"
    "   <arg name=\"Source\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"FallbackSourceUpdated\">\n"
    "   <arg name=\"Source\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"NewPlaybackStream\">\n"
    "   <arg name=\"PlaybackStream\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"PlaybackStreamRemoved\">\n"
    "   <arg name=\"PlaybackStream\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"NewRecordStream\">\n"
    "   <arg name=\"RecordStream\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"RecordStreamRemoved\">\n"
    "   <arg name=\"RecordStream\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"NewSample\">\n"
    "   <arg name=\"Sample\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"SampleRemoved\">\n"
    "   <arg name=\"Sample\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"NewModule\">\n"
    "   <arg name=\"Module\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"ModuleRemoved\">\n"
    "   <arg name=\"Module\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"NewClient\">\n"
    "   <arg name=\"Client\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <signal name=\"ClientRemoved\">\n"
    "   <arg name=\"Client\" type=\"o\"/>\n"
    "  </signal>\n"
    "  <property name=\"InterfaceRevision\" type=\"u\" access=\"read\"/>\n"
    "  <property name=\"Name\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Version\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Username\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Hostname\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"DefaultChannels\" type=\"ay\" access=\"readwrite\"/>\n"
    "  <property name=\"DefaultSampleFormat\" type=\"y\" access=\"readwrite\"/>\n"
    "  <property name=\"DefaultSampleRate\" type=\"u\" access=\"readwrite\"/>\n"
    "  <property name=\"Sinks\" type=\"ao\" access=\"read\"/>\n"
    "  <property name=\"FallbackSink\" type=\"s\" access=\"readwrite\"/>\n"
    "  <property name=\"Sources\" type=\"ao\" access=\"read\"/>\n"
    "  <property name=\"FallbackSource\" type=\"o\" access=\"readwrite\"/>\n"
    "  <property name=\"PlaybackStreams\" type=\"ao\" access=\"read\"/>\n"
    "  <property name=\"RecordStreams\" type=\"ao\" access=\"read\"/>\n"
    "  <property name=\"Samples\" type=\"ao\" access=\"read\"/>\n"
    "  <property name=\"Modules\" type=\"ao\" access=\"read\"/>\n"
    "  <property name=\"Clients\" type=\"ao\" access=\"read\"/>\n"
    "  <property name=\"Extensions\" type=\"as\" access=\"read\"/>\n"
    " </interface>\n";

/* If you need to modify this list, note that handle_get_all() uses hard-coded
 * indexes to point to these strings, so make sure the indexes don't go wrong
 * there. */
static const char *properties[] = {
    "InterfaceRevision",
    "Name",
    "Version",
    "Username",
    "Hostname",
    "DefaultChannels",
    "DefaultSampleFormat",
    "DefaultSampleRate",
    "Sinks",
    "FallbackSink",
    "Sources",
    "FallbackSource",
    "PlaybackStreams",
    "RecordStreams",
    "Samples",
    "Modules",
    "Clients",
    "Extensions",
    NULL
};

static const char *methods[] = {
    "GetCardByName",
    "GetSinkByName",
    "GetSourceByName",
    "GetSampleByName",
    "UploadSample",
    "LoadSampleFromFile",
    "AddLazySample",
    "AddLazySamplesFromDirectory",
    "LoadModule",
    "Exit",
    NULL
};

static DBusHandlerResult handle_get_name(DBusConnection *conn, DBusMessage *msg, pa_dbusobj_core *c) {
    DBusHandlerResult r = DBUS_HANDLER_RESULT_HANDLED;
    DBusMessage *reply = NULL;
    const char *server_name = PACKAGE_NAME;
    DBusMessageIter msg_iter;
    DBusMessageIter variant_iter;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    if (!(reply = dbus_message_new_method_return(msg))) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    dbus_message_iter_init_append(reply, &msg_iter);
    if (!dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_VARIANT, "s", &variant_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &server_name)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_close_container(&msg_iter, &variant_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_connection_send(conn, reply, NULL)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    r = DBUS_HANDLER_RESULT_HANDLED;

finish:
    if (reply)
        dbus_message_unref(reply);

    return r;
}

static DBusHandlerResult handle_get(DBusConnection *conn, DBusMessage *msg, pa_dbusobj_core *c) {
    DBusHandlerResult r = DBUS_HANDLER_RESULT_HANDLED;
    const char* interface;
    const char* property;
    DBusMessage *reply = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID)) {
        if (!(reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments"))) {
            r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            goto finish;
        }
        if (!dbus_connection_send(conn, reply, NULL)) {
            r = DBUS_HANDLER_RESULT_NEED_MEMORY;
            goto finish;
        }
        r = DBUS_HANDLER_RESULT_HANDLED;
        goto finish;
    }

    if (*interface && !pa_streq(interface, INTERFACE_CORE)) {
        r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        goto finish;
    }

    if (pa_streq(property, "Name")) {
        r = handle_get_name(conn, msg, c);
        goto finish;
    }

    if (!(reply = dbus_message_new_error_printf(msg, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "%s: No such property", property))) {
        r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        goto finish;
    }
    if (!dbus_connection_send(conn, reply, NULL)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    r = DBUS_HANDLER_RESULT_HANDLED;

finish:
    if (reply)
        dbus_message_unref(reply);

    return r;
}

static DBusHandlerResult handle_set(DBusConnection *conn, DBusMessage *msg, pa_dbusobj_core *c) {
    DBusHandlerResult r = DBUS_HANDLER_RESULT_HANDLED;
    const char* interface;
    const char* property;
    DBusMessage *reply = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID)) {
        if (!(reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments"))) {
            r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            goto finish;
        }
        if (!dbus_connection_send(conn, reply, NULL)) {
            r = DBUS_HANDLER_RESULT_NEED_MEMORY;
            goto finish;
        }
        r = DBUS_HANDLER_RESULT_HANDLED;
        goto finish;
    }

    if (*interface && !pa_streq(interface, INTERFACE_CORE)) {
        r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        goto finish;
    }

    if (pa_streq(property, "Name")) {
        if (!(reply = dbus_message_new_error_printf(msg, DBUS_ERROR_ACCESS_DENIED, "%s: Property not settable", property))) {
            r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            goto finish;
        }
        if (!dbus_connection_send(conn, reply, NULL)) {
            r = DBUS_HANDLER_RESULT_NEED_MEMORY;
            goto finish;
        }
        r = DBUS_HANDLER_RESULT_HANDLED;
        goto finish;
    }

    if (!(reply = dbus_message_new_error_printf(msg, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "%s: No such property", property))) {
        r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        goto finish;
    }
    if (!dbus_connection_send(conn, reply, NULL)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    r = DBUS_HANDLER_RESULT_HANDLED;
    goto finish;

    if (!(reply = dbus_message_new_error_printf(msg, DBUS_ERROR_ACCESS_DENIED, "%s: Property not settable", property))) {
        r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        goto finish;
    }
    if (!dbus_connection_send(conn, reply, NULL)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    r = DBUS_HANDLER_RESULT_HANDLED;

finish:
    if (reply)
        dbus_message_unref(reply);

    return r;
}

static DBusHandlerResult handle_get_all(DBusConnection *conn, DBusMessage *msg, pa_dbusobj_core *c) {
    DBusHandlerResult r = DBUS_HANDLER_RESULT_HANDLED;
    DBusMessage *reply = NULL;
    char *interface = NULL;
    char const *server_name = PACKAGE_NAME;
    DBusMessageIter msg_iter;
    DBusMessageIter dict_iter;
    DBusMessageIter dict_entry_iter;
    DBusMessageIter variant_iter;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID)) {
        if (!(reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments"))) {
            r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            goto finish;
        }
        if (!dbus_connection_send(conn, reply, NULL)) {
            r = DBUS_HANDLER_RESULT_NEED_MEMORY;
            goto finish;
        }
        r = DBUS_HANDLER_RESULT_HANDLED;
        goto finish;
    }

    if (!(reply = dbus_message_new_method_return(msg))) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    dbus_message_iter_init_append(reply, &msg_iter);
    if (!dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_append_basic(&dict_entry_iter, DBUS_TYPE_STRING, &properties[1])) { /* Name */
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_open_container(&dict_entry_iter, DBUS_TYPE_VARIANT, "s", &variant_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &server_name)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_close_container(&dict_entry_iter, &variant_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_close_container(&dict_iter, &dict_entry_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_message_iter_close_container(&msg_iter, &dict_iter)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    if (!dbus_connection_send(conn, reply, NULL)) {
        r = DBUS_HANDLER_RESULT_NEED_MEMORY;
        goto finish;
    }
    r = DBUS_HANDLER_RESULT_HANDLED;

finish:
    if (reply)
        dbus_message_unref(reply);

    return r;
}

static DBusHandlerResult receive_cb(DBusConnection *connection, DBusMessage *message, void *user_data) {
    pa_dbusobj_core *c = user_data;

    pa_assert(connection);
    pa_assert(message);
    pa_assert(c);

    if (dbus_message_is_method_call(message, DBUS_INTERFACE_PROPERTIES, "Get") ||
        (!dbus_message_get_interface(message) && dbus_message_has_member(message, "Get")))
        return handle_get(connection, message, c);

    if (dbus_message_is_method_call(message, DBUS_INTERFACE_PROPERTIES, "Set") ||
        (!dbus_message_get_interface(message) && dbus_message_has_member(message, "Set")))
        return handle_set(connection, message, c);

    if (dbus_message_is_method_call(message, DBUS_INTERFACE_PROPERTIES, "GetAll") ||
        (!dbus_message_get_interface(message) && dbus_message_has_member(message, "GetAll")))
        return handle_get_all(connection, message, c);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

pa_dbusobj_core *pa_dbusobj_core_new(pa_core *core) {
    pa_dbusobj_core *c;

    pa_assert(core);

    c = pa_xnew(pa_dbusobj_core, 1);
    c->core = core;

    pa_dbus_add_interface(core, OBJECT_PATH, INTERFACE_CORE, properties, methods, introspection_snippet, receive_cb, c);


    return c;
}

void pa_dbusobj_core_free(pa_dbusobj_core *c) {
    pa_assert(c);

    pa_dbus_remove_interface(c->core, OBJECT_PATH, INTERFACE_CORE);

    pa_xfree(c);
}
