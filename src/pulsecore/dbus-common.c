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
#include <pulsecore/strbuf.h>

#include "dbus-common.h"

struct dbus_state {
    pa_core *core;
    pa_hashmap *objects; /* Object path -> struct object_entry */
    pa_idxset *connections; /* DBusConnections */
};

struct object_entry {
    char *path;
    pa_hashmap *interfaces; /* Interface name -> struct interface_entry */
    char *introspection;
};

struct interface_entry {
    char *name;
    char **properties;
    char **methods;
    char *introspection_snippet;
    DBusObjectPathMessageFunction receive;
    void *userdata;
};

char *pa_get_dbus_address_from_server_type(pa_server_type_t server_type) {
    char *address = NULL;
    char *runtime_path = NULL;
    char *escaped_path = NULL;

    switch (server_type) {
        case PA_SERVER_TYPE_USER:
            if (!(runtime_path = pa_runtime_path(PA_DBUS_SOCKET_NAME))) {
                pa_log("pa_runtime_path() failed.");
                break;
            }

            if (!(escaped_path = dbus_address_escape_value(runtime_path))) {
                pa_log("dbus_address_escape_value() failed.");
                break;
            }

            address = pa_sprintf_malloc("unix:path=%s", escaped_path);
            break;

        case PA_SERVER_TYPE_SYSTEM:
            if (!(escaped_path = dbus_address_escape_value(PA_DBUS_SYSTEM_SOCKET_PATH))) {
                pa_log("dbus_address_escape_value() failed.");
                break;
            }

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

static void update_introspection(struct object_entry *oe) {
    pa_strbuf *buf;
    void *state = NULL;
    struct interface_entry *iface_entry = NULL;

    pa_assert(oe);

    buf = pa_strbuf_new();
    pa_strbuf_puts(buf, DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE);
    pa_strbuf_puts(buf, "<node>\n");

    while ((iface_entry = pa_hashmap_iterate(oe->interfaces, &state, NULL)))
        pa_strbuf_puts(buf, iface_entry->introspection_snippet);

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
    SUCCESS,
    NO_SUCH_PROPERTY,
    NO_SUCH_METHOD,
    INVALID_MESSAGE_ARGUMENTS
};

static enum find_result_t find_interface_by_property(struct object_entry *obj_entry, const char *property, struct interface_entry **entry) {
    void *state = NULL;

    pa_assert(obj_entry);
    pa_assert(property);
    pa_assert(entry);

    while ((*entry = pa_hashmap_iterate(obj_entry->interfaces, &state, NULL))) {
        char *iface_property;
        char **pos = (*entry)->properties;

        while ((iface_property = *pos++)) {
            if (pa_streq(iface_property, property))
                return SUCCESS;
        }
    }

    return NO_SUCH_PROPERTY;
}

static enum find_result_t find_interface_by_method(struct object_entry *obj_entry, const char *method, struct interface_entry **entry) {
    void *state = NULL;

    pa_assert(obj_entry);
    pa_assert(method);
    pa_assert(entry);

    while ((*entry = pa_hashmap_iterate(obj_entry->interfaces, &state, NULL))) {
        char *iface_method;
        char **pos = (*entry)->methods;

        while ((iface_method = *pos++)) {
            if (pa_streq(iface_method, method))
                return SUCCESS;
        }
    }

    return NO_SUCH_METHOD;
}

static enum find_result_t find_interface_from_properties_call(struct object_entry *obj_entry, DBusMessage *msg, struct interface_entry **entry) {
    const char *interface;
    const char *property;

    pa_assert(obj_entry);
    pa_assert(msg);
    pa_assert(entry);

    if (dbus_message_has_member(msg, "GetAll")) {
        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID))
            return INVALID_MESSAGE_ARGUMENTS;

        if (*interface) {
            if ((*entry = pa_hashmap_get(obj_entry->interfaces, interface)))
                return SUCCESS;
            else
                return NO_SUCH_METHOD;
        } else {
            pa_assert_se((*entry = pa_hashmap_first(obj_entry->interfaces)));
            return SUCCESS;
        }
    } else {
        pa_assert(dbus_message_has_member(msg, "Get") || dbus_message_has_member(msg, "Set"));

        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID))
            return INVALID_MESSAGE_ARGUMENTS;

        if (*interface) {
            if ((*entry = pa_hashmap_get(obj_entry->interfaces, interface)))
                return SUCCESS;
            else
                return NO_SUCH_METHOD;
        } else
            return find_interface_by_property(obj_entry, property, entry);
    }
}

static enum find_result_t find_interface(struct object_entry *obj_entry, DBusMessage *msg, struct interface_entry **entry) {
    const char *interface;

    pa_assert(obj_entry);
    pa_assert(msg);
    pa_assert(entry);

    *entry = NULL;

    if (dbus_message_has_interface(msg, DBUS_INTERFACE_PROPERTIES))
        return find_interface_from_properties_call(obj_entry, msg, entry);

    else if ((interface = dbus_message_get_interface(msg))) {
        if ((*entry = pa_hashmap_get(obj_entry->interfaces, interface)))
            return SUCCESS;
        else
            return NO_SUCH_METHOD;

    } else { /* The method call doesn't contain an interface. */
        if (dbus_message_has_member(msg, "Get") || dbus_message_has_member(msg, "Set") || dbus_message_has_member(msg, "GetAll")) {
            if (find_interface_by_method(obj_entry, dbus_message_get_member(msg), entry) == SUCCESS)
                return SUCCESS; /* The object has a method named Get, Set or GetAll in some other interface than .Properties. */
            else
                /* Assume this is a .Properties call. */
                return find_interface_from_properties_call(obj_entry, msg, entry);

        } else /* This is not a .Properties call. */
            return find_interface_by_method(obj_entry, dbus_message_get_member(msg), entry);
    }
}

static DBusHandlerResult handle_message_cb(DBusConnection *connection, DBusMessage *message, void *user_data) {
    struct dbus_state *dbus_state = user_data;
    struct object_entry *obj_entry;
    struct interface_entry *iface_entry;
    DBusMessage *reply = NULL;

    pa_assert(connection);
    pa_assert(message);
    pa_assert(dbus_state);
    pa_assert(dbus_state->objects);

    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    pa_assert_se((obj_entry = pa_hashmap_get(dbus_state->objects, dbus_message_get_path(message))));

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect") ||
        (!dbus_message_get_interface(message) && dbus_message_has_member(message, "Introspect"))) {
        if (!(reply = dbus_message_new_method_return(message)))
            goto oom;

        if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &obj_entry->introspection, DBUS_TYPE_INVALID))
            goto fail;

        if (!dbus_connection_send(connection, reply, NULL))
            goto oom;

        pa_log_debug("%s.%s handled.", obj_entry->path, "Introspect");

        dbus_message_unref(reply);

        return DBUS_HANDLER_RESULT_HANDLED;
    }

    switch (find_interface(obj_entry, message, &iface_entry)) {
        case SUCCESS:
            return iface_entry->receive(connection, message, iface_entry->userdata);

        case NO_SUCH_PROPERTY:
            if (!(reply = dbus_message_new_error(message, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "No such property")))
                goto fail;

            if (!dbus_connection_send(connection, reply, NULL))
                goto oom;

            dbus_message_unref(reply);

            return DBUS_HANDLER_RESULT_HANDLED;

        case NO_SUCH_METHOD:
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        case INVALID_MESSAGE_ARGUMENTS:
            if (!(reply = dbus_message_new_error(message, DBUS_ERROR_INVALID_ARGS, "Invalid arguments")))
                goto fail;

            if (!dbus_connection_send(connection, reply, NULL))
                goto oom;

            dbus_message_unref(reply);

            return DBUS_HANDLER_RESULT_HANDLED;

        default:
            pa_assert_not_reached();
    }

fail:
    if (reply)
        dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

oom:
    if (reply)
        dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusObjectPathVTable vtable = {
    .unregister_function = NULL,
    .message_function = handle_message_cb,
    .dbus_internal_pad1 = NULL,
    .dbus_internal_pad2 = NULL,
    .dbus_internal_pad3 = NULL,
    .dbus_internal_pad4 = NULL
};

static void register_object(struct dbus_state *dbus_state, struct object_entry *obj_entry) {
    DBusConnection *conn;
    void *state = NULL;

    pa_assert(dbus_state);
    pa_assert(obj_entry);

    if (!dbus_state->connections)
        return;

    while ((conn = pa_idxset_iterate(dbus_state->connections, &state, NULL))) {
        if (!dbus_connection_register_object_path(conn, obj_entry->path, &vtable, dbus_state))
            pa_log_debug("dbus_connection_register_object_path() failed.");
    }
}

static char **copy_strarray(const char * const *array) {
    unsigned n = 0;
    char **copy;
    unsigned i;

    while (array[n++])
        ;

    copy = pa_xnew0(char *, n);

    for (i = 0; i < n - 1; ++i)
        copy[i] = pa_xstrdup(array[i]);

    return copy;
}

int pa_dbus_add_interface(pa_core *c,
                          const char* path,
                          const char* interface,
                          const char * const *properties,
                          const char * const *methods,
                          const char* introspection_snippet,
                          DBusObjectPathMessageFunction receive_cb,
                          void *userdata) {
    struct dbus_state *dbus_state;
    pa_hashmap *objects;
    struct object_entry *obj_entry;
    struct interface_entry *iface_entry;
    pa_bool_t state_created = FALSE;
    pa_bool_t object_map_created = FALSE;
    pa_bool_t obj_entry_created = FALSE;

    pa_assert(c);
    pa_assert(path);
    pa_assert(introspection_snippet);
    pa_assert(receive_cb);

    if (!(dbus_state = pa_hashmap_get(c->shared, "dbus-state"))) {
        dbus_state = pa_xnew0(struct dbus_state, 1);
        dbus_state->core = c;
        pa_hashmap_put(c->shared, "dbus-state", dbus_state);
        state_created = TRUE;
    }

    if (!(objects = dbus_state->objects)) {
        objects = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        dbus_state->objects = objects;
        object_map_created = TRUE;
    }

    if (!(obj_entry = pa_hashmap_get(objects, path))) {
        obj_entry = pa_xnew(struct object_entry, 1);
        obj_entry->path = pa_xstrdup(path);
        obj_entry->interfaces = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        obj_entry->introspection = NULL;
        pa_hashmap_put(objects, path, obj_entry);
        obj_entry_created = TRUE;
    }

    if (pa_hashmap_get(obj_entry->interfaces, interface) != NULL)
        goto fail; /* The interface was already registered. */

    iface_entry = pa_xnew(struct interface_entry, 1);
    iface_entry->name = pa_xstrdup(interface);
    iface_entry->properties = copy_strarray(properties);
    iface_entry->methods = copy_strarray(methods);
    iface_entry->introspection_snippet = pa_xstrdup(introspection_snippet);
    iface_entry->receive = receive_cb;
    iface_entry->userdata = userdata;
    pa_hashmap_put(obj_entry->interfaces, iface_entry->name, iface_entry);

    update_introspection(obj_entry);

    if (obj_entry_created)
        register_object(dbus_state, obj_entry);

    return 0;

fail:
    if (obj_entry_created) {
        pa_hashmap_remove(objects, path);
        pa_xfree(obj_entry);
    }

    if (object_map_created) {
        dbus_state->objects = NULL;
        pa_hashmap_free(objects, NULL, NULL);
    }

    if (state_created) {
        pa_hashmap_remove(c->shared, "dbus-state");
        pa_xfree(dbus_state);
    }

    return -1;
}

static void unregister_object(struct dbus_state *dbus_state, struct object_entry *obj_entry) {
    DBusConnection *conn;
    void *state = NULL;

    pa_assert(dbus_state);
    pa_assert(obj_entry);

    if (!dbus_state->connections)
        return;

    while ((conn = pa_idxset_iterate(dbus_state->connections, &state, NULL))) {
        if (!dbus_connection_unregister_object_path(conn, obj_entry->path))
            pa_log_debug("dbus_connection_unregister_object_path() failed.");
    }
}

static void free_strarray(char **array) {
    char **pos = array;

    while (*pos++)
        pa_xfree(*pos);

    pa_xfree(array);
}

int pa_dbus_remove_interface(pa_core *c, const char* path, const char* interface) {
    struct dbus_state *dbus_state;
    pa_hashmap *objects;
    struct object_entry *obj_entry;
    struct interface_entry *iface_entry;

    pa_assert(c);
    pa_assert(path);
    pa_assert(interface);

    if (!(dbus_state = pa_hashmap_get(c->shared, "dbus-state")))
        return -1;

    if (!(objects = dbus_state->objects))
        return -1;

    if (!(obj_entry = pa_hashmap_get(objects, path)))
        return -1;

    if (!(iface_entry = pa_hashmap_remove(obj_entry->interfaces, interface)))
        return -1;

    update_introspection(obj_entry);

    pa_xfree(iface_entry->name);
    free_strarray(iface_entry->properties);
    free_strarray(iface_entry->methods);
    pa_xfree(iface_entry->introspection_snippet);
    pa_xfree(iface_entry);

    if (pa_hashmap_isempty(obj_entry->interfaces)) {
        unregister_object(dbus_state, obj_entry);

        pa_hashmap_remove(objects, path);
        pa_xfree(obj_entry->path);
        pa_hashmap_free(obj_entry->interfaces, NULL, NULL);
        pa_xfree(obj_entry->introspection);
        pa_xfree(obj_entry);
    }

    if (pa_hashmap_isempty(objects)) {
        dbus_state->objects = NULL;
        pa_hashmap_free(objects, NULL, NULL);
    }

    if (!dbus_state->objects && !dbus_state->connections) {
        pa_hashmap_remove(c->shared, "dbus-state");
        pa_xfree(dbus_state);
    }

    return 0;
}

static void register_all_objects(struct dbus_state *dbus_state, DBusConnection *conn) {
    struct object_entry *obj_entry;
    void *state = NULL;

    pa_assert(dbus_state);
    pa_assert(conn);

    if (!dbus_state->objects)
        return;

    while ((obj_entry = pa_hashmap_iterate(dbus_state->objects, &state, NULL))) {
        if (!dbus_connection_register_object_path(conn, obj_entry->path, &vtable, dbus_state))
            pa_log_debug("dbus_connection_register_object_path() failed.");
    }
}

int pa_dbus_register_connection(pa_core *c, DBusConnection *conn) {
    struct dbus_state *dbus_state;
    pa_idxset *connections;
    pa_bool_t state_created = FALSE;
    pa_bool_t connection_set_created = FALSE;

    pa_assert(c);
    pa_assert(conn);

    if (!(dbus_state = pa_hashmap_get(c->shared, "dbus-state"))) {
        dbus_state = pa_xnew0(struct dbus_state, 1);
        dbus_state->core = c;
        pa_hashmap_put(c->shared, "dbus-state", dbus_state);
        state_created = TRUE;
    }

    if (!(connections = dbus_state->connections)) {
        connections = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
        dbus_state->connections = connections;
        connection_set_created = TRUE;
    }

    if (pa_idxset_get_by_data(connections, conn, NULL))
        goto fail; /* The connection was already registered. */

    register_all_objects(dbus_state, conn);

    pa_idxset_put(connections, dbus_connection_ref(conn), NULL);

    return 0;

fail:
    if (connection_set_created) {
        dbus_state->connections = NULL;
        pa_idxset_free(connections, NULL, NULL);
    }

    if (state_created) {
        pa_hashmap_remove(c->shared, "dbus-state");
        pa_xfree(dbus_state);
    }

    return -1;
}

static void unregister_all_objects(struct dbus_state *dbus_state, DBusConnection *conn) {
    struct object_entry *obj_entry;
    void *state = NULL;

    pa_assert(dbus_state);
    pa_assert(conn);

    if (!dbus_state->objects)
        return;

    while ((obj_entry = pa_hashmap_iterate(dbus_state->objects, &state, NULL))) {
        if (!dbus_connection_unregister_object_path(conn, obj_entry->path))
            pa_log_debug("dus_connection_unregister_object_path() failed.");
    }
}

int pa_dbus_unregister_connection(pa_core *c, DBusConnection *conn) {
    struct dbus_state *dbus_state;
    pa_idxset *connections;

    pa_assert(c);
    pa_assert(conn);

    if (!(dbus_state = pa_hashmap_get(c->shared, "dbus-state")))
        return -1;

    if (!(connections = dbus_state->connections))
        return -1;

    if (!pa_idxset_remove_by_data(connections, conn, NULL))
        return -1;

    unregister_all_objects(dbus_state, conn);

    dbus_connection_unref(conn);

    if (pa_idxset_isempty(connections)) {
        dbus_state->connections = NULL;
        pa_idxset_free(connections, NULL, NULL);
    }

    if (!dbus_state->objects && !dbus_state->connections) {
        pa_hashmap_remove(c->shared, "dbus-state");
        pa_xfree(dbus_state);
    }

    return 0;
}
