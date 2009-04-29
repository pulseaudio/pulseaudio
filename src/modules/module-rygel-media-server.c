/***
  This file is part of PulseAudio.

  Copyright 2005-2009 Lennart Poettering

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>
#include <pulse/i18n.h>

#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/modargs.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/endianmacros.h>
#include <pulsecore/namereg.h>
#include <pulsecore/mime-type.h>
#include <pulsecore/strbuf.h>

#include "module-rygel-media-server-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("UPnP MediaServer Plugin for Rygel");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

/* This implements http://live.gnome.org/action/edit/Rygel/MediaProviderSpec */

#define SERVICE_NAME "org.Rygel.MediaServer1.PulseAudio"

#define OBJECT_ROOT "/org/Rygel/MediaServer1/PulseAudio"
#define OBJECT_SINKS "/org/Rygel/MediaServer1/PulseAudio/Sinks"
#define OBJECT_SOURCES "/org/Rygel/MediaServer1/PulseAudio/Sources"

#define CONTAINER_INTROSPECT_XML_PREFIX                                 \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
    "<node>"                                                            \
    " <!-- If you are looking for documentation make sure to check out" \
    "      http://live.gnome.org/Rygel/MediaProviderSpec -->"           \
    " <interface name=\"org.Rygel.MediaContainer1\">"                   \
    "  <method name=\"GetContainers\">"                                 \
    "   <arg name=\"children\" type=\"ao\" direction=\"out\"/>"         \
    "  </method>"                                                       \
    "  <method name=\"GetItems\">"                                      \
    "   <arg name=\"children\" type=\"ao\" direction=\"out\"/>"         \
    "  </method>"                                                       \
    "  <signal name=\"ItemAdded\">"                                     \
    "   <arg name=\"path\" type=\"o\"/>"                                \
    "  </signal>"                                                       \
    "  <signal name=\"ItemRemoved\">"                                   \
    "   <arg name=\"path\" type=\"o\"/>"                                \
    "  </signal>"                                                       \
    "  <signal name=\"ContainerAdded\">"                                \
    "   <arg name=\"path\" type=\"o\"/>"                                \
    "  </signal>"                                                       \
    "  <signal name=\"ContainerRemoved\">"                              \
    "   <arg name=\"path\" type=\"o\"/>"                                \
    "  </signal>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"org.Rygel.MediaObject1\">"                      \
    "  <property name=\"display-name\" type=\"s\" access=\"read\"/>"    \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Properties\">"             \
    "  <method name=\"Get\">"                                           \
    "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>"          \
    "   <arg name=\"property\" direction=\"in\" type=\"s\"/>"           \
    "   <arg name=\"value\" direction=\"out\" type=\"v\"/>"             \
    "  </method>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
    "  <method name=\"Introspect\">"                                    \
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
    "  </method>"                                                       \
    " </interface>"

#define CONTAINER_INTROSPECT_XML_POSTFIX                                \
    "</node>"

#define ROOT_INTROSPECT_XML                                             \
    CONTAINER_INTROSPECT_XML_PREFIX                                     \
    "<node name=\"Sinks\"/>"                                            \
    "<node name=\"Sources\"/>"                                          \
    CONTAINER_INTROSPECT_XML_POSTFIX

#define ITEM_INTROSPECT_XML                                             \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
    "<node>"                                                            \
    " <!-- If you are looking for documentation make sure to check out" \
    "      http://live.gnome.org/Rygel/MediaProviderSpec -->"           \
    " <interface name=\"org.Rygel.MediaItem1\">"                        \
    "  <property name=\"urls\" type=\"as\" access=\"read\"/>"           \
    "  <property name=\"mime-type\" type=\"s\" access=\"read\"/>"       \
    "  <property name=\"type\" type=\"s\" access=\"read\"/>"            \
    " </interface>"                                                     \
    " <interface name=\"org.Rygel.MediaObject1\">"                      \
    "  <property name=\"display-name\" type=\"s\" access=\"read\"/>"    \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Properties\">"             \
    "  <method name=\"Get\">"                                           \
    "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>"          \
    "   <arg name=\"property\" direction=\"in\" type=\"s\"/>"           \
    "   <arg name=\"value\" direction=\"out\" type=\"v\"/>"             \
    "  </method>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
    "  <method name=\"Introspect\">"                                    \
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
    "  </method>"                                                       \
    " </interface>"                                                     \
    "</node>"


static const char* const valid_modargs[] = {
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_dbus_connection *bus;
    pa_bool_t got_name:1;

    pa_hook_slot *source_new_slot, *source_unlink_slot;
};

static void send_signal(struct userdata *u, pa_source *s, const char *name) {
    DBusMessage *m;
    char *child;
    const char *parent;

    pa_assert(u);
    pa_source_assert_ref(s);

    if (u->core->state == PA_CORE_SHUTDOWN)
        return;

    if (s->monitor_of) {
        parent = OBJECT_SINKS;
        child = pa_sprintf_malloc(OBJECT_SINKS "/%s", s->monitor_of->name);
    } else {
        parent = OBJECT_SOURCES;
        child = pa_sprintf_malloc(OBJECT_SOURCES "/%s", s->name);
    }

    pa_assert_se(m = dbus_message_new_signal(parent, "org.Rygel.MediaContainer1", name));
    pa_assert_se(dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &child, DBUS_TYPE_INVALID));
    pa_assert_se(dbus_connection_send(pa_dbus_connection_get(u->bus), m, NULL));

    pa_xfree(child);

    dbus_message_unref(m);
}

static pa_hook_result_t source_new_cb(pa_core *c, pa_source *s, struct userdata *u) {
    pa_assert(c);
    pa_source_assert_ref(s);

    send_signal(u, s, "ItemAdded");

    return PA_HOOK_OK;
}


static pa_hook_result_t source_unlink_cb(pa_core *c, pa_source *s, struct userdata *u) {
    pa_assert(c);
    pa_source_assert_ref(s);

    send_signal(u, s, "ItemRemoved");

    return PA_HOOK_OK;
}

static pa_bool_t message_is_property_get(DBusMessage *m, const char *interface, const char *property) {
    char *i, *p;
    DBusError error;

    dbus_error_init(&error);

    pa_assert(m);

    if (!dbus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "Get"))
        return FALSE;

    if (!dbus_message_get_args(m, &error, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID) || dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return FALSE;
    }

    return pa_streq(i, interface) && pa_streq(p, property);
}

static void append_variant_string(DBusMessage *m, const char *s) {
    DBusMessageIter iter, sub;

    pa_assert(m);
    pa_assert(s);

    dbus_message_iter_init_append(m, &iter);
    pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &s));
    pa_assert_se(dbus_message_iter_close_container(&iter, &sub));
}

static DBusHandlerResult root_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    struct userdata *u = userdata;
    DBusMessage *r = NULL;

    pa_assert(u);

    if (dbus_message_is_method_call(m, "org.Rygel.MediaContainer1", "GetContainers")) {
        const char * array[] = { OBJECT_SINKS, OBJECT_SOURCES };
        const char ** parray = array;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(
                             r,
                             DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &parray, 2,
                             DBUS_TYPE_INVALID));

    } else if (dbus_message_is_method_call(m, "org.Rygel.MediaContainer1", "GetItems")) {
        const char * array[] = { };
        const char **  parray = array;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(
                             r,
                             DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &parray, 0,
                             DBUS_TYPE_INVALID));

    } else if (message_is_property_get(m, "org.Rygel.MediaObject1", "display-name")) {
        pa_assert_se(r = dbus_message_new_method_return(m));
        append_variant_string(r, "PulseAudio");
    } else if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char *xml = ROOT_INTROSPECT_XML;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(
                             r,
                             DBUS_TYPE_STRING, &xml,
                             DBUS_TYPE_INVALID));

    } else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (r) {
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(u->bus), r, NULL));
        dbus_message_unref(r);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult sinks_and_sources_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    struct userdata *u = userdata;
    DBusMessage *r = NULL;
    const char *path;

    pa_assert(u);

    path = dbus_message_get_path(m);

    if (pa_streq(path, OBJECT_SINKS) || pa_streq(path, OBJECT_SOURCES)) {

        /* Container nodes */

        if (dbus_message_is_method_call(m, "org.Rygel.MediaContainer1", "GetContainers")) {

            const char * array[] = { };
            const char ** parray = array;

            pa_assert_se(r = dbus_message_new_method_return(m));
            pa_assert_se(dbus_message_append_args(
                                 r,
                                 DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &parray, 0,
                                 DBUS_TYPE_INVALID));

        } else if (dbus_message_is_method_call(m, "org.Rygel.MediaContainer1", "GetItems")) {
            unsigned n, i = 0;
            char ** array;
            uint32_t idx;

            if (pa_streq(path, OBJECT_SINKS))
                n = pa_idxset_size(u->core->sinks);
            else
                n = pa_idxset_size(u->core->sources);

            array = pa_xnew(char*, n);

            if (pa_streq(path, OBJECT_SINKS)) {
                pa_sink *sink;

                PA_IDXSET_FOREACH(sink, u->core->sinks, idx)
                    array[i++] = pa_sprintf_malloc(OBJECT_SINKS "/%u", sink->index);
            } else {
                pa_source *source;

                PA_IDXSET_FOREACH(source, u->core->sources, idx)
                    if (!source->monitor_of)
                        array[i++] = pa_sprintf_malloc(OBJECT_SOURCES "/%u", source->index);
            }

            pa_assert(i <= n);

            pa_assert_se(r = dbus_message_new_method_return(m));
            pa_assert_se(dbus_message_append_args(
                                 r,
                                 DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &array, i,
                                 DBUS_TYPE_INVALID));

            for (; i >= 1; i--)
                pa_xfree(array[i-1]);

            pa_xfree(array);

        } else if (message_is_property_get(m, "org.Rygel.MediaObject1", "display-name")) {

            if (pa_streq(path, OBJECT_SINKS)) {
                pa_assert_se(r = dbus_message_new_method_return(m));
                append_variant_string(r, _("Output Devices"));
            } else {
                pa_assert_se(r = dbus_message_new_method_return(m));
                append_variant_string(r, _("Input Devices"));
            }

        } else if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
            pa_strbuf *sb;
            char *xml;
            uint32_t idx;

            sb = pa_strbuf_new();
            pa_strbuf_puts(sb, CONTAINER_INTROSPECT_XML_PREFIX);

            if (pa_streq(path, OBJECT_SINKS)) {
                pa_sink *sink;

                PA_IDXSET_FOREACH(sink, u->core->sinks, idx)
                    pa_strbuf_printf(sb, "<node name=\"%u\"/>", sink->index);
            } else {
                pa_source *source;

                PA_IDXSET_FOREACH(source, u->core->sources, idx)
                    if (!source->monitor_of)
                        pa_strbuf_printf(sb, "<node name=\"%u\"/>", source->index);
            }

            pa_strbuf_puts(sb, CONTAINER_INTROSPECT_XML_POSTFIX);
            xml = pa_strbuf_tostring_free(sb);

            pa_assert_se(r = dbus_message_new_method_return(m));
            pa_assert_se(dbus_message_append_args(
                                 r,
                                 DBUS_TYPE_STRING, &xml,
                                 DBUS_TYPE_INVALID));

            pa_xfree(xml);
        }

    } else {
        pa_sink *sink = NULL;
        pa_source *source = NULL;

        /* Child nodes */

        if (pa_startswith(path, OBJECT_SINKS "/"))
            sink = pa_namereg_get(u->core, path + sizeof(OBJECT_SINKS), PA_NAMEREG_SINK);
        else if (pa_startswith(path, OBJECT_SOURCES "/"))
            source = pa_namereg_get(u->core, path + sizeof(OBJECT_SOURCES), PA_NAMEREG_SOURCE);

        if (!sink && !source)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        if (message_is_property_get(m, "org.Rygel.MediaObject1", "display-name")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_string(r, pa_strna(pa_proplist_gets(sink ? sink->proplist : source->proplist, PA_PROP_DEVICE_DESCRIPTION)));

        } else if (message_is_property_get(m, "org.Rygel.MediaItem1", "type")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_string(r, "audio");

        } else if (message_is_property_get(m, "org.Rygel.MediaItem1", "mime-type")) {
            char *t;

            if (sink)
                t = pa_sample_spec_to_mime_type_mimefy(&sink->sample_spec, &sink->channel_map);
            else
                t = pa_sample_spec_to_mime_type_mimefy(&source->sample_spec, &source->channel_map);

            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_string(r, t);

        } else if (message_is_property_get(m, "org.Rygel.MediaItem1", "urls")) {
            char * array[1];
            char ** parray = array;

            pa_assert_se(r = dbus_message_new_method_return(m));

            array[0] = pa_sprintf_malloc("http://@ADDRESS@:4714/listen/source/%s",
                                         sink ? sink->monitor_source->name : source->name);

            pa_assert_se(dbus_message_append_args(
                                 r,
                                 DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &parray, 1,
                                 DBUS_TYPE_INVALID));

            pa_xfree(array[0]);

        } else if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
            const char *xml =
                ITEM_INTROSPECT_XML;

            pa_assert_se(r = dbus_message_new_method_return(m));
            pa_assert_se(dbus_message_append_args(
                                 r,
                                 DBUS_TYPE_STRING, &xml,
                                 DBUS_TYPE_INVALID));

        } else
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (r) {
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(u->bus), r, NULL));
        dbus_message_unref(r);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

int pa__init(pa_module *m) {

    struct userdata *u;
    pa_modargs *ma = NULL;
    DBusError error;
    static const DBusObjectPathVTable vtable_root = {
        .message_function = root_handler,
    };
    static const DBusObjectPathVTable vtable_sinks_and_sources = {
        .message_function = sinks_and_sources_handler,
    };

    dbus_error_init(&error);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;

    u->source_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_LATE, (pa_hook_cb_t) source_new_cb, u);
    u->source_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) source_unlink_cb, u);

    if (!(u->bus = pa_dbus_bus_get(m->core, DBUS_BUS_SESSION, &error))) {
        pa_log("Failed to get session bus connection: %s", error.message);
        goto fail;
    }

    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(u->bus), OBJECT_ROOT, &vtable_root, u));
    pa_assert_se(dbus_connection_register_fallback(pa_dbus_connection_get(u->bus), OBJECT_SINKS, &vtable_sinks_and_sources, u));
    pa_assert_se(dbus_connection_register_fallback(pa_dbus_connection_get(u->bus), OBJECT_SOURCES, &vtable_sinks_and_sources, u));

    if (dbus_bus_request_name(pa_dbus_connection_get(u->bus), SERVICE_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        pa_log("Failed to request service name " SERVICE_NAME ": %s", error.message);
        goto fail;
    }

    u->got_name = TRUE;

    pa_modargs_free(ma);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    dbus_error_free(&error);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata*u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->source_new_slot)
        pa_hook_slot_free(u->source_new_slot);
    if (u->source_unlink_slot)
        pa_hook_slot_free(u->source_unlink_slot);

    if (u->bus) {
        DBusError error;

        dbus_error_init(&error);

        dbus_connection_unregister_object_path(pa_dbus_connection_get(u->bus), OBJECT_ROOT);
        dbus_connection_unregister_object_path(pa_dbus_connection_get(u->bus), OBJECT_SINKS);
        dbus_connection_unregister_object_path(pa_dbus_connection_get(u->bus), OBJECT_SOURCES);

        if (u->got_name) {
            if (dbus_bus_release_name(pa_dbus_connection_get(u->bus), SERVICE_NAME, &error) != DBUS_RELEASE_NAME_REPLY_RELEASED) {
                pa_log("Failed to release service name " SERVICE_NAME ": %s", error.message);
                dbus_error_free(&error);
            }
        }

        pa_dbus_connection_unref(u->bus);
    }

    pa_xfree(u);
}
