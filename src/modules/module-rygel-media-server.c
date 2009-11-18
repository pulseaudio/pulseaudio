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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>
#include <pulse/i18n.h>
#include <pulse/utf8.h>

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
#include <pulsecore/protocol-http.h>
#include <pulsecore/parseaddr.h>

#include "module-rygel-media-server-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("UPnP MediaServer Plugin for Rygel");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "display_name=<UPnP Media Server name>");

/* This implements http://live.gnome.org/Rygel/MediaServerSpec */

#define SERVICE_NAME "org.gnome.UPnP.MediaServer1.PulseAudio"

#define OBJECT_ROOT "/org/gnome/UPnP/MediaServer1/PulseAudio"
#define OBJECT_SINKS "/org/gnome/UPnP/MediaServer1/PulseAudio/Sinks"
#define OBJECT_SOURCES "/org/gnome/UPnP/MediaServer1/PulseAudio/Sources"

#define CONTAINER_INTROSPECT_XML_PREFIX                                 \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
    "<node>"                                                            \
    " <!-- If you are looking for documentation make sure to check out" \
    "      http://live.gnome.org/Rygel/MediaServerSpec -->"             \
    " <interface name=\"org.gnome.UPnP.MediaContainer1\">"              \
    "  <signal name=\"Updated\">"                                       \
    "   <arg name=\"path\" type=\"o\"/>"                                \
    "  </signal>"                                                       \
    "  <property name=\"Items\" type=\"ao\" access=\"read\"/>"          \
    "  <property name=\"ItemCount\" type=\"u\" access=\"read\"/>"       \
    "  <property name=\"Containers\" type=\"ao\" access=\"read\"/>"     \
    "  <property name=\"ContainerCount\" type=\"u\" access=\"read\"/>"  \
    " </interface>"                                                     \
    " <interface name=\"org.gnome.UPnP.MediaObject1\">"                 \
    "  <property name=\"Parent\" type=\"s\" access=\"read\"/>"          \
    "  <property name=\"DisplayName\" type=\"s\" access=\"read\"/>"     \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Properties\">"             \
    "  <method name=\"Get\">"                                           \
    "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>"          \
    "   <arg name=\"property\" direction=\"in\" type=\"s\"/>"           \
    "   <arg name=\"value\" direction=\"out\" type=\"v\"/>"             \
    "  </method>"                                                       \
    "  <method name=\"GetAll\">"                                        \
    "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>"          \
    "   <arg name=\"properties\" direction=\"out\" type=\"a{sv}\"/>"    \
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
    " <interface name=\"org.gnome.UPnP.MediaItem1\">"                   \
    "  <property name=\"URLs\" type=\"as\" access=\"read\"/>"           \
    "  <property name=\"MIMEType\" type=\"s\" access=\"read\"/>"        \
    "  <property name=\"Type\" type=\"s\" access=\"read\"/>"            \
    " </interface>"                                                     \
    " <interface name=\"org.gnome.UPnP.MediaObject1\">"                 \
    "  <property name=\"Parent\" type=\"s\" access=\"read\"/>"          \
    "  <property name=\"DisplayName\" type=\"s\" access=\"read\"/>"     \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Properties\">"             \
    "  <method name=\"Get\">"                                           \
    "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>"          \
    "   <arg name=\"property\" direction=\"in\" type=\"s\"/>"           \
    "   <arg name=\"value\" direction=\"out\" type=\"v\"/>"             \
    "  </method>"                                                       \
    "  <method name=\"GetAll\">"                                        \
    "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>"          \
    "   <arg name=\"properties\" direction=\"out\" type=\"a{sv}\"/>"    \
    "  </method>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
    "  <method name=\"Introspect\">"                                    \
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
    "  </method>"                                                       \
    " </interface>"                                                     \
    "</node>"


static const char* const valid_modargs[] = {
    "display_name",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_dbus_connection *bus;
    pa_bool_t got_name:1;

    char *display_name;

    pa_hook_slot *source_new_slot, *source_unlink_slot;

    pa_http_protocol *http;
};

static void send_signal(struct userdata *u, pa_source *s) {
    DBusMessage *m;
    const char *parent;

    pa_assert(u);
    pa_source_assert_ref(s);

    if (u->core->state == PA_CORE_SHUTDOWN)
        return;

    if (s->monitor_of)
        parent = OBJECT_SINKS;
    else
        parent = OBJECT_SOURCES;

    pa_assert_se(m = dbus_message_new_signal(parent, "org.gnome.UPnP.MediaContainer1", "Updated"));
    pa_assert_se(dbus_connection_send(pa_dbus_connection_get(u->bus), m, NULL));

    dbus_message_unref(m);
}

static pa_hook_result_t source_new_or_unlink_cb(pa_core *c, pa_source *s, struct userdata *u) {
    pa_assert(c);
    pa_source_assert_ref(s);

    send_signal(u, s);

    return PA_HOOK_OK;
}

static pa_bool_t message_is_property_get(DBusMessage *m, const char *interface, const char *property) {
    const char *i, *p;
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

static pa_bool_t message_is_property_get_all(DBusMessage *m, const char *interface) {
    const char *i;
    DBusError error;

    dbus_error_init(&error);

    pa_assert(m);

    if (!dbus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "GetAll"))
        return FALSE;

    if (!dbus_message_get_args(m, &error, DBUS_TYPE_STRING, &i, DBUS_TYPE_INVALID) || dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return FALSE;
    }

    return pa_streq(i, interface);
}

static void append_variant_object_array(DBusMessage *m, DBusMessageIter *iter, const char *path[], unsigned n) {
    DBusMessageIter _iter, variant, array;
    unsigned c;

    pa_assert(m);
    pa_assert(path);

    if (!iter) {
        dbus_message_iter_init_append(m, &_iter);
        iter = &_iter;
    }

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "ao", &variant));
    pa_assert_se(dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "o", &array));

    for (c = 0; c < n; c++)
        pa_assert_se(dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, path + c));

    pa_assert_se(dbus_message_iter_close_container(&variant, &array));
    pa_assert_se(dbus_message_iter_close_container(iter, &variant));
}

static void append_variant_string(DBusMessage *m, DBusMessageIter *iter, const char *s) {
    DBusMessageIter _iter, sub;

    pa_assert(m);
    pa_assert(s);

    if (!iter) {
        dbus_message_iter_init_append(m, &_iter);
        iter = &_iter;
    }

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &s));
    pa_assert_se(dbus_message_iter_close_container(iter, &sub));
}

static void append_variant_object(DBusMessage *m, DBusMessageIter *iter, const char *s) {
    DBusMessageIter _iter, sub;

    pa_assert(m);
    pa_assert(s);

    if (!iter) {
        dbus_message_iter_init_append(m, &_iter);
        iter = &_iter;
    }

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "o", &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_OBJECT_PATH, &s));
    pa_assert_se(dbus_message_iter_close_container(iter, &sub));
}

static void append_variant_unsigned(DBusMessage *m, DBusMessageIter *iter, unsigned u) {
    DBusMessageIter _iter, sub;

    pa_assert(m);

    if (!iter) {
        dbus_message_iter_init_append(m, &_iter);
        iter = &_iter;
    }

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "u", &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_UINT32, &u));
    pa_assert_se(dbus_message_iter_close_container(iter, &sub));
}

static void append_property_dict_entry_object_array(DBusMessage *m, DBusMessageIter *iter, const char *name, const char *path[], unsigned n) {
    DBusMessageIter sub;

    pa_assert(iter);

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &name));
    append_variant_object_array(m, &sub, path, n);
    pa_assert_se(dbus_message_iter_close_container(iter, &sub));
}

static void append_property_dict_entry_string(DBusMessage *m, DBusMessageIter *iter, const char *name, const char *value) {
    DBusMessageIter sub;

    pa_assert(iter);

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &name));
    append_variant_string(m, &sub, value);
    pa_assert_se(dbus_message_iter_close_container(iter, &sub));
}

static void append_property_dict_entry_object(DBusMessage *m, DBusMessageIter *iter, const char *name, const char *value) {
    DBusMessageIter sub;

    pa_assert(iter);

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &name));
    append_variant_object(m, &sub, value);
    pa_assert_se(dbus_message_iter_close_container(iter, &sub));
}

static void append_property_dict_entry_unsigned(DBusMessage *m, DBusMessageIter *iter, const char *name, unsigned u) {
    DBusMessageIter sub;

    pa_assert(iter);

    pa_assert_se(dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &sub));
    pa_assert_se(dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &name));
    append_variant_unsigned(m, &sub, u);
    pa_assert_se(dbus_message_iter_close_container(iter, &sub));
}

static const char *array_root_containers[] = { OBJECT_SINKS, OBJECT_SOURCES };
static const char *array_no_children[] = { };

static DBusHandlerResult root_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    struct userdata *u = userdata;
    DBusMessage *r = NULL;

    pa_assert(u);

    if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "Containers")) {
        pa_assert_se(r = dbus_message_new_method_return(m));
        append_variant_object_array(r, NULL, (const char**) array_root_containers, PA_ELEMENTSOF(array_root_containers));

    } else if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "ContainerCount")) {
        pa_assert_se(r = dbus_message_new_method_return(m));
        append_variant_unsigned(r, NULL, PA_ELEMENTSOF(array_root_containers));

    } else if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "Items")) {
        pa_assert_se(r = dbus_message_new_method_return(m));
        append_variant_object_array(r, NULL, array_no_children, PA_ELEMENTSOF(array_no_children));

    } else if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "ItemCount")) {
        pa_assert_se(r = dbus_message_new_method_return(m));
        append_variant_unsigned(r, NULL, PA_ELEMENTSOF(array_no_children));

    } else if (message_is_property_get_all(m, "org.gnome.UPnP.MediaContainer1")) {
        DBusMessageIter iter, sub;

        pa_assert_se(r = dbus_message_new_method_return(m));
        dbus_message_iter_init_append(r, &iter);

        pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub));
        append_property_dict_entry_object_array(r, &sub, "Containers", array_root_containers, PA_ELEMENTSOF(array_root_containers));
        append_property_dict_entry_unsigned(r, &sub, "ContainerCount", PA_ELEMENTSOF(array_root_containers));
        append_property_dict_entry_object_array(r, &sub, "Items", array_no_children, PA_ELEMENTSOF(array_no_children));
        append_property_dict_entry_unsigned(r, &sub, "ItemCount", PA_ELEMENTSOF(array_no_children));
        pa_assert_se(dbus_message_iter_close_container(&iter, &sub));

    } else if (message_is_property_get(m, "org.gnome.UPnP.MediaObject1", "Parent")) {
        pa_assert_se(r = dbus_message_new_method_return(m));
        append_variant_object(r, NULL, OBJECT_ROOT);

    } else if (message_is_property_get(m, "org.gnome.UPnP.MediaObject1", "DisplayName")) {
        pa_assert_se(r = dbus_message_new_method_return(m));
        append_variant_string(r, NULL, u->display_name);

    } else if (message_is_property_get_all(m, "org.gnome.UPnP.MediaObject1")) {
        DBusMessageIter iter, sub;

        pa_assert_se(r = dbus_message_new_method_return(m));
        dbus_message_iter_init_append(r, &iter);

        pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub));
        append_property_dict_entry_string(r, &sub, "DisplayName", u->display_name);
        pa_assert_se(dbus_message_iter_close_container(&iter, &sub));

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

static char *compute_url(struct userdata *u, const char *name) {
    pa_strlist *i;

    pa_assert(u);
    pa_assert(name);

    for (i = pa_http_protocol_servers(u->http); i; i = pa_strlist_next(i)) {
        pa_parsed_address a;

        if (pa_parse_address(pa_strlist_data(i), &a) >= 0 &&
            (a.type == PA_PARSED_ADDRESS_TCP4 ||
             a.type == PA_PARSED_ADDRESS_TCP6 ||
             a.type == PA_PARSED_ADDRESS_TCP_AUTO)) {

            const char *address;
            char *s;

            if (pa_is_ip_address(a.path_or_host))
                address = a.path_or_host;
            else
                address = "@ADDRESS@";

            if (a.port <= 0)
                a.port = 4714;

            s = pa_sprintf_malloc("http://%s:%u/listen/source/%s", address, a.port, name);

            pa_xfree(a.path_or_host);
            return s;
        }

        pa_xfree(a.path_or_host);
    }

    return pa_sprintf_malloc("http://@ADDRESS@:4714/listen/source/%s", name);
}

static char **child_array(struct userdata *u, const char *path, unsigned *n) {
    unsigned m;
    uint32_t idx;
    char **array;

    pa_assert(u);
    pa_assert(path);
    pa_assert(n);

    if (pa_streq(path, OBJECT_SINKS))
        m = pa_idxset_size(u->core->sinks);
    else {
        unsigned k;

        m = pa_idxset_size(u->core->sources);
        k = pa_idxset_size(u->core->sinks);

        pa_assert(m >= k);

        /* Subtract the monitor sources from the numbers of
         * sources. There is one monitor source for each sink */
        m -= k;
    }

    array = pa_xnew(char*, m);
    *n = 0;

    if (pa_streq(path, OBJECT_SINKS)) {
        pa_sink *sink;

        PA_IDXSET_FOREACH(sink, u->core->sinks, idx) {
            pa_assert((*n) < m);
            array[(*n)++] = pa_sprintf_malloc(OBJECT_SINKS "/%u", sink->index);
        }
    } else {
        pa_source *source;

        PA_IDXSET_FOREACH(source, u->core->sources, idx) {

            if (!source->monitor_of) {
                pa_assert((*n) < m);
                array[(*n)++] = pa_sprintf_malloc(OBJECT_SOURCES "/%u", source->index);
            }
        }
    }

    pa_assert((*n) <= m);

    return array;
}

static void free_child_array(char **array, unsigned n) {

    for (; n >= 1; n--)
        pa_xfree(array[n-1]);

    pa_xfree(array);
}

static DBusHandlerResult sinks_and_sources_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    struct userdata *u = userdata;
    DBusMessage *r = NULL;
    const char *path;

    pa_assert(u);

    path = dbus_message_get_path(m);

    if (pa_streq(path, OBJECT_SINKS) || pa_streq(path, OBJECT_SOURCES)) {

        /* Container nodes */

        if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "Containers")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_object_array(r, NULL, array_no_children, PA_ELEMENTSOF(array_no_children));

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "ContainerCount")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_unsigned(r, NULL, PA_ELEMENTSOF(array_no_children));

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "Items")) {
            char ** array;
            unsigned n;

            array = child_array(u, path, &n);

            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_object_array(r, NULL, (const char**) array, n);

            free_child_array(array, n);

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaContainer1", "ItemCount")) {
            unsigned n, k;

            n = pa_idxset_size(u->core->sinks);
            k = pa_idxset_size(u->core->sources);
            pa_assert(k >= n);

            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_unsigned(r, NULL,
                                    pa_streq(path, OBJECT_SINKS) ? n : k - n);

        } else if (message_is_property_get_all(m, "org.gnome.UPnP.MediaContainer1")) {
            DBusMessageIter iter, sub;
            char **array;
            unsigned n, k;

            pa_assert_se(r = dbus_message_new_method_return(m));
            dbus_message_iter_init_append(r, &iter);

            pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub));
            append_property_dict_entry_object_array(r, &sub, "Containers", array_no_children, PA_ELEMENTSOF(array_no_children));
            append_property_dict_entry_unsigned(r, &sub, "ContainerCount", 0);

            array = child_array(u, path, &n);
            append_property_dict_entry_object_array(r, &sub, "Items", (const char**) array, n);
            free_child_array(array, n);

            n = pa_idxset_size(u->core->sinks);
            k = pa_idxset_size(u->core->sources);
            pa_assert(k >= n);

            append_property_dict_entry_unsigned(r, &sub, "ItemCount",
                                                pa_streq(path, OBJECT_SINKS) ? n : k - n);

            pa_assert_se(dbus_message_iter_close_container(&iter, &sub));

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaObject1", "Parent")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_object(r, NULL, OBJECT_ROOT);

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaObject1", "DisplayName")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_string(r,
                                  NULL,
                                  pa_streq(path, OBJECT_SINKS) ?
                                  _("Output Devices") :
                                  _("Input Devices"));

        } else if (message_is_property_get_all(m, "org.gnome.UPnP.MediaObject1")) {
            DBusMessageIter iter, sub;

            pa_assert_se(r = dbus_message_new_method_return(m));

            dbus_message_iter_init_append(r, &iter);

            pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub));
            append_property_dict_entry_object(m, &sub, "Parent", OBJECT_ROOT);
            append_property_dict_entry_string(m, &sub, "DisplayName",
                                              pa_streq(path, OBJECT_SINKS) ?
                                              _("Output Devices") :
                                              _("Input Devices"));
            pa_assert_se(dbus_message_iter_close_container(&iter, &sub));

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
        } else
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

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

        if (message_is_property_get(m, "org.gnome.UPnP.MediaObject1", "Parent")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_object(r, NULL, sink ? OBJECT_SINKS : OBJECT_SOURCES);

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaObject1", "DisplayName")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_string(r, NULL, pa_strna(pa_proplist_gets(sink ? sink->proplist : source->proplist, PA_PROP_DEVICE_DESCRIPTION)));

        } else if (message_is_property_get_all(m, "org.gnome.UPnP.MediaObject1")) {
            DBusMessageIter iter, sub;

            pa_assert_se(r = dbus_message_new_method_return(m));
            dbus_message_iter_init_append(r, &iter);

            pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub));
            append_property_dict_entry_object(r, &sub, "Parent", sink ? OBJECT_SINKS : OBJECT_SOURCES);
            append_property_dict_entry_string(r, &sub, "DisplayName", pa_strna(pa_proplist_gets(sink ? sink->proplist : source->proplist, PA_PROP_DEVICE_DESCRIPTION)));
            pa_assert_se(dbus_message_iter_close_container(&iter, &sub));

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaItem1", "Type")) {
            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_string(r, NULL, "audio");

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaItem1", "MIMEType")) {
            char *t;

            if (sink)
                t = pa_sample_spec_to_mime_type_mimefy(&sink->sample_spec, &sink->channel_map);
            else
                t = pa_sample_spec_to_mime_type_mimefy(&source->sample_spec, &source->channel_map);

            pa_assert_se(r = dbus_message_new_method_return(m));
            append_variant_string(r, NULL, t);
            pa_xfree(t);

        } else if (message_is_property_get(m, "org.gnome.UPnP.MediaItem1", "URLs")) {
            DBusMessageIter iter, sub, array;
            char *url;

            pa_assert_se(r = dbus_message_new_method_return(m));

            dbus_message_iter_init_append(r, &iter);

            url = compute_url(u, sink ? sink->monitor_source->name : source->name);

            pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "as", &sub));
            pa_assert_se(dbus_message_iter_open_container(&sub, DBUS_TYPE_ARRAY, "s", &array));
            pa_assert_se(dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &url));
            pa_assert_se(dbus_message_iter_close_container(&sub, &array));
            pa_assert_se(dbus_message_iter_close_container(&iter, &sub));

            pa_xfree(url);

        } else if (message_is_property_get_all(m, "org.gnome.UPnP.MediaItem1")) {
            DBusMessageIter iter, sub, dict, variant, array;
            char *url, *t;
            const char *un = "URLs";

            pa_assert_se(r = dbus_message_new_method_return(m));
            dbus_message_iter_init_append(r, &iter);

            pa_assert_se(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub));
            append_property_dict_entry_string(r, &sub, "Type", "audio");

            if (sink)
                t = pa_sample_spec_to_mime_type_mimefy(&sink->sample_spec, &sink->channel_map);
            else
                t = pa_sample_spec_to_mime_type_mimefy(&source->sample_spec, &source->channel_map);

            append_property_dict_entry_string(r, &sub, "MIMEType", t);
            pa_xfree(t);

            pa_assert_se(dbus_message_iter_open_container(&sub, DBUS_TYPE_DICT_ENTRY, NULL, &dict));
            pa_assert_se(dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &un));

            url = compute_url(u, sink ? sink->monitor_source->name : source->name);

            pa_assert_se(dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "as", &variant));
            pa_assert_se(dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array));
            pa_assert_se(dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &url));
            pa_assert_se(dbus_message_iter_close_container(&variant, &array));
            pa_assert_se(dbus_message_iter_close_container(&dict, &variant));
            pa_assert_se(dbus_message_iter_close_container(&sub, &dict));

            pa_xfree(url);

            pa_assert_se(dbus_message_iter_close_container(&iter, &sub));

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
    const char *t;

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
    u->http = pa_http_protocol_get(u->core);

    if ((t = pa_modargs_get_value(ma, "display_name", NULL)))
        u->display_name = pa_utf8_filter(t);
    else
        u->display_name = pa_xstrdup(_("Audio on @HOSTNAME@"));

    u->source_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_LATE, (pa_hook_cb_t) source_new_or_unlink_cb, u);
    u->source_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) source_new_or_unlink_cb, u);

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

    pa_xfree(u->display_name);

    if (u->http)
        pa_http_protocol_unref(u->http);

    pa_xfree(u);
}
