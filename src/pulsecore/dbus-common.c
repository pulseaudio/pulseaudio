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
    pa_strbuf_puts(buf, "<node>");

    while ((iface_entry = pa_hashmap_iterate(oe->interfaces, &state, NULL)))
        pa_strbuf_puts(buf, iface_entry->introspection_snippet);

    pa_strbuf_puts(buf, " <interface name=\"org.freedesktop.DBus.Introspectable\">"
                        "  <method name=\"Introspect\">"
                        "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"
                        "  </method>"
                        " </interface>");

    pa_strbuf_puts(buf, "</node>");

    pa_xfree(oe->introspection);
    oe->introspection = pa_strbuf_tostring_free(buf);
}

static struct interface_entry *find_interface(struct object_entry *obj_entry, DBusMessage *msg) {
    const char *interface;
    struct interface_entry *iface_entry;
    void *state = NULL;

    pa_assert(obj_entry);
    pa_assert(msg);

    if ((interface = dbus_message_get_interface(msg)))
        return pa_hashmap_get(obj_entry->interfaces, interface);

    /* NULL interface, we'll have to search for an interface that contains the
     * method. */

    while ((iface_entry = pa_hashmap_iterate(obj_entry->interfaces, &state, NULL))) {
        char *method;
        char **pos = iface_entry->methods;

        while ((method = *pos++)) {
            if (!strcmp(dbus_message_get_member(msg), method))
                return iface_entry;
        }
    }

    return NULL;
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

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
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

    if (!(iface_entry = find_interface(obj_entry, message)))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    return iface_entry->receive(connection, message, iface_entry->userdata);

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

static char **copy_methods(const char * const *methods) {
    unsigned n = 0;
    char **copy;
    unsigned i;

    while (methods[n++])
        ;

    copy = pa_xnew0(char *, n);

    for (i = 0; i < n - 1; ++i)
        copy[i] = pa_xstrdup(methods[i]);

    return copy;
}

int pa_dbus_add_interface(pa_core *c, const char* path, const char* interface, const char * const *methods, const char* introspection_snippet, DBusObjectPathMessageFunction receive_cb, void *userdata) {
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
    iface_entry->methods = copy_methods(methods);
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

static void free_methods(char **methods) {
    char **pos = methods;

    while (*pos++)
        pa_xfree(*pos);

    pa_xfree(methods);
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
    free_methods(iface_entry->methods);
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
