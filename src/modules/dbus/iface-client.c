/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen
  Copyright 2009 Vincent Filali-Ansary <filali.v@azurdigitalnetworks.net>

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

#include <pulsecore/core-util.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>

#include "iface-client.h"

#define OBJECT_NAME "client"

struct pa_dbusiface_client {
    pa_dbusiface_core *core;

    pa_client *client;
    char *path;

    pa_dbus_protocol *dbus_protocol;
};

static void handle_get_index(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_driver(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_owner_module(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_playback_streams(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_record_streams(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_property_list(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void handle_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata);

/*static void handle_kill(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_update_properties(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_remove_properties(DBusConnection *conn, DBusMessage *msg, void *userdata);*/

enum property_handler_index {
    PROPERTY_HANDLER_INDEX,
    PROPERTY_HANDLER_DRIVER,
    PROPERTY_HANDLER_OWNER_MODULE,
    PROPERTY_HANDLER_PLAYBACK_STREAMS,
    PROPERTY_HANDLER_RECORD_STREAMS,
    PROPERTY_HANDLER_PROPERTY_LIST,
    PROPERTY_HANDLER_MAX
};

static pa_dbus_property_handler property_handlers[PROPERTY_HANDLER_MAX] = {
    [PROPERTY_HANDLER_INDEX]            = { .property_name = "Index",           .type = "u",      .get_cb = handle_get_index,            .set_cb = NULL },
    [PROPERTY_HANDLER_DRIVER]           = { .property_name = "Driver",          .type = "s",      .get_cb = handle_get_driver,           .set_cb = NULL },
    [PROPERTY_HANDLER_OWNER_MODULE]     = { .property_name = "OwnerModule",     .type = "o",      .get_cb = handle_get_owner_module,     .set_cb = NULL },
    [PROPERTY_HANDLER_PLAYBACK_STREAMS] = { .property_name = "PlaybackStreams", .type = "ao",     .get_cb = handle_get_playback_streams, .set_cb = NULL },
    [PROPERTY_HANDLER_RECORD_STREAMS]   = { .property_name = "RecordStreams",   .type = "ao",     .get_cb = handle_get_record_streams,   .set_cb = NULL },
    [PROPERTY_HANDLER_PROPERTY_LIST]    = { .property_name = "PropertyList",    .type = "a{say}", .get_cb = handle_get_property_list,    .set_cb = NULL }
};

/*enum method_handler_index {
    METHOD_HANDLER_KILL,
    METHOD_HANDLER_UPDATE_PROPERTIES,
    METHOD_HANDLER_REMOVE_PROPERTIES,
    METHOD_HANDLER_MAX
};

static pa_dbus_arg_info update_properties_args[] = { { "property_list", "a{say}", "in" }, { "update_mode", "u", "in" } };
static pa_dbus_arg_info update_properties_args[] = { { "keys", "as", "in" } };

static pa_dbus_method_handler method_handlers[METHOD_HANDLER_MAX] = {
    [METHOD_HANDLER_KILL] = {
        .method_name = "Kill",
        .arguments = NULL,
        .n_arguments = 0,
        .receive_cb = handle_kill },
    [METHOD_HANDLER_UPDATE_PROPERTIES] = {
        .method_name = "UpdateProperties",
        .arguments = update_propertes_args,
        .n_arguments = sizeof(update_properties_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = handle_update_properties },
    [METHOD_HANDLER_REMOVE_PROPERTIES] = {
        .method_name = "RemoveProperties",
        .arguments = remove_propertes_args,
        .n_arguments = sizeof(update_properties_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = handle_update_properties }
};

enum signal_index {
    SIGNAL_PROPERTY_LIST_UPDATED,
    SIGNAL_CLIENT_EVENT,
    SIGNAL_MAX
};

static pa_dbus_arg_info active_profile_updated_args[] = { { "profile",       "o",      NULL } };
static pa_dbus_arg_info property_list_updated_args[]  = { { "property_list", "a{say}", NULL } };

static pa_dbus_signal_info signals[SIGNAL_MAX] = {
    [SIGNAL_ACTIVE_PROFILE_UPDATED] = { .name = "ActiveProfileUpdated", .arguments = active_profile_updated_args, .n_arguments = 1 },
    [SIGNAL_PROPERTY_LIST_UPDATED]  = { .name = "PropertyListUpdated",  .arguments = property_list_updated_args,  .n_arguments = 1 }
};*/

static pa_dbus_interface_info client_interface_info = {
    .name = PA_DBUSIFACE_CLIENT_INTERFACE,
    .method_handlers = /*method_handlers*/ NULL,
    .n_method_handlers = /*METHOD_HANDLER_MAX*/ 0,
    .property_handlers = property_handlers,
    .n_property_handlers = PROPERTY_HANDLER_MAX,
    .get_all_properties_cb = handle_get_all,
    .signals = /*signals*/ NULL,
    .n_signals = /*SIGNAL_MAX*/ 0
};

static void handle_get_index(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_client *c = userdata;
    dbus_uint32_t idx = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    idx = c->client->index;

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &idx);
}

static void handle_get_driver(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_client *c = userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_STRING, &c->client->driver);
}

static void handle_get_owner_module(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_client *c = userdata;
    const char *owner_module = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    if (!c->client->module) {
        pa_dbus_send_error(conn, msg, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "Client %d doesn't have an owner module.", c->client->index);
        return;
    }

    owner_module = pa_dbusiface_core_get_module_path(c->core, c->client->module);

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &owner_module);
}

/* The caller frees the array, but not the strings. */
static const char **get_playback_streams(pa_dbusiface_client *c, unsigned *n) {
    const char **playback_streams = NULL;
    unsigned i = 0;
    uint32_t idx = 0;
    pa_sink_input *sink_input = NULL;

    pa_assert(c);
    pa_assert(n);

    *n = pa_idxset_size(c->client->sink_inputs);

    if (*n == 0)
        return NULL;

    playback_streams = pa_xnew(const char *, *n);

    PA_IDXSET_FOREACH(sink_input, c->client->sink_inputs, idx)
        playback_streams[i++] = pa_dbusiface_core_get_playback_stream_path(c->core, sink_input);

    return playback_streams;
}

static void handle_get_playback_streams(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_client *c = userdata;
    const char **playback_streams = NULL;
    unsigned n_playback_streams = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    playback_streams = get_playback_streams(c, &n_playback_streams);

    pa_dbus_send_basic_array_variant_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, playback_streams, n_playback_streams);

    pa_xfree(playback_streams);
}

/* The caller frees the array, but not the strings. */
static const char **get_record_streams(pa_dbusiface_client *c, unsigned *n) {
    const char **record_streams = NULL;
    unsigned i = 0;
    uint32_t idx = 0;
    pa_source_output *source_output = NULL;

    pa_assert(c);
    pa_assert(n);

    *n = pa_idxset_size(c->client->source_outputs);

    if (*n == 0)
        return NULL;

    record_streams = pa_xnew(const char *, *n);

    PA_IDXSET_FOREACH(source_output, c->client->source_outputs, idx)
        record_streams[i++] = pa_dbusiface_core_get_record_stream_path(c->core, source_output);

    return record_streams;
}

static void handle_get_record_streams(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_client *c = userdata;
    const char **record_streams = NULL;
    unsigned n_record_streams = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    record_streams = get_record_streams(c, &n_record_streams);

    pa_dbus_send_basic_array_variant_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, record_streams, n_record_streams);

    pa_xfree(record_streams);
}

static void handle_get_property_list(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_client *c = userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    pa_dbus_send_proplist_variant_reply(conn, msg, c->client->proplist);
}

static void handle_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_client *c = userdata;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter;
    DBusMessageIter dict_iter;
    dbus_uint32_t idx = 0;
    const char *owner_module = NULL;
    const char **playback_streams = NULL;
    unsigned n_playback_streams = 0;
    const char **record_streams = NULL;
    unsigned n_record_streams = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);

    idx = c->client->index;
    if (c->client->module)
        owner_module = pa_dbusiface_core_get_module_path(c->core, c->client->module);
    playback_streams = get_playback_streams(c, &n_playback_streams);
    record_streams = get_record_streams(c, &n_record_streams);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));

    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    pa_dbus_append_basic_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_INDEX].property_name, DBUS_TYPE_UINT32, &idx);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_DRIVER].property_name, DBUS_TYPE_STRING, &c->client->driver);

    if (owner_module)
        pa_dbus_append_basic_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_OWNER_MODULE].property_name, DBUS_TYPE_OBJECT_PATH, &owner_module);

    pa_dbus_append_basic_array_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_PLAYBACK_STREAMS].property_name, DBUS_TYPE_OBJECT_PATH, playback_streams, n_playback_streams);
    pa_dbus_append_basic_array_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_RECORD_STREAMS].property_name, DBUS_TYPE_OBJECT_PATH, record_streams, n_record_streams);
    pa_dbus_append_proplist_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_PROPERTY_LIST].property_name, c->client->proplist);

    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));

    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_message_unref(reply);

    pa_xfree(playback_streams);
    pa_xfree(record_streams);
}

pa_dbusiface_client *pa_dbusiface_client_new(pa_dbusiface_core *core, pa_client *client) {
    pa_dbusiface_client *c = NULL;

    pa_assert(core);
    pa_assert(client);

    c = pa_xnew(pa_dbusiface_client, 1);
    c->core = core;
    c->client = client;
    c->path = pa_sprintf_malloc("%s/%s%u", PA_DBUS_CORE_OBJECT_PATH, OBJECT_NAME, client->index);
    c->dbus_protocol = pa_dbus_protocol_get(client->core);

    pa_assert_se(pa_dbus_protocol_add_interface(c->dbus_protocol, c->path, &client_interface_info, c) >= 0);

    return c;
}

void pa_dbusiface_client_free(pa_dbusiface_client *c) {
    pa_assert(c);

    pa_assert_se(pa_dbus_protocol_remove_interface(c->dbus_protocol, c->path, client_interface_info.name) >= 0);

    pa_dbus_protocol_unref(c->dbus_protocol);

    pa_xfree(c->path);
    pa_xfree(c);
}

const char *pa_dbusiface_client_get_path(pa_dbusiface_client *c) {
    pa_assert(c);

    return c->path;
}
