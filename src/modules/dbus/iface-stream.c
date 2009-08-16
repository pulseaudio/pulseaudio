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

#include <pulsecore/core-util.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/protocol-dbus.h>

#include "iface-stream.h"

#define PLAYBACK_OBJECT_NAME "playback_stream"
#define RECORD_OBJECT_NAME "record_stream"

enum stream_type {
    STREAM_TYPE_PLAYBACK,
    STREAM_TYPE_RECORD
};

struct pa_dbusiface_stream {
    union {
        pa_sink_input *sink_input;
        pa_source_output *source_output;
    };
    enum stream_type type;
    char *path;
    pa_cvolume volume;
    pa_bool_t is_muted;
    pa_proplist *proplist;

    pa_dbus_protocol *dbus_protocol;
    pa_subscription *subscription;
};

static void handle_get_index(DBusConnection *conn, DBusMessage *msg, void *userdata);
/*static void handle_get_driver(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_owner_module(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_client(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_device(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_sample_format(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_sample_rate(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_channels(DBusConnection *conn, DBusMessage *msg, void *userdata);*/
static void handle_get_volume(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_set_volume(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_is_muted(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_set_is_muted(DBusConnection *conn, DBusMessage *msg, void *userdata);
/*static void handle_get_buffer_latency(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_device_latency(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_get_resample_method(DBusConnection *conn, DBusMessage *msg, void *userdata);*/
static void handle_get_property_list(DBusConnection *conn, DBusMessage *msg, void *userdata);

static void handle_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata);

/*static void handle_move(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void handle_kill(DBusConnection *conn, DBusMessage *msg, void *userdata);*/

enum property_handler_index {
    PROPERTY_HANDLER_INDEX,
/*    PROPERTY_HANDLER_DRIVER,
    PROPERTY_HANDLER_OWNER_MODULE,
    PROPERTY_HANDLER_CLIENT,
    PROPERTY_HANDLER_DEVICE,
    PROPERTY_HANDLER_SAMPLE_FORMAT,
    PROPERTY_HANDLER_SAMPLE_RATE,
    PROPERTY_HANDLER_CHANNELS,*/
    PROPERTY_HANDLER_VOLUME,
    PROPERTY_HANDLER_IS_MUTED,
/*    PROPERTY_HANDLER_BUFFER_LATENCY,
    PROPERTY_HANDLER_DEVICE_LATENCY,
    PROPERTY_HANDLER_RESAMPLE_METHOD,*/
    PROPERTY_HANDLER_PROPERTY_LIST,
    PROPERTY_HANDLER_MAX
};

static pa_dbus_property_handler property_handlers[PROPERTY_HANDLER_MAX] = {
    [PROPERTY_HANDLER_INDEX]           = { .property_name = "Index",          .type = "u",      .get_cb = handle_get_index,           .set_cb = NULL },
/*    [PROPERTY_HANDLER_DRIVER]          = { .property_name = "Driver",         .type = "s",      .get_cb = handle_get_driver,          .set_cb = NULL },
    [PROPERTY_HANDLER_OWNER_MODULE]    = { .property_name = "OwnerModule",    .type = "o",      .get_cb = handle_get_owner_module,    .set_cb = NULL },
    [PROPERTY_HANDLER_CLIENT]          = { .property_name = "Client",         .type = "o",      .get_cb = handle_get_client,          .set_cb = NULL },
    [PROPERTY_HANDLER_DEVICE]          = { .property_name = "Device",         .type = "o",      .get_cb = handle_get_device,          .set_cb = NULL },
    [PROPERTY_HANDLER_SAMPLE_FORMAT]   = { .property_name = "SampleFormat",   .type = "u",      .get_cb = handle_get_sample_format,   .set_cb = NULL },
    [PROPERTY_HANDLER_SAMPLE_RATE]     = { .property_name = "SampleRate",     .type = "u",      .get_cb = handle_get_sample_rate,     .set_cb = NULL },
    [PROPERTY_HANDLER_CHANNELS]        = { .property_name = "Channels",       .type = "au",     .get_cb = handle_get_channels,        .set_cb = NULL },*/
    [PROPERTY_HANDLER_VOLUME]          = { .property_name = "Volume",         .type = "au",     .get_cb = handle_get_volume,          .set_cb = handle_set_volume },
    [PROPERTY_HANDLER_IS_MUTED]        = { .property_name = "IsMuted",        .type = "b",      .get_cb = handle_get_is_muted,        .set_cb = handle_set_is_muted },
/*    [PROPERTY_HANDLER_BUFFER_LATENCY]  = { .property_name = "BufferLatency",  .type = "t",      .get_cb = handle_get_buffer_latency,  .set_cb = NULL },
    [PROPERTY_HANDLER_DEVICE_LATENCY]  = { .property_name = "DeviceLatency",  .type = "t",      .get_cb = handle_get_device_latency,  .set_cb = NULL },
    [PROPERTY_HANDLER_RESAMPLE_METHOD] = { .property_name = "ResampleMethod", .type = "s",      .get_cb = handle_get_resample_method, .set_cb = NULL },*/
    [PROPERTY_HANDLER_PROPERTY_LIST]   = { .property_name = "PropertyList",   .type = "a{say}", .get_cb = handle_get_property_list,   .set_cb = NULL }
};

/*enum method_handler_index {
    METHOD_HANDLER_MOVE,
    METHOD_HANDLER_KILL,
    METHOD_HANDLER_MAX
};

static pa_dbus_arg_info move_args[] = { { "device", "o", "in" } };

static pa_dbus_method_handler method_handlers[METHOD_HANDLER_MAX] = {
    [METHOD_HANDLER_MOVE] = {
        .method_name = "Move",
        .arguments = move_args,
        .n_arguments = sizeof(move_args) / sizeof(pa_dbus_arg_info),
        .receive_cb = handle_move },
    [METHOD_HANDLER_KILL] = {
        .method_name = "Kill",
        .arguments = NULL,
        .n_arguments = 0,
        .receive_cb = handle_kill }
};*/

enum signal_index {
/*    SIGNAL_DEVICE_UPDATED,
    SIGNAL_SAMPLE_RATE_UPDATED,*/
    SIGNAL_VOLUME_UPDATED,
    SIGNAL_MUTE_UPDATED,
    SIGNAL_PROPERTY_LIST_UPDATED,
/*    SIGNAL_STREAM_EVENT,*/
    SIGNAL_MAX
};

/*static pa_dbus_arg_info device_updated_args[]        = { { "device",        "o",      NULL } };
static pa_dbus_arg_info sample_rate_updated_args[]   = { { "sample_rate",   "u",      NULL } };*/
static pa_dbus_arg_info volume_updated_args[]        = { { "volume",        "au",     NULL } };
static pa_dbus_arg_info mute_updated_args[]          = { { "muted",         "b",      NULL } };
static pa_dbus_arg_info property_list_updated_args[] = { { "property_list", "a{say}", NULL } };
/*static pa_dbus_arg_info stream_event_args[]          = { { "name",          "s",      NULL }, { "property_list", "a{say}", NULL } };*/

static pa_dbus_signal_info signals[SIGNAL_MAX] = {
/*    [SIGNAL_DEVICE_UPDATED]        = { .name = "DeviceUpdated",       .arguments = device_updated_args,        .n_arguments = 1 },
    [SIGNAL_SAMPLE_RATE_UPDATED]   = { .name = "SampleRateUpdated",   .arguments = sample_rate_updated_args,   .n_arguments = 1 },*/
    [SIGNAL_VOLUME_UPDATED]        = { .name = "VolumeUpdated",       .arguments = volume_updated_args,        .n_arguments = 1 },
    [SIGNAL_MUTE_UPDATED]          = { .name = "MuteUpdated",         .arguments = mute_updated_args,          .n_arguments = 1 },
    [SIGNAL_PROPERTY_LIST_UPDATED] = { .name = "PropertyListUpdated", .arguments = property_list_updated_args, .n_arguments = 1 }/*,
    [SIGNAL_STREAM_EVENT]          = { .name = "StreamEvent",         .arguments = stream_event_args,          .n_arguments = sizeof(stream_event_args) / sizeof(pa_dbus_arg_info) }*/
};

static pa_dbus_interface_info stream_interface_info = {
    .name = PA_DBUSIFACE_STREAM_INTERFACE,
    .method_handlers = /*method_handlers*/ NULL,
    .n_method_handlers = /*METHOD_HANDLER_MAX*/ 0,
    .property_handlers = property_handlers,
    .n_property_handlers = PROPERTY_HANDLER_MAX,
    .get_all_properties_cb = handle_get_all,
    .signals = signals,
    .n_signals = SIGNAL_MAX
};

static void handle_get_index(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_stream *s = userdata;
    dbus_uint32_t idx;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(s);

    idx = (s->type == STREAM_TYPE_PLAYBACK) ? s->sink_input->index : s->source_output->index;

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &idx);
}

static void handle_get_volume(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_stream *s = userdata;
    dbus_uint32_t volume[PA_CHANNELS_MAX];
    unsigned i = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(s);

    if (s->type == STREAM_TYPE_RECORD) {
        pa_dbus_send_error(conn, msg, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "Record streams don't have volume.");
        return;
    }

    for (i = 0; i < s->volume.channels; ++i)
        volume[i] = s->volume.values[i];

    pa_dbus_send_basic_array_variant_reply(conn, msg, DBUS_TYPE_UINT32, volume, s->volume.channels);
}

static void handle_set_volume(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_stream *s = userdata;
    unsigned stream_channels = 0;
    dbus_uint32_t *volume = NULL;
    unsigned n_volume_entries = 0;
    pa_cvolume new_vol;
    unsigned i = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(s);

    if (s->type == STREAM_TYPE_RECORD) {
        pa_dbus_send_error(conn, msg, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "Record streams don't have volume.");
        return;
    }

    pa_cvolume_init(&new_vol);

    stream_channels = s->sink_input->channel_map.channels;

    new_vol.channels = stream_channels;

    if (pa_dbus_get_fixed_array_set_property_arg(conn, msg, DBUS_TYPE_UINT32, &volume, &n_volume_entries) < 0)
        return;

    if (n_volume_entries != stream_channels) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected %u volume entries, got %u.", stream_channels, n_volume_entries);
        return;
    }

    for (i = 0; i < n_volume_entries; ++i) {
        if (volume[i] > PA_VOLUME_MAX) {
            pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Too large volume value: %u", volume[i]);
            return;
        }
        new_vol.values[i] = volume[i];
    }

    pa_sink_input_set_volume(s->sink_input, &new_vol, TRUE, TRUE);

    pa_dbus_send_empty_reply(conn, msg);
}

static void handle_get_is_muted(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_stream *s = userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(s);

    if (s->type == STREAM_TYPE_RECORD) {
        pa_dbus_send_error(conn, msg, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "Record streams don't have mute.");
        return;
    }

    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_BOOLEAN, &s->is_muted);
}

static void handle_set_is_muted(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_stream *s = userdata;
    dbus_bool_t is_muted = FALSE;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(s);

    if (pa_dbus_get_basic_set_property_arg(conn, msg, DBUS_TYPE_BOOLEAN, &is_muted) < 0)
        return;

    if (s->type == STREAM_TYPE_RECORD) {
        pa_dbus_send_error(conn, msg, PA_DBUS_ERROR_NO_SUCH_PROPERTY, "Record streams don't have mute.");
        return;
    }

    pa_sink_input_set_mute(s->sink_input, is_muted, TRUE);

    pa_dbus_send_empty_reply(conn, msg);
};

static void handle_get_property_list(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_stream *s = userdata;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(s);

    pa_dbus_send_proplist_variant_reply(conn, msg, s->proplist);
}

static void handle_get_all(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    pa_dbusiface_stream *s = userdata;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter;
    DBusMessageIter dict_iter;
    dbus_uint32_t idx;
    dbus_uint32_t volume[PA_CHANNELS_MAX];
    unsigned i = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(s);

    idx = (s->type == STREAM_TYPE_PLAYBACK) ? s->sink_input->index : s->source_output->index;
    if (s->type == STREAM_TYPE_PLAYBACK) {
        for (i = 0; i < s->volume.channels; ++i)
            volume[i] = s->volume.values[i];
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));

    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    pa_dbus_append_basic_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_INDEX].property_name, DBUS_TYPE_UINT32, &idx);

    if (s->type == STREAM_TYPE_PLAYBACK) {
        pa_dbus_append_basic_array_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_VOLUME].property_name, DBUS_TYPE_UINT32, volume, s->volume.channels);
        pa_dbus_append_basic_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_IS_MUTED].property_name, DBUS_TYPE_BOOLEAN, &s->is_muted);
    }

    pa_dbus_append_proplist_variant_dict_entry(&dict_iter, property_handlers[PROPERTY_HANDLER_PROPERTY_LIST].property_name, s->proplist);

    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));
    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

static void subscription_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    pa_dbusiface_stream *s = userdata;

    pa_assert(c);
    pa_assert(s);

    if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_CHANGE) {
        DBusMessage *signal = NULL;
        pa_proplist *new_proplist = NULL;
        unsigned i = 0;

        pa_assert(((s->type == STREAM_TYPE_PLAYBACK)
                    && ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT))
                  || ((s->type == STREAM_TYPE_RECORD)
                       && ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT)));

        if (s->type == STREAM_TYPE_PLAYBACK) {
            pa_cvolume new_volume;
            pa_bool_t new_muted = FALSE;

            pa_sink_input_get_volume(s->sink_input, &new_volume, TRUE);

            if (!pa_cvolume_equal(&s->volume, &new_volume)) {
                dbus_uint32_t volume[PA_CHANNELS_MAX];
                dbus_uint32_t *volume_ptr = volume;

                s->volume = new_volume;

                for (i = 0; i < s->volume.channels; ++i)
                    volume[i] = s->volume.values[i];

                pa_assert_se(signal = dbus_message_new_signal(s->path,
                                                              PA_DBUSIFACE_STREAM_INTERFACE,
                                                              signals[SIGNAL_VOLUME_UPDATED].name));
                pa_assert_se(dbus_message_append_args(signal,
                                                      DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &volume_ptr, s->volume.channels,
                                                      DBUS_TYPE_INVALID));

                pa_dbus_protocol_send_signal(s->dbus_protocol, signal);
                dbus_message_unref(signal);
                signal = NULL;
            }

            new_muted = pa_sink_input_get_mute(s->sink_input);

            if (s->is_muted != new_muted) {
                s->is_muted = new_muted;

                pa_assert_se(signal = dbus_message_new_signal(s->path,
                                                              PA_DBUSIFACE_STREAM_INTERFACE,
                                                              signals[SIGNAL_MUTE_UPDATED].name));
                pa_assert_se(dbus_message_append_args(signal, DBUS_TYPE_BOOLEAN, &s->is_muted, DBUS_TYPE_INVALID));

                pa_dbus_protocol_send_signal(s->dbus_protocol, signal);
                dbus_message_unref(signal);
                signal = NULL;
            }
        }

        new_proplist = (s->type == STREAM_TYPE_PLAYBACK) ? s->sink_input->proplist : s->source_output->proplist;

        if (!pa_proplist_equal(s->proplist, new_proplist)) {
            DBusMessageIter msg_iter;

            pa_proplist_update(s->proplist, PA_UPDATE_SET, new_proplist);

            pa_assert_se(signal = dbus_message_new_signal(s->path,
                                                          PA_DBUSIFACE_STREAM_INTERFACE,
                                                          signals[SIGNAL_PROPERTY_LIST_UPDATED].name));
            dbus_message_iter_init_append(signal, &msg_iter);
            pa_dbus_append_proplist(&msg_iter, s->proplist);

            pa_dbus_protocol_send_signal(s->dbus_protocol, signal);
            dbus_message_unref(signal);
            signal = NULL;
        }
    }
}

pa_dbusiface_stream *pa_dbusiface_stream_new_playback(pa_dbusiface_core *core, pa_sink_input *sink_input) {
    pa_dbusiface_stream *s;

    pa_assert(core);
    pa_assert(sink_input);

    s = pa_xnew(pa_dbusiface_stream, 1);
    s->sink_input = pa_sink_input_ref(sink_input);
    s->type = STREAM_TYPE_PLAYBACK;
    s->path = pa_sprintf_malloc("%s/%s%u", PA_DBUS_CORE_OBJECT_PATH, PLAYBACK_OBJECT_NAME, sink_input->index);
    pa_sink_input_get_volume(sink_input, &s->volume, TRUE);
    s->is_muted = pa_sink_input_get_mute(sink_input);
    s->proplist = pa_proplist_copy(sink_input->proplist);
    s->dbus_protocol = pa_dbus_protocol_get(sink_input->core);
    s->subscription = pa_subscription_new(sink_input->core, PA_SUBSCRIPTION_MASK_SINK_INPUT, subscription_cb, s);

    pa_assert_se(pa_dbus_protocol_add_interface(s->dbus_protocol, s->path, &stream_interface_info, s) >= 0);

    return s;
}

pa_dbusiface_stream *pa_dbusiface_stream_new_record(pa_dbusiface_core *core, pa_source_output *source_output) {
    pa_dbusiface_stream *s;

    pa_assert(core);
    pa_assert(source_output);

    s = pa_xnew(pa_dbusiface_stream, 1);
    s->source_output = pa_source_output_ref(source_output);
    s->type = STREAM_TYPE_RECORD;
    s->path = pa_sprintf_malloc("%s/%s%u", PA_DBUS_CORE_OBJECT_PATH, RECORD_OBJECT_NAME, source_output->index);
    pa_cvolume_init(&s->volume);
    s->is_muted = FALSE;
    s->proplist = pa_proplist_copy(source_output->proplist);
    s->dbus_protocol = pa_dbus_protocol_get(source_output->core);
    s->subscription = pa_subscription_new(source_output->core, PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, subscription_cb, s);

    pa_assert_se(pa_dbus_protocol_add_interface(s->dbus_protocol, s->path, &stream_interface_info, s) >= 0);

    return s;
}

void pa_dbusiface_stream_free(pa_dbusiface_stream *s) {
    pa_assert(s);

    pa_assert_se(pa_dbus_protocol_remove_interface(s->dbus_protocol, s->path, stream_interface_info.name) >= 0);

    if (s->type == STREAM_TYPE_PLAYBACK)
        pa_sink_input_unref(s->sink_input);
    else
        pa_source_output_unref(s->source_output);

    pa_proplist_free(s->proplist);
    pa_dbus_protocol_unref(s->dbus_protocol);
    pa_subscription_free(s->subscription);

    pa_xfree(s->path);
    pa_xfree(s);
}

const char *pa_dbusiface_stream_get_path(pa_dbusiface_stream *s) {
    pa_assert(s);

    return s->path;
}
