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
#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/shared.h>
#include <pulsecore/strbuf.h>

#include "protocol-dbus.h"

struct pa_dbus_protocol {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_hashmap *objects; /* Object path -> struct object_entry */
    pa_hashmap *connections; /* DBusConnection -> struct connection_entry */
    pa_idxset *extensions; /* Strings */

    pa_hook hooks[PA_DBUS_PROTOCOL_HOOK_MAX];
};

struct object_entry {
    char *path;
    pa_hashmap *interfaces; /* Interface name -> struct interface_entry */
    char *introspection;
};

struct connection_entry {
    DBusConnection *connection;
    pa_client *client;

    pa_bool_t listening_for_all_signals;

    /* Contains object paths. If this is empty, then signals from all objects
     * are accepted. Only used when listening_for_all_signals == TRUE. */
    pa_idxset *all_signals_objects;

    /* Signal name -> idxset. The idxsets contain object paths. If an idxset is
     * empty, then that signal is accepted from all objects. Only used when
     * listening_for_all_signals == FALSE. */
    pa_hashmap *listening_signals;
};

struct interface_entry {
    char *name;
    pa_hashmap *method_handlers;
    pa_hashmap *property_handlers;
    pa_dbus_receive_cb_t get_all_properties_cb;
    pa_dbus_signal_info *signals;
    unsigned n_signals;
    void *userdata;
};

char *pa_get_dbus_address_from_server_type(pa_server_type_t server_type) {
    char *address = NULL;
    char *runtime_path = NULL;
    char *escaped_path = NULL;

    switch (server_type) {
        case PA_SERVER_TYPE_USER:
            pa_assert_se((runtime_path = pa_runtime_path(PA_DBUS_SOCKET_NAME)));
            pa_assert_se((escaped_path = dbus_address_escape_value(runtime_path)));
            address = pa_sprintf_malloc("unix:path=%s", escaped_path);
            break;

        case PA_SERVER_TYPE_SYSTEM:
            pa_assert_se((escaped_path = dbus_address_escape_value(PA_DBUS_SYSTEM_SOCKET_PATH)));
            address = pa_sprintf_malloc("unix:path=%s", escaped_path);
            break;

        case PA_SERVER_TYPE_NONE:
            address = pa_xnew0(char, 1);
            break;

        default:
            pa_assert_not_reached();
    }

    pa_xfree(runtime_path);
    pa_xfree(escaped_path);

    return address;
}

static pa_dbus_protocol *dbus_protocol_new(pa_core *c) {
    pa_dbus_protocol *p;
    unsigned i;

    pa_assert(c);

    p = pa_xnew(pa_dbus_protocol, 1);
    PA_REFCNT_INIT(p);
    p->core = pa_core_ref(c);
    p->objects = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    p->connections = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    p->extensions = pa_idxset_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    for (i = 0; i < PA_DBUS_PROTOCOL_HOOK_MAX; ++i)
        pa_hook_init(&p->hooks[i], p);

    pa_assert_se(pa_shared_set(c, "dbus-protocol", p) >= 0);

    return p;
}

pa_dbus_protocol* pa_dbus_protocol_get(pa_core *c) {
    pa_dbus_protocol *p;

    if ((p = pa_shared_get(c, "dbus-protocol")))
        return pa_dbus_protocol_ref(p);

    return dbus_protocol_new(c);
}

pa_dbus_protocol* pa_dbus_protocol_ref(pa_dbus_protocol *p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) >= 1);

    PA_REFCNT_INC(p);

    return p;
}

void pa_dbus_protocol_unref(pa_dbus_protocol *p) {
    unsigned i;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) >= 1);

    if (PA_REFCNT_DEC(p) > 0)
        return;

    pa_assert(pa_hashmap_isempty(p->objects));
    pa_assert(pa_hashmap_isempty(p->connections));
    pa_assert(pa_idxset_isempty(p->extensions));

    pa_hashmap_free(p->objects, NULL, NULL);
    pa_hashmap_free(p->connections, NULL, NULL);
    pa_idxset_free(p->extensions, NULL, NULL);

    for (i = 0; i < PA_DBUS_PROTOCOL_HOOK_MAX; ++i)
        pa_hook_done(&p->hooks[i]);

    pa_assert_se(pa_shared_remove(p->core, "dbus-protocol") >= 0);

    pa_core_unref(p->core);

    pa_xfree(p);
}

static void update_introspection(struct object_entry *oe) {
    pa_strbuf *buf;
    void *interfaces_state = NULL;
    struct interface_entry *iface_entry = NULL;

    pa_assert(oe);

    buf = pa_strbuf_new();
    pa_strbuf_puts(buf, DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE);
    pa_strbuf_puts(buf, "<node>\n");

    while ((iface_entry = pa_hashmap_iterate(oe->interfaces, &interfaces_state, NULL))) {
        pa_dbus_method_handler *method_handler;
        pa_dbus_property_handler *property_handler;
        void *handlers_state = NULL;
        unsigned i;
        unsigned j;

        pa_strbuf_printf(buf, " <interface name=\"%s\">\n", iface_entry->name);

        while ((method_handler = pa_hashmap_iterate(iface_entry->method_handlers, &handlers_state, NULL))) {
            pa_strbuf_printf(buf, "  <method name=\"%s\">\n", method_handler->method_name);

            for (i = 0; i < method_handler->n_arguments; ++i)
                pa_strbuf_printf(buf, "   <arg name=\"%s\" type=\"%s\" direction=\"%s\"/>\n", method_handler->arguments[i].name,
                                                                                              method_handler->arguments[i].type,
                                                                                              method_handler->arguments[i].direction);

            pa_strbuf_puts(buf, "  </method>\n");
        }

        handlers_state = NULL;

        while ((property_handler = pa_hashmap_iterate(iface_entry->property_handlers, &handlers_state, NULL)))
            pa_strbuf_printf(buf, "  <property name=\"%s\" type=\"%s\" access=\"%s\"/>\n", property_handler->property_name,
                                                                                           property_handler->type,
                                                                                           property_handler->get_cb ? (property_handler->set_cb ? "readwrite" : "read") : "write");

        for (i = 0; i < iface_entry->n_signals; ++i) {
            pa_strbuf_printf(buf, "  <signal name=\"%s\">\n", iface_entry->signals[i].name);

            for (j = 0; j < iface_entry->signals[i].n_arguments; ++j)
                pa_strbuf_printf(buf, "   <arg name=\"%s\" type=\"%s\"/>\n", iface_entry->signals[i].arguments[j].name,
                                                                             iface_entry->signals[i].arguments[j].type);

            pa_strbuf_puts(buf, "  </signal>\n");
        }

        pa_strbuf_puts(buf, " </interface>\n");
    }

    pa_strbuf_puts(buf, " <interface name=\"" DBUS_INTERFACE_INTROSPECTABLE "\">\n"
                        "  <method name=\"Introspect\">\n"
                        "   <arg name=\"data\" type=\"s\" direction=\"out\"/>\n"
                        "  </method>\n"
                        " </interface>\n"
                        " <interface name=\"" DBUS_INTERFACE_PROPERTIES "\">\n"
                        "  <method name=\"Get\">\n"
                        "   <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
                        "   <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
                        "   <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
                        "  </method>\n"
                        "  <method name=\"Set\">\n"
                        "   <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
                        "   <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
                        "   <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
                        "  </method>\n"
                        "  <method name=\"GetAll\">\n"
                        "   <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
                        "   <arg name=\"props\" type=\"a{sv}\" direction=\"out\"/>\n"
                        "  </method>\n"
                        " </interface>\n");

    pa_strbuf_puts(buf, "</node>\n");

    pa_xfree(oe->introspection);
    oe->introspection = pa_strbuf_tostring_free(buf);
}

enum find_result_t {
    FOUND_METHOD,
    FOUND_GET_PROPERTY,
    FOUND_SET_PROPERTY,
    FOUND_GET_ALL,
    PROPERTY_ACCESS_DENIED,
    NO_SUCH_METHOD,
    NO_SUCH_PROPERTY,
    INVALID_MESSAGE_ARGUMENTS
};

static enum find_result_t find_handler_by_property(struct object_entry *obj_entry,
                                                   DBusMessage *msg,
                                                   const char *property,
                                                   struct interface_entry **iface_entry,
                                                   pa_dbus_property_handler **property_handler) {
    void *state = NULL;

    pa_assert(obj_entry);
    pa_assert(msg);
    pa_assert(property);
    pa_assert(iface_entry);
    pa_assert(property_handler);

    while ((*iface_entry = pa_hashmap_iterate(obj_entry->interfaces, &state, NULL))) {
        if ((*property_handler = pa_hashmap_get((*iface_entry)->property_handlers, property))) {
            if (dbus_message_has_member(msg, "Get"))
                return (*property_handler)->get_cb ? FOUND_GET_PROPERTY : PROPERTY_ACCESS_DENIED;
            else if (dbus_message_has_member(msg, "Set"))
                return (*property_handler)->set_cb ? FOUND_SET_PROPERTY : PROPERTY_ACCESS_DENIED;
            else
                pa_assert_not_reached();
        }
    }

    return NO_SUCH_PROPERTY;
}

static enum find_result_t find_handler_by_method(struct object_entry *obj_entry,
                                                 const char *method,
                                                 struct interface_entry **iface_entry,
                                                 pa_dbus_method_handler **method_handler) {
    void *state = NULL;

    pa_assert(obj_entry);
    pa_assert(method);
    pa_assert(iface_entry);
    pa_assert(method_handler);

    while ((*iface_entry = pa_hashmap_iterate(obj_entry->interfaces, &state, NULL))) {
        if ((*method_handler = pa_hashmap_get((*iface_entry)->method_handlers, method)))
            return FOUND_METHOD;
    }

    pa_log("find_handler_by_method() failed.");
    return NO_SUCH_METHOD;
}

static enum find_result_t find_handler_from_properties_call(struct object_entry *obj_entry,
                                                            DBusMessage *msg,
                                                            struct interface_entry **iface_entry,
                                                            pa_dbus_property_handler **property_handler,
                                                            const char **attempted_property) {
    const char *interface;

    pa_assert(obj_entry);
    pa_assert(msg);
    pa_assert(iface_entry);

    if (dbus_message_has_member(msg, "GetAll")) {
        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID))
            return INVALID_MESSAGE_ARGUMENTS;

        if (*interface) {
            if ((*iface_entry = pa_hashmap_get(obj_entry->interfaces, interface)))
                return FOUND_GET_ALL;
            else {
                pa_log("GetAll message has unknown interface: %s", interface);
                return NO_SUCH_METHOD; /* XXX: NO_SUCH_INTERFACE or something like that might be more accurate. */
            }
        } else {
            pa_assert_se((*iface_entry = pa_hashmap_first(obj_entry->interfaces)));
            return FOUND_GET_ALL;
        }
    } else {
        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, attempted_property, DBUS_TYPE_INVALID))
            return INVALID_MESSAGE_ARGUMENTS;

        if (*interface) {
            if ((*iface_entry = pa_hashmap_get(obj_entry->interfaces, interface)) &&
                (*property_handler = pa_hashmap_get((*iface_entry)->property_handlers, *attempted_property))) {
                if (dbus_message_has_member(msg, "Get"))
                    return (*property_handler)->get_cb ? FOUND_GET_PROPERTY : PROPERTY_ACCESS_DENIED;
                else if (dbus_message_has_member(msg, "Set"))
                    return (*property_handler)->set_cb ? FOUND_SET_PROPERTY : PROPERTY_ACCESS_DENIED;
                else
                    pa_assert_not_reached();
            } else
                return NO_SUCH_PROPERTY;
        } else
            return find_handler_by_property(obj_entry, msg, *attempted_property, iface_entry, property_handler);
    }
}

static enum find_result_t find_handler(struct object_entry *obj_entry,
                                       DBusMessage *msg,
                                       struct interface_entry **iface_entry,
                                       pa_dbus_method_handler **method_handler,
                                       pa_dbus_property_handler **property_handler,
                                       const char **attempted_property) {
    const char *interface;

    pa_assert(obj_entry);
    pa_assert(msg);
    pa_assert(iface_entry);
    pa_assert(method_handler);
    pa_assert(property_handler);
    pa_assert(attempted_property);

    *iface_entry = NULL;
    *method_handler = NULL;

    if (dbus_message_has_interface(msg, DBUS_INTERFACE_PROPERTIES))
        return find_handler_from_properties_call(obj_entry, msg, iface_entry, property_handler, attempted_property);

    else if ((interface = dbus_message_get_interface(msg))) {
        if ((*iface_entry = pa_hashmap_get(obj_entry->interfaces, interface)) &&
            (*method_handler = pa_hashmap_get((*iface_entry)->method_handlers, dbus_message_get_member(msg))))
            return FOUND_METHOD;
        else {
            pa_log("Message has unknown interface or there's no method handler.");
            return NO_SUCH_METHOD;
        }

    } else { /* The method call doesn't contain an interface. */
        if (dbus_message_has_member(msg, "Get") || dbus_message_has_member(msg, "Set") || dbus_message_has_member(msg, "GetAll")) {
            if (find_handler_by_method(obj_entry, dbus_message_get_member(msg), iface_entry, method_handler) == FOUND_METHOD)
                return FOUND_METHOD; /* The object has a method named Get, Set or GetAll in some other interface than .Properties. */
            else
                /* Assume this is a .Properties call. */
                return find_handler_from_properties_call(obj_entry, msg, iface_entry, property_handler, attempted_property);

        } else /* This is not a .Properties call. */
            return find_handler_by_method(obj_entry, dbus_message_get_member(msg), iface_entry, method_handler);
    }
}

static DBusHandlerResult handle_message_cb(DBusConnection *connection, DBusMessage *message, void *user_data) {
    pa_dbus_protocol *p = user_data;
    struct object_entry *obj_entry = NULL;
    struct interface_entry *iface_entry = NULL;
    pa_dbus_method_handler *method_handler = NULL;
    pa_dbus_property_handler *property_handler = NULL;
    const char *attempted_property = NULL;
    DBusMessage *reply = NULL;

    pa_assert(connection);
    pa_assert(message);
    pa_assert(p);
    pa_assert(p->objects);

    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    pa_log("Received method call: destination = %s, name = %s, iface = %s", dbus_message_get_path(message), dbus_message_get_member(message), dbus_message_get_interface(message));

    pa_assert_se((obj_entry = pa_hashmap_get(p->objects, dbus_message_get_path(message))));

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect") ||
        (!dbus_message_get_interface(message) && dbus_message_has_member(message, "Introspect"))) {
        pa_assert_se((reply = dbus_message_new_method_return(message)));
        pa_assert_se(dbus_message_append_args(reply, DBUS_TYPE_STRING, &obj_entry->introspection, DBUS_TYPE_INVALID));
        pa_assert_se(dbus_connection_send(connection, reply, NULL));

        pa_log_debug("%s.Introspect handled.", obj_entry->path);

        goto finish;
    }

    switch (find_handler(obj_entry, message, &iface_entry, &method_handler, &property_handler, &attempted_property)) {
        case FOUND_METHOD:
            method_handler->receive_cb(connection, message, iface_entry->userdata);
            break;

        case FOUND_GET_PROPERTY:
            property_handler->get_cb(connection, message, iface_entry->userdata);
            break;

        case FOUND_SET_PROPERTY:
            property_handler->set_cb(connection, message, iface_entry->userdata);
            break;

        case FOUND_GET_ALL:
            if (iface_entry->get_all_properties_cb)
                iface_entry->get_all_properties_cb(connection, message, iface_entry->userdata);
            break;

        case PROPERTY_ACCESS_DENIED:
            pa_assert_se((reply = dbus_message_new_error_printf(message, DBUS_ERROR_ACCESS_DENIED, "%s access denied for property %s", dbus_message_get_member(message), attempted_property)));
            pa_assert_se(dbus_connection_send(connection, reply, NULL));
            break;

        case NO_SUCH_METHOD:
            pa_assert_se((reply = dbus_message_new_error_printf(message, DBUS_ERROR_UNKNOWN_METHOD, "%s: No such method", dbus_message_get_member(message))));
            pa_assert_se(dbus_connection_send(connection, reply, NULL));
            break;

        case NO_SUCH_PROPERTY:
            pa_assert_se((reply = dbus_message_new_error_printf(message, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "%s: No such property", attempted_property)));
            pa_assert_se(dbus_connection_send(connection, reply, NULL));
            break;

        case INVALID_MESSAGE_ARGUMENTS:
            pa_assert_se((reply = dbus_message_new_error_printf(message, DBUS_ERROR_INVALID_ARGS, "Invalid arguments for %s", dbus_message_get_member(message))));
            pa_assert_se(dbus_connection_send(connection, reply, NULL));
            break;

        default:
            pa_assert_not_reached();
    }

finish:
    if (reply)
        dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusObjectPathVTable vtable = {
    .unregister_function = NULL,
    .message_function = handle_message_cb,
    .dbus_internal_pad1 = NULL,
    .dbus_internal_pad2 = NULL,
    .dbus_internal_pad3 = NULL,
    .dbus_internal_pad4 = NULL
};

static void register_object(pa_dbus_protocol *p, struct object_entry *obj_entry) {
    struct connection_entry *conn_entry;
    void *state = NULL;

    pa_assert(p);
    pa_assert(obj_entry);

    while ((conn_entry = pa_hashmap_iterate(p->connections, &state, NULL)))
        pa_assert_se(dbus_connection_register_object_path(conn_entry->connection, obj_entry->path, &vtable, p));
}

static pa_dbus_arg_info *copy_args(const pa_dbus_arg_info *src, unsigned n) {
    pa_dbus_arg_info *dst;
    unsigned i;

    if (n == 0)
        return NULL;

    pa_assert(src);

    dst = pa_xnew0(pa_dbus_arg_info, n);

    for (i = 0; i < n; ++i) {
        dst[i].name = pa_xstrdup(src[i].name);
        dst[i].type = pa_xstrdup(src[i].type);
        dst[i].direction = pa_xstrdup(src[i].direction);
    }

    return dst;
}

static pa_hashmap *create_method_handlers(const pa_dbus_interface_info *info) {
    pa_hashmap *handlers;
    unsigned i;

    pa_assert(info);
    pa_assert(info->method_handlers || info->n_method_handlers == 0);

    handlers = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    for (i = 0; i < info->n_method_handlers; ++i) {
        pa_dbus_method_handler *h = pa_xnew(pa_dbus_method_handler, 1);
        h->method_name = pa_xstrdup(info->method_handlers[i].method_name);
        h->arguments = copy_args(info->method_handlers[i].arguments, info->method_handlers[i].n_arguments);
        h->n_arguments = info->method_handlers[i].n_arguments;
        h->receive_cb = info->method_handlers[i].receive_cb;

        pa_hashmap_put(handlers, h->method_name, h);
    }

    return handlers;
}

static pa_hashmap *create_property_handlers(const pa_dbus_interface_info *info) {
    pa_hashmap *handlers;
    unsigned i = 0;

    pa_assert(info);
    pa_assert(info->property_handlers || info->n_property_handlers == 0);

    handlers = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    for (i = 0; i < info->n_property_handlers; ++i) {
        pa_dbus_property_handler *h = pa_xnew(pa_dbus_property_handler, 1);
        h->property_name = pa_xstrdup(info->property_handlers[i].property_name);
        h->type = pa_xstrdup(info->property_handlers[i].type);
        h->get_cb = info->property_handlers[i].get_cb;
        h->set_cb = info->property_handlers[i].set_cb;

        pa_hashmap_put(handlers, h->property_name, h);
    }

    return handlers;
}

static pa_dbus_signal_info *copy_signals(const pa_dbus_interface_info *info) {
    pa_dbus_signal_info *dst;
    unsigned i;

    pa_assert(info);

    if (info->n_signals == 0)
        return NULL;

    pa_assert(info->signals);

    dst = pa_xnew(pa_dbus_signal_info, info->n_signals);

    for (i = 0; i < info->n_signals; ++i) {
        dst[i].name = pa_xstrdup(info->signals[i].name);
        dst[i].arguments = copy_args(info->signals[i].arguments, info->signals[i].n_arguments);
        dst[i].n_arguments = info->signals[i].n_arguments;
    }

    return dst;
}

int pa_dbus_protocol_add_interface(pa_dbus_protocol *p,
                                   const char *path,
                                   const pa_dbus_interface_info *info,
                                   void *userdata) {
    struct object_entry *obj_entry;
    struct interface_entry *iface_entry;
    pa_bool_t obj_entry_created = FALSE;

    pa_assert(p);
    pa_assert(path);
    pa_assert(info);
    pa_assert(info->name);
    pa_assert(info->method_handlers || info->n_method_handlers == 0);
    pa_assert(info->property_handlers || info->n_property_handlers == 0);
    pa_assert(info->get_all_properties_cb || info->n_property_handlers == 0);
    pa_assert(info->signals || info->n_signals == 0);

    if (!(obj_entry = pa_hashmap_get(p->objects, path))) {
        obj_entry = pa_xnew(struct object_entry, 1);
        obj_entry->path = pa_xstrdup(path);
        obj_entry->interfaces = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        obj_entry->introspection = NULL;

        pa_hashmap_put(p->objects, path, obj_entry);
        obj_entry_created = TRUE;
    }

    if (pa_hashmap_get(obj_entry->interfaces, info->name) != NULL)
        goto fail; /* The interface was already registered. */

    iface_entry = pa_xnew(struct interface_entry, 1);
    iface_entry->name = pa_xstrdup(info->name);
    iface_entry->method_handlers = create_method_handlers(info);
    iface_entry->property_handlers = create_property_handlers(info);
    iface_entry->get_all_properties_cb = info->get_all_properties_cb;
    iface_entry->signals = copy_signals(info);
    iface_entry->n_signals = info->n_signals;
    iface_entry->userdata = userdata;
    pa_hashmap_put(obj_entry->interfaces, iface_entry->name, iface_entry);

    update_introspection(obj_entry);

    if (obj_entry_created)
        register_object(p, obj_entry);

    pa_log("Interface %s added for object %s. GetAll callback? %s", iface_entry->name, obj_entry->path, iface_entry->get_all_properties_cb ? "yes" : "no");

    return 0;

fail:
    if (obj_entry_created) {
        pa_hashmap_remove(p->objects, path);
        pa_dbus_protocol_unref(p);
        pa_xfree(obj_entry);
    }

    return -1;
}

static void unregister_object(pa_dbus_protocol *p, struct object_entry *obj_entry) {
    struct connection_entry *conn_entry;
    void *state = NULL;

    pa_assert(p);
    pa_assert(obj_entry);

    while ((conn_entry = pa_hashmap_iterate(p->connections, &state, NULL)))
        pa_assert_se(dbus_connection_unregister_object_path(conn_entry->connection, obj_entry->path));
}

static void method_handler_free_cb(void *p, void *userdata) {
    pa_dbus_method_handler *h = p;
    unsigned i;

    pa_assert(h);

    pa_xfree((char *) h->method_name);

    for (i = 0; i < h->n_arguments; ++i) {
        pa_xfree((char *) h->arguments[i].name);
        pa_xfree((char *) h->arguments[i].type);
        pa_xfree((char *) h->arguments[i].direction);
    }

    pa_xfree((pa_dbus_arg_info *) h->arguments);
}

static void property_handler_free_cb(void *p, void *userdata) {
    pa_dbus_property_handler *h = p;

    pa_assert(h);

    pa_xfree((char *) h->property_name);
    pa_xfree((char *) h->type);

    pa_xfree(h);
}

int pa_dbus_protocol_remove_interface(pa_dbus_protocol *p, const char* path, const char* interface) {
    struct object_entry *obj_entry;
    struct interface_entry *iface_entry;
    unsigned i;

    pa_assert(p);
    pa_assert(path);
    pa_assert(interface);

    if (!(obj_entry = pa_hashmap_get(p->objects, path)))
        return -1;

    if (!(iface_entry = pa_hashmap_remove(obj_entry->interfaces, interface)))
        return -1;

    update_introspection(obj_entry);

    pa_xfree(iface_entry->name);
    pa_hashmap_free(iface_entry->method_handlers, method_handler_free_cb, NULL);
    pa_hashmap_free(iface_entry->property_handlers, property_handler_free_cb, NULL);

    for (i = 0; i < iface_entry->n_signals; ++i) {
        unsigned j;

        pa_xfree((char *) iface_entry->signals[i].name);

        for (j = 0; j < iface_entry->signals[i].n_arguments; ++j) {
            pa_xfree((char *) iface_entry->signals[i].arguments[j].name);
            pa_xfree((char *) iface_entry->signals[i].arguments[j].type);
            pa_assert(iface_entry->signals[i].arguments[j].direction == NULL);
        }

        pa_xfree((pa_dbus_arg_info *) iface_entry->signals[i].arguments);
    }

    pa_xfree(iface_entry->signals);
    pa_xfree(iface_entry);

    if (pa_hashmap_isempty(obj_entry->interfaces)) {
        unregister_object(p, obj_entry);

        pa_hashmap_remove(p->objects, path);
        pa_xfree(obj_entry->path);
        pa_hashmap_free(obj_entry->interfaces, NULL, NULL);
        pa_xfree(obj_entry->introspection);
        pa_xfree(obj_entry);
    }

    return 0;
}

static void register_all_objects(pa_dbus_protocol *p, DBusConnection *conn) {
    struct object_entry *obj_entry;
    void *state = NULL;

    pa_assert(p);
    pa_assert(conn);

    while ((obj_entry = pa_hashmap_iterate(p->objects, &state, NULL)))
        pa_assert_se(dbus_connection_register_object_path(conn, obj_entry->path, &vtable, p));
}

int pa_dbus_protocol_register_connection(pa_dbus_protocol *p, DBusConnection *conn, pa_client *client) {
    struct connection_entry *conn_entry;

    pa_assert(p);
    pa_assert(conn);
    pa_assert(client);

    if (pa_hashmap_get(p->connections, conn))
        return -1; /* The connection was already registered. */

    register_all_objects(p, conn);

    conn_entry = pa_xnew(struct connection_entry, 1);
    conn_entry->connection = dbus_connection_ref(conn);
    conn_entry->client = client;
    conn_entry->listening_for_all_signals = FALSE;
    conn_entry->all_signals_objects = pa_idxset_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    conn_entry->listening_signals = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    pa_hashmap_put(p->connections, conn, conn_entry);

    return 0;
}

static void unregister_all_objects(pa_dbus_protocol *p, DBusConnection *conn) {
    struct object_entry *obj_entry;
    void *state = NULL;

    pa_assert(p);
    pa_assert(conn);

    while ((obj_entry = pa_hashmap_iterate(p->objects, &state, NULL)))
        pa_assert_se(dbus_connection_unregister_object_path(conn, obj_entry->path));
}

static void free_listened_object_name_cb(void *p, void *userdata) {
    pa_assert(p);

    pa_xfree(p);
}

static void free_listening_signals_idxset_cb(void *p, void *userdata) {
    pa_idxset *set = p;

    pa_assert(set);

    pa_idxset_free(set, free_listened_object_name_cb, NULL);
}

int pa_dbus_protocol_unregister_connection(pa_dbus_protocol *p, DBusConnection *conn) {
    struct connection_entry *conn_entry;

    pa_assert(p);
    pa_assert(conn);

    if (!(conn_entry = pa_hashmap_remove(p->connections, conn)))
        return -1;

    unregister_all_objects(p, conn);

    dbus_connection_unref(conn_entry->connection);
    pa_idxset_free(conn_entry->all_signals_objects, free_listened_object_name_cb, NULL);
    pa_hashmap_free(conn_entry->listening_signals, free_listening_signals_idxset_cb, NULL);
    pa_xfree(conn_entry);

    return 0;
}

pa_client *pa_dbus_protocol_get_client(pa_dbus_protocol *p, DBusConnection *conn) {
    struct connection_entry *conn_entry;

    pa_assert(p);
    pa_assert(conn);

    if (!(conn_entry = pa_hashmap_get(p->connections, conn)))
        return NULL;

    return conn_entry->client;
}

void pa_dbus_protocol_add_signal_listener(pa_dbus_protocol *p, DBusConnection *conn, const char *signal, char **objects, unsigned n_objects) {
    struct connection_entry *conn_entry;
    pa_idxset *object_set;
    char *object_path;
    unsigned i;

    pa_assert(p);
    pa_assert(conn);
    pa_assert(objects || n_objects == 0);

    pa_assert_se((conn_entry = pa_hashmap_get(p->connections, conn)));

    /* all_signals_objects will either be emptied or replaced with new objects,
     * so we empty it here unconditionally. If listening_for_all_signals is
     * currently FALSE, the idxset is empty already. */
    while ((object_path = pa_idxset_steal_first(conn_entry->all_signals_objects, NULL)))
        pa_xfree(object_path);

    if (signal) {
        conn_entry->listening_for_all_signals = FALSE;

        /* Replace the old object list with a new one. */
        if ((object_set = pa_hashmap_get(conn_entry->listening_signals, signal)))
            pa_idxset_free(object_set, free_listened_object_name_cb, NULL);
        object_set = pa_idxset_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

        for (i = 0; i < n_objects; ++i)
            pa_idxset_put(object_set, pa_xstrdup(objects[i]), NULL);

    } else {
        conn_entry->listening_for_all_signals = TRUE;

        /* We're not interested in individual signals anymore, so let's empty
         * listening_signals. */
        while ((object_set = pa_hashmap_steal_first(conn_entry->listening_signals)))
            pa_idxset_free(object_set, free_listened_object_name_cb, NULL);

        for (i = 0; i < n_objects; ++i)
            pa_idxset_put(conn_entry->all_signals_objects, pa_xstrdup(objects[i]), NULL);
    }
}

void pa_dbus_protocol_remove_signal_listener(pa_dbus_protocol *p, DBusConnection *conn, const char *signal) {
    struct connection_entry *conn_entry;
    pa_idxset *object_set;

    pa_assert(p);
    pa_assert(conn);

    pa_assert_se((conn_entry = pa_hashmap_get(p->connections, conn)));

    if (signal) {
        if ((object_set = pa_hashmap_get(conn_entry->listening_signals, signal)))
            pa_idxset_free(object_set, free_listened_object_name_cb, NULL);

    } else {
        char *object_path;

        conn_entry->listening_for_all_signals = FALSE;

        while ((object_path = pa_idxset_steal_first(conn_entry->all_signals_objects, NULL)))
            pa_xfree(object_path);

        while ((object_set = pa_hashmap_steal_first(conn_entry->listening_signals)))
            pa_idxset_free(object_set, free_listened_object_name_cb, NULL);
    }
}

void pa_dbus_protocol_send_signal(pa_dbus_protocol *p, DBusMessage *signal) {
    struct connection_entry *conn_entry;
    void *state = NULL;
    pa_idxset *object_set;
    DBusMessage *signal_copy;

    pa_assert(p);
    pa_assert(signal);
    pa_assert(dbus_message_get_type(signal) == DBUS_MESSAGE_TYPE_SIGNAL);

    /* XXX: We have to do some linear searching to find connections that want
     * to receive the signal. This shouldn't be very significant performance
     * problem, and adding an (object path, signal name) -> connection mapping
     * would be likely to create substantial complexity. */

    while ((conn_entry = pa_hashmap_iterate(p->connections, &state, NULL))) {

        if ((conn_entry->listening_for_all_signals /* Case 1: listening for all signals */
             && (pa_idxset_get_by_data(conn_entry->all_signals_objects, dbus_message_get_path(signal), NULL)
                 || pa_idxset_isempty(conn_entry->all_signals_objects)))

            || (!conn_entry->listening_for_all_signals /* Case 2: not listening for all signals */
                && (object_set = pa_hashmap_get(conn_entry->listening_signals, signal))
                && (pa_idxset_get_by_data(object_set, dbus_message_get_path(signal), NULL)
                    || pa_idxset_isempty(object_set)))) {

            pa_assert_se(signal_copy = dbus_message_copy(signal));
            pa_assert_se(dbus_connection_send(conn_entry->connection, signal_copy, NULL));
            dbus_message_unref(signal_copy);
        }
    }
}

const char **pa_dbus_protocol_get_extensions(pa_dbus_protocol *p, unsigned *n) {
    const char **extensions;
    const char *ext_name;
    void *state = NULL;
    unsigned i = 0;

    pa_assert(p);
    pa_assert(n);

    *n = pa_idxset_size(p->extensions);

    if (*n <= 0)
        return NULL;

    extensions = pa_xnew(const char *, *n);

    while ((ext_name = pa_idxset_iterate(p->extensions, &state, NULL))) {
        extensions[i] = ext_name;
        ++i;
    }

    return extensions;
}

int pa_dbus_protocol_register_extension(pa_dbus_protocol *p, const char *name) {
    char *internal_name;

    pa_assert(p);
    pa_assert(name);

    internal_name = pa_xstrdup(name);

    if (pa_idxset_put(p->extensions, internal_name, NULL) < 0) {
        pa_xfree(internal_name);
        return -1;
    }

    pa_hook_fire(&p->hooks[PA_DBUS_PROTOCOL_HOOK_EXTENSION_REGISTERED], internal_name);

    return 0;
}

int pa_dbus_protocol_unregister_extension(pa_dbus_protocol *p, const char *name) {
    char *internal_name;

    pa_assert(p);
    pa_assert(name);

    if (!(internal_name = pa_idxset_remove_by_data(p->extensions, name, NULL)))
        return -1;

    pa_hook_fire(&p->hooks[PA_DBUS_PROTOCOL_HOOK_EXTENSION_UNREGISTERED], internal_name);

    pa_xfree(internal_name);

    return 0;
}

pa_hook_slot *pa_dbus_protocol_hook_connect(pa_dbus_protocol *p, pa_dbus_protocol_hook_t hook, pa_hook_priority_t prio, pa_hook_cb_t cb, void *data) {
    pa_assert(p);
    pa_assert(hook < PA_DBUS_PROTOCOL_HOOK_MAX);
    pa_assert(cb);

    return pa_hook_connect(&p->hooks[hook], prio, cb, data);
}
