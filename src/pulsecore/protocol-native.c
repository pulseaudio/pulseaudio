/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <pulse/timeval.h>
#include <pulse/version.h>
#include <pulse/utf8.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/native-common.h>
#include <pulsecore/packet.h>
#include <pulsecore/client.h>
#include <pulsecore/source-output.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/pstream.h>
#include <pulsecore/tagstruct.h>
#include <pulsecore/pdispatch.h>
#include <pulsecore/pstream-util.h>
#include <pulsecore/authkey.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/autoload.h>
#include <pulsecore/authkey-prop.h>
#include <pulsecore/strlist.h>
#include <pulsecore/props.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/llist.h>
#include <pulsecore/creds.h>
#include <pulsecore/core-util.h>
#include <pulsecore/ipacl.h>
#include <pulsecore/thread-mq.h>

#include "protocol-native.h"

/* Kick a client if it doesn't authenticate within this time */
#define AUTH_TIMEOUT 60

/* Don't accept more connection than this */
#define MAX_CONNECTIONS 64

#define MAX_MEMBLOCKQ_LENGTH (4*1024*1024) /* 4MB */

typedef struct connection connection;
struct pa_protocol_native;

typedef struct record_stream {
    pa_msgobject parent;

    connection *connection;
    uint32_t index;

    pa_source_output *source_output;
    pa_memblockq *memblockq;
    size_t fragment_size;
} record_stream;

typedef struct output_stream {
    pa_msgobject parent;
} output_stream;

typedef struct playback_stream {
    output_stream parent;

    connection *connection;
    uint32_t index;

    pa_sink_input *sink_input;
    pa_memblockq *memblockq;
    int drain_request;
    uint32_t drain_tag;
    uint32_t syncid;
    int underrun;

    pa_atomic_t missing;
    size_t minreq;

    /* Only updated after SINK_INPUT_MESSAGE_UPDATE_LATENCY */
    int64_t read_index, write_index;
    size_t resampled_chunk_length;
} playback_stream;

typedef struct upload_stream {
    output_stream parent;

    connection *connection;
    uint32_t index;

    pa_memchunk memchunk;
    size_t length;
    char *name;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
} upload_stream;

struct connection {
    pa_msgobject parent;

    int authorized;
    uint32_t version;
    pa_protocol_native *protocol;
    pa_client *client;
    pa_pstream *pstream;
    pa_pdispatch *pdispatch;
    pa_idxset *record_streams, *output_streams;
    uint32_t rrobin_index;
    pa_subscription *subscription;
    pa_time_event *auth_timeout_event;
};

PA_DECLARE_CLASS(record_stream);
#define RECORD_STREAM(o) (record_stream_cast(o))
static PA_DEFINE_CHECK_TYPE(record_stream, pa_msgobject);

PA_DECLARE_CLASS(output_stream);
#define OUTPUT_STREAM(o) (output_stream_cast(o))
static PA_DEFINE_CHECK_TYPE(output_stream, pa_msgobject);

PA_DECLARE_CLASS(playback_stream);
#define PLAYBACK_STREAM(o) (playback_stream_cast(o))
static PA_DEFINE_CHECK_TYPE(playback_stream, output_stream);

PA_DECLARE_CLASS(upload_stream);
#define UPLOAD_STREAM(o) (upload_stream_cast(o))
static PA_DEFINE_CHECK_TYPE(upload_stream, output_stream);

PA_DECLARE_CLASS(connection);
#define CONNECTION(o) (connection_cast(o))
static PA_DEFINE_CHECK_TYPE(connection, pa_msgobject);

struct pa_protocol_native {
    pa_module *module;
    pa_core *core;
    int public;
    pa_socket_server *server;
    pa_idxset *connections;
    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];
    int auth_cookie_in_property;
#ifdef HAVE_CREDS
    char *auth_group;
#endif
    pa_ip_acl *auth_ip_acl;
};

enum {
    SINK_INPUT_MESSAGE_POST_DATA = PA_SINK_INPUT_MESSAGE_MAX, /* data from main loop to sink input */
    SINK_INPUT_MESSAGE_DRAIN, /* disabled prebuf, get playback started. */
    SINK_INPUT_MESSAGE_FLUSH,
    SINK_INPUT_MESSAGE_TRIGGER,
    SINK_INPUT_MESSAGE_SEEK,
    SINK_INPUT_MESSAGE_PREBUF_FORCE,
    SINK_INPUT_MESSAGE_UPDATE_LATENCY
};

enum {
    PLAYBACK_STREAM_MESSAGE_REQUEST_DATA,      /* data requested from sink input from the main loop */
    PLAYBACK_STREAM_MESSAGE_UNDERFLOW,
    PLAYBACK_STREAM_MESSAGE_OVERFLOW,
    PLAYBACK_STREAM_MESSAGE_DRAIN_ACK
};

enum {
    RECORD_STREAM_MESSAGE_POST_DATA         /* data from source output to main loop */
};

enum {
    CONNECTION_MESSAGE_RELEASE,
    CONNECTION_MESSAGE_REVOKE
};

static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk);
static void sink_input_drop_cb(pa_sink_input *i, size_t length);
static void sink_input_kill_cb(pa_sink_input *i);
static void sink_input_suspend_cb(pa_sink_input *i, pa_bool_t suspend);
static void sink_input_moved_cb(pa_sink_input *i);

static void send_memblock(connection *c);
static void request_bytes(struct playback_stream*s);

static void source_output_kill_cb(pa_source_output *o);
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk);
static void source_output_suspend_cb(pa_source_output *o, pa_bool_t suspend);
static void source_output_moved_cb(pa_source_output *o);
static pa_usec_t source_output_get_latency_cb(pa_source_output *o);

static int sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk);

static void command_exit(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_create_playback_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_drain_playback_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_create_record_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_delete_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_auth(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_set_client_name(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_lookup(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_stat(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_get_playback_latency(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_get_record_latency(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_create_upload_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_finish_upload_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_play_sample(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_remove_sample(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_get_info(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_get_info_list(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_get_server_info(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_subscribe(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_set_volume(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_set_mute(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_cork_playback_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_trigger_or_flush_or_prebuf_playback_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_set_default_sink_or_source(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_set_stream_name(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_kill(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_load_module(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_unload_module(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_add_autoload(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_remove_autoload(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_get_autoload_info(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_get_autoload_info_list(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_cork_record_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_flush_record_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_move_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_suspend(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_set_stream_buffer_attr(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_update_stream_sample_rate(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

static const pa_pdispatch_cb_t command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_ERROR] = NULL,
    [PA_COMMAND_TIMEOUT] = NULL,
    [PA_COMMAND_REPLY] = NULL,
    [PA_COMMAND_CREATE_PLAYBACK_STREAM] = command_create_playback_stream,
    [PA_COMMAND_DELETE_PLAYBACK_STREAM] = command_delete_stream,
    [PA_COMMAND_DRAIN_PLAYBACK_STREAM] = command_drain_playback_stream,
    [PA_COMMAND_CREATE_RECORD_STREAM] = command_create_record_stream,
    [PA_COMMAND_DELETE_RECORD_STREAM] = command_delete_stream,
    [PA_COMMAND_AUTH] = command_auth,
    [PA_COMMAND_REQUEST] = NULL,
    [PA_COMMAND_EXIT] = command_exit,
    [PA_COMMAND_SET_CLIENT_NAME] = command_set_client_name,
    [PA_COMMAND_LOOKUP_SINK] = command_lookup,
    [PA_COMMAND_LOOKUP_SOURCE] = command_lookup,
    [PA_COMMAND_STAT] = command_stat,
    [PA_COMMAND_GET_PLAYBACK_LATENCY] = command_get_playback_latency,
    [PA_COMMAND_GET_RECORD_LATENCY] = command_get_record_latency,
    [PA_COMMAND_CREATE_UPLOAD_STREAM] = command_create_upload_stream,
    [PA_COMMAND_DELETE_UPLOAD_STREAM] = command_delete_stream,
    [PA_COMMAND_FINISH_UPLOAD_STREAM] = command_finish_upload_stream,
    [PA_COMMAND_PLAY_SAMPLE] = command_play_sample,
    [PA_COMMAND_REMOVE_SAMPLE] = command_remove_sample,
    [PA_COMMAND_GET_SINK_INFO] = command_get_info,
    [PA_COMMAND_GET_SOURCE_INFO] = command_get_info,
    [PA_COMMAND_GET_CLIENT_INFO] = command_get_info,
    [PA_COMMAND_GET_MODULE_INFO] = command_get_info,
    [PA_COMMAND_GET_SINK_INPUT_INFO] = command_get_info,
    [PA_COMMAND_GET_SOURCE_OUTPUT_INFO] = command_get_info,
    [PA_COMMAND_GET_SAMPLE_INFO] = command_get_info,
    [PA_COMMAND_GET_SINK_INFO_LIST] = command_get_info_list,
    [PA_COMMAND_GET_SOURCE_INFO_LIST] = command_get_info_list,
    [PA_COMMAND_GET_MODULE_INFO_LIST] = command_get_info_list,
    [PA_COMMAND_GET_CLIENT_INFO_LIST] = command_get_info_list,
    [PA_COMMAND_GET_SINK_INPUT_INFO_LIST] = command_get_info_list,
    [PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST] = command_get_info_list,
    [PA_COMMAND_GET_SAMPLE_INFO_LIST] = command_get_info_list,
    [PA_COMMAND_GET_SERVER_INFO] = command_get_server_info,
    [PA_COMMAND_SUBSCRIBE] = command_subscribe,

    [PA_COMMAND_SET_SINK_VOLUME] = command_set_volume,
    [PA_COMMAND_SET_SINK_INPUT_VOLUME] = command_set_volume,
    [PA_COMMAND_SET_SOURCE_VOLUME] = command_set_volume,

    [PA_COMMAND_SET_SINK_MUTE] = command_set_mute,
    [PA_COMMAND_SET_SINK_INPUT_MUTE] = command_set_mute,
    [PA_COMMAND_SET_SOURCE_MUTE] = command_set_mute,

    [PA_COMMAND_SUSPEND_SINK] = command_suspend,
    [PA_COMMAND_SUSPEND_SOURCE] = command_suspend,

    [PA_COMMAND_CORK_PLAYBACK_STREAM] = command_cork_playback_stream,
    [PA_COMMAND_FLUSH_PLAYBACK_STREAM] = command_trigger_or_flush_or_prebuf_playback_stream,
    [PA_COMMAND_TRIGGER_PLAYBACK_STREAM] = command_trigger_or_flush_or_prebuf_playback_stream,
    [PA_COMMAND_PREBUF_PLAYBACK_STREAM] = command_trigger_or_flush_or_prebuf_playback_stream,

    [PA_COMMAND_CORK_RECORD_STREAM] = command_cork_record_stream,
    [PA_COMMAND_FLUSH_RECORD_STREAM] = command_flush_record_stream,

    [PA_COMMAND_SET_DEFAULT_SINK] = command_set_default_sink_or_source,
    [PA_COMMAND_SET_DEFAULT_SOURCE] = command_set_default_sink_or_source,
    [PA_COMMAND_SET_PLAYBACK_STREAM_NAME] = command_set_stream_name,
    [PA_COMMAND_SET_RECORD_STREAM_NAME] = command_set_stream_name,
    [PA_COMMAND_KILL_CLIENT] = command_kill,
    [PA_COMMAND_KILL_SINK_INPUT] = command_kill,
    [PA_COMMAND_KILL_SOURCE_OUTPUT] = command_kill,
    [PA_COMMAND_LOAD_MODULE] = command_load_module,
    [PA_COMMAND_UNLOAD_MODULE] = command_unload_module,
    [PA_COMMAND_GET_AUTOLOAD_INFO] = command_get_autoload_info,
    [PA_COMMAND_GET_AUTOLOAD_INFO_LIST] = command_get_autoload_info_list,
    [PA_COMMAND_ADD_AUTOLOAD] = command_add_autoload,
    [PA_COMMAND_REMOVE_AUTOLOAD] = command_remove_autoload,

    [PA_COMMAND_MOVE_SINK_INPUT] = command_move_stream,
    [PA_COMMAND_MOVE_SOURCE_OUTPUT] = command_move_stream,

    [PA_COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR] = command_set_stream_buffer_attr,
    [PA_COMMAND_SET_RECORD_STREAM_BUFFER_ATTR] = command_set_stream_buffer_attr,

    [PA_COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE] = command_update_stream_sample_rate,
    [PA_COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE] = command_update_stream_sample_rate
};

/* structure management */

static void upload_stream_unlink(upload_stream *s) {
    pa_assert(s);

    if (!s->connection)
        return;

    pa_assert_se(pa_idxset_remove_by_data(s->connection->output_streams, s, NULL) == s);
    s->connection = NULL;
    upload_stream_unref(s);
}

static void upload_stream_free(pa_object *o) {
    upload_stream *s = UPLOAD_STREAM(o);
    pa_assert(s);

    upload_stream_unlink(s);

    pa_xfree(s->name);

    if (s->memchunk.memblock)
        pa_memblock_unref(s->memchunk.memblock);

    pa_xfree(s);
}

static upload_stream* upload_stream_new(
        connection *c,
        const pa_sample_spec *ss,
        const pa_channel_map *map,
        const char *name, size_t length) {

    upload_stream *s;

    pa_assert(c);
    pa_assert(ss);
    pa_assert(name);
    pa_assert(length > 0);

    s = pa_msgobject_new(upload_stream);
    s->parent.parent.parent.free = upload_stream_free;
    s->connection = c;
    s->sample_spec = *ss;
    s->channel_map = *map;
    s->name = pa_xstrdup(name);
    pa_memchunk_reset(&s->memchunk);
    s->length = length;

    pa_idxset_put(c->output_streams, s, &s->index);

    return s;
}

static void record_stream_unlink(record_stream *s) {
    pa_assert(s);

    if (!s->connection)
        return;

    if (s->source_output) {
        pa_source_output_unlink(s->source_output);
        pa_source_output_unref(s->source_output);
        s->source_output = NULL;
    }

    pa_assert_se(pa_idxset_remove_by_data(s->connection->record_streams, s, NULL) == s);
    s->connection = NULL;
    record_stream_unref(s);
}

static void record_stream_free(pa_object *o) {
    record_stream *s = RECORD_STREAM(o);
    pa_assert(s);

    record_stream_unlink(s);

    pa_memblockq_free(s->memblockq);
    pa_xfree(s);
}

static int record_stream_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    record_stream *s = RECORD_STREAM(o);
    record_stream_assert_ref(s);

    if (!s->connection)
        return -1;

    switch (code) {

        case RECORD_STREAM_MESSAGE_POST_DATA:

            if (pa_memblockq_push_align(s->memblockq, chunk) < 0) {
/*                 pa_log_warn("Failed to push data into output queue."); */
                return -1;
            }

            if (!pa_pstream_is_pending(s->connection->pstream))
                send_memblock(s->connection);

            break;
    }

    return 0;
}

static record_stream* record_stream_new(
        connection *c,
        pa_source *source,
        pa_sample_spec *ss,
        pa_channel_map *map,
        const char *name,
        uint32_t *maxlength,
        uint32_t fragment_size,
        pa_source_output_flags_t flags) {

    record_stream *s;
    pa_source_output *source_output;
    size_t base;
    pa_source_output_new_data data;

    pa_assert(c);
    pa_assert(ss);
    pa_assert(name);
    pa_assert(maxlength);
    pa_assert(*maxlength > 0);

    pa_source_output_new_data_init(&data);
    data.module = c->protocol->module;
    data.client = c->client;
    data.source = source;
    data.driver = __FILE__;
    data.name = name;
    pa_source_output_new_data_set_sample_spec(&data, ss);
    pa_source_output_new_data_set_channel_map(&data, map);

    if (!(source_output = pa_source_output_new(c->protocol->core, &data, flags)))
        return NULL;

    s = pa_msgobject_new(record_stream);
    s->parent.parent.free = record_stream_free;
    s->parent.process_msg = record_stream_process_msg;
    s->connection = c;
    s->source_output = source_output;
    s->source_output->push = source_output_push_cb;
    s->source_output->kill = source_output_kill_cb;
    s->source_output->get_latency = source_output_get_latency_cb;
    s->source_output->moved = source_output_moved_cb;
    s->source_output->suspend = source_output_suspend_cb;
    s->source_output->userdata = s;

    s->memblockq = pa_memblockq_new(
            0,
            *maxlength,
            0,
            base = pa_frame_size(&s->source_output->sample_spec),
            1,
            0,
            NULL);

    *maxlength = pa_memblockq_get_maxlength(s->memblockq);

    s->fragment_size = (fragment_size/base)*base;
    if (s->fragment_size <= 0)
        s->fragment_size = base;

    if (s->fragment_size > *maxlength)
        s->fragment_size = *maxlength;

    *ss = s->source_output->sample_spec;
    *map = s->source_output->channel_map;

    pa_idxset_put(c->record_streams, s, &s->index);

    pa_source_output_put(s->source_output);
    return s;
}

static void playback_stream_unlink(playback_stream *s) {
    pa_assert(s);

    if (!s->connection)
        return;

    if (s->sink_input) {
        pa_sink_input_unlink(s->sink_input);
        pa_sink_input_unref(s->sink_input);
        s->sink_input = NULL;
    }

    if (s->drain_request)
        pa_pstream_send_error(s->connection->pstream, s->drain_tag, PA_ERR_NOENTITY);

    pa_assert_se(pa_idxset_remove_by_data(s->connection->output_streams, s, NULL) == s);
    s->connection = NULL;
    playback_stream_unref(s);
}

static void playback_stream_free(pa_object* o) {
    playback_stream *s = PLAYBACK_STREAM(o);
    pa_assert(s);

    playback_stream_unlink(s);

    pa_memblockq_free(s->memblockq);
    pa_xfree(s);
}

static int playback_stream_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    playback_stream *s = PLAYBACK_STREAM(o);
    playback_stream_assert_ref(s);

    if (!s->connection)
        return -1;

    switch (code) {
        case PLAYBACK_STREAM_MESSAGE_REQUEST_DATA: {
            pa_tagstruct *t;
            uint32_t l = 0;

            for (;;) {
                int32_t k;

                if ((k = pa_atomic_load(&s->missing)) <= 0)
                    break;

                l += k;

                if (l < s->minreq)
                    break;

                if (pa_atomic_sub(&s->missing, k) <= k)
                    break;
            }

            if (l < s->minreq)
                break;

            t = pa_tagstruct_new(NULL, 0);
            pa_tagstruct_putu32(t, PA_COMMAND_REQUEST);
            pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
            pa_tagstruct_putu32(t, s->index);
            pa_tagstruct_putu32(t, l);
            pa_pstream_send_tagstruct(s->connection->pstream, t);

/*             pa_log("Requesting %u bytes", l);     */
            break;
        }

        case PLAYBACK_STREAM_MESSAGE_UNDERFLOW: {
            pa_tagstruct *t;

            /* Report that we're empty */
            t = pa_tagstruct_new(NULL, 0);
            pa_tagstruct_putu32(t, PA_COMMAND_UNDERFLOW);
            pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
            pa_tagstruct_putu32(t, s->index);
            pa_pstream_send_tagstruct(s->connection->pstream, t);
            break;
        }

        case PLAYBACK_STREAM_MESSAGE_OVERFLOW: {
            pa_tagstruct *t;

            /* Notify the user we're overflowed*/
            t = pa_tagstruct_new(NULL, 0);
            pa_tagstruct_putu32(t, PA_COMMAND_OVERFLOW);
            pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
            pa_tagstruct_putu32(t, s->index);
            pa_pstream_send_tagstruct(s->connection->pstream, t);
            break;
        }

        case PLAYBACK_STREAM_MESSAGE_DRAIN_ACK:
            pa_pstream_send_simple_ack(s->connection->pstream, PA_PTR_TO_UINT(userdata));
            break;

    }

    return 0;
}

static playback_stream* playback_stream_new(
        connection *c,
        pa_sink *sink,
        pa_sample_spec *ss,
        pa_channel_map *map,
        const char *name,
        uint32_t *maxlength,
        uint32_t *tlength,
        uint32_t *prebuf,
        uint32_t *minreq,
        pa_cvolume *volume,
        uint32_t syncid,
        uint32_t *missing,
        pa_sink_input_flags_t flags) {

    playback_stream *s, *ssync;
    pa_sink_input *sink_input;
    pa_memblock *silence;
    uint32_t idx;
    int64_t start_index;
    pa_sink_input_new_data data;

    pa_assert(c);
    pa_assert(ss);
    pa_assert(name);
    pa_assert(maxlength);

    /* Find syncid group */
    for (ssync = pa_idxset_first(c->output_streams, &idx); ssync; ssync = pa_idxset_next(c->output_streams, &idx)) {

        if (!playback_stream_isinstance(ssync))
            continue;

        if (ssync->syncid == syncid)
            break;
    }

    /* Synced streams must connect to the same sink */
    if (ssync) {

        if (!sink)
            sink = ssync->sink_input->sink;
        else if (sink != ssync->sink_input->sink)
            return NULL;
    }

    pa_sink_input_new_data_init(&data);
    data.sink = sink;
    data.driver = __FILE__;
    data.name = name;
    pa_sink_input_new_data_set_sample_spec(&data, ss);
    pa_sink_input_new_data_set_channel_map(&data, map);
    pa_sink_input_new_data_set_volume(&data, volume);
    data.module = c->protocol->module;
    data.client = c->client;
    data.sync_base = ssync ? ssync->sink_input : NULL;

    if (!(sink_input = pa_sink_input_new(c->protocol->core, &data, flags)))
        return NULL;

    s = pa_msgobject_new(playback_stream);
    s->parent.parent.parent.free = playback_stream_free;
    s->parent.parent.process_msg = playback_stream_process_msg;
    s->connection = c;
    s->syncid = syncid;
    s->sink_input = sink_input;
    s->underrun = 1;

    s->sink_input->parent.process_msg = sink_input_process_msg;
    s->sink_input->peek = sink_input_peek_cb;
    s->sink_input->drop = sink_input_drop_cb;
    s->sink_input->kill = sink_input_kill_cb;
    s->sink_input->moved = sink_input_moved_cb;
    s->sink_input->suspend = sink_input_suspend_cb;
    s->sink_input->userdata = s;

    start_index = ssync ? pa_memblockq_get_read_index(ssync->memblockq) : 0;

    silence = pa_silence_memblock_new(c->protocol->core->mempool, &s->sink_input->sample_spec, 0);

    s->memblockq = pa_memblockq_new(
            start_index,
            *maxlength,
            *tlength,
            pa_frame_size(&s->sink_input->sample_spec),
            *prebuf,
            *minreq,
            silence);

    pa_memblock_unref(silence);

    *maxlength = (uint32_t) pa_memblockq_get_maxlength(s->memblockq);
    *tlength = (uint32_t) pa_memblockq_get_tlength(s->memblockq);
    *prebuf = (uint32_t) pa_memblockq_get_prebuf(s->memblockq);
    *minreq = (uint32_t) pa_memblockq_get_minreq(s->memblockq);
    *missing = (uint32_t) pa_memblockq_pop_missing(s->memblockq);

    *ss = s->sink_input->sample_spec;
    *map = s->sink_input->channel_map;

    s->minreq = pa_memblockq_get_minreq(s->memblockq);
    pa_atomic_store(&s->missing, 0);
    s->drain_request = 0;

    pa_idxset_put(c->output_streams, s, &s->index);

    pa_sink_input_put(s->sink_input);

    return s;
}

static int connection_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    connection *c = CONNECTION(o);
    connection_assert_ref(c);

    if (!c->protocol)
        return -1;

    switch (code) {

        case CONNECTION_MESSAGE_REVOKE:
            pa_pstream_send_revoke(c->pstream, PA_PTR_TO_UINT(userdata));
            break;

        case CONNECTION_MESSAGE_RELEASE:
            pa_pstream_send_release(c->pstream, PA_PTR_TO_UINT(userdata));
            break;
    }

    return 0;
}

static void connection_unlink(connection *c) {
    record_stream *r;
    output_stream *o;

    pa_assert(c);

    if (!c->protocol)
        return;

    while ((r = pa_idxset_first(c->record_streams, NULL)))
        record_stream_unlink(r);

    while ((o = pa_idxset_first(c->output_streams, NULL)))
        if (playback_stream_isinstance(o))
            playback_stream_unlink(PLAYBACK_STREAM(o));
        else
            upload_stream_unlink(UPLOAD_STREAM(o));

    if (c->subscription)
        pa_subscription_free(c->subscription);

    if (c->pstream)
        pa_pstream_unlink(c->pstream);

    if (c->auth_timeout_event) {
        c->protocol->core->mainloop->time_free(c->auth_timeout_event);
        c->auth_timeout_event = NULL;
    }

    pa_assert_se(pa_idxset_remove_by_data(c->protocol->connections, c, NULL) == c);
    c->protocol = NULL;
    connection_unref(c);
}

static void connection_free(pa_object *o) {
    connection *c = CONNECTION(o);

    pa_assert(c);

    connection_unlink(c);

    pa_idxset_free(c->record_streams, NULL, NULL);
    pa_idxset_free(c->output_streams, NULL, NULL);

    pa_pdispatch_unref(c->pdispatch);
    pa_pstream_unref(c->pstream);
    pa_client_free(c->client);

    pa_xfree(c);
}

/* Called from thread context */
static void request_bytes(playback_stream *s) {
    size_t m, previous_missing;

    playback_stream_assert_ref(s);

    m = pa_memblockq_pop_missing(s->memblockq);

    if (m <= 0)
        return;

/*     pa_log("request_bytes(%u)", m); */

    previous_missing = pa_atomic_add(&s->missing, m);
    if (previous_missing < s->minreq && previous_missing+m >= s->minreq) {
        pa_assert(pa_thread_mq_get());
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(s), PLAYBACK_STREAM_MESSAGE_REQUEST_DATA, NULL, 0, NULL, NULL);
    }
}

static void send_memblock(connection *c) {
    uint32_t start;
    record_stream *r;

    start = PA_IDXSET_INVALID;
    for (;;) {
        pa_memchunk chunk;

        if (!(r = RECORD_STREAM(pa_idxset_rrobin(c->record_streams, &c->rrobin_index))))
            return;

        if (start == PA_IDXSET_INVALID)
            start = c->rrobin_index;
        else if (start == c->rrobin_index)
            return;

        if (pa_memblockq_peek(r->memblockq,  &chunk) >= 0) {
            pa_memchunk schunk = chunk;

            if (schunk.length > r->fragment_size)
                schunk.length = r->fragment_size;

            pa_pstream_send_memblock(c->pstream, r->index, 0, PA_SEEK_RELATIVE, &schunk);

            pa_memblockq_drop(r->memblockq, schunk.length);
            pa_memblock_unref(schunk.memblock);

            return;
        }
    }
}

static void send_playback_stream_killed(playback_stream *p) {
    pa_tagstruct *t;
    playback_stream_assert_ref(p);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_PLAYBACK_STREAM_KILLED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, p->index);
    pa_pstream_send_tagstruct(p->connection->pstream, t);
}

static void send_record_stream_killed(record_stream *r) {
    pa_tagstruct *t;
    record_stream_assert_ref(r);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_RECORD_STREAM_KILLED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, r->index);
    pa_pstream_send_tagstruct(r->connection->pstream, t);
}

/*** sink input callbacks ***/

/* Called from thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink_input *i = PA_SINK_INPUT(o);
    playback_stream *s;

    pa_sink_input_assert_ref(i);
    s = PLAYBACK_STREAM(i->userdata);
    playback_stream_assert_ref(s);

    switch (code) {

        case SINK_INPUT_MESSAGE_SEEK:
            pa_memblockq_seek(s->memblockq, offset, PA_PTR_TO_UINT(userdata));
            request_bytes(s);
            return 0;

        case SINK_INPUT_MESSAGE_POST_DATA: {
            pa_assert(chunk);

/*             pa_log("sink input post: %u", chunk->length); */

            if (pa_memblockq_push_align(s->memblockq, chunk) < 0) {

                pa_log_warn("Failed to push data into queue");
                pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(s), PLAYBACK_STREAM_MESSAGE_OVERFLOW, NULL, 0, NULL, NULL);
                pa_memblockq_seek(s->memblockq, chunk->length, PA_SEEK_RELATIVE);
            }

            request_bytes(s);

            s->underrun = 0;
            return 0;
        }

        case SINK_INPUT_MESSAGE_DRAIN: {

            pa_memblockq_prebuf_disable(s->memblockq);

            if (!pa_memblockq_is_readable(s->memblockq))
                pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(s), PLAYBACK_STREAM_MESSAGE_DRAIN_ACK, userdata, 0, NULL, NULL);
            else {
                s->drain_tag = PA_PTR_TO_UINT(userdata);
                s->drain_request = 1;
            }
            request_bytes(s);

            return 0;
        }

        case SINK_INPUT_MESSAGE_FLUSH:
        case SINK_INPUT_MESSAGE_PREBUF_FORCE:
        case SINK_INPUT_MESSAGE_TRIGGER: {

            pa_sink_input *isync;
            void (*func)(pa_memblockq *bq);

            switch  (code) {
                case SINK_INPUT_MESSAGE_FLUSH:
                    func = pa_memblockq_flush;
                    break;

                case SINK_INPUT_MESSAGE_PREBUF_FORCE:
                    func = pa_memblockq_prebuf_force;
                    break;

                case SINK_INPUT_MESSAGE_TRIGGER:
                    func = pa_memblockq_prebuf_disable;
                    break;

                default:
                    pa_assert_not_reached();
            }

            func(s->memblockq);
            s->underrun = 0;
            request_bytes(s);

            /* Do the same for all other members in the sync group */
            for (isync = i->sync_prev; isync; isync = isync->sync_prev) {
                playback_stream *ssync = PLAYBACK_STREAM(isync->userdata);
                func(ssync->memblockq);
                ssync->underrun = 0;
                request_bytes(ssync);
            }

            for (isync = i->sync_next; isync; isync = isync->sync_next) {
                playback_stream *ssync = PLAYBACK_STREAM(isync->userdata);
                func(ssync->memblockq);
                ssync->underrun = 0;
                request_bytes(ssync);
            }

            return 0;
        }

        case SINK_INPUT_MESSAGE_UPDATE_LATENCY:

            s->read_index = pa_memblockq_get_read_index(s->memblockq);
            s->write_index = pa_memblockq_get_write_index(s->memblockq);
            s->resampled_chunk_length = s->sink_input->thread_info.resampled_chunk.memblock ? s->sink_input->thread_info.resampled_chunk.length : 0;
            return 0;

        case PA_SINK_INPUT_MESSAGE_SET_STATE:

            pa_memblockq_prebuf_force(s->memblockq);
            request_bytes(s);
            break;

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            *r = pa_bytes_to_usec(pa_memblockq_get_length(s->memblockq), &i->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
        }
    }

    return pa_sink_input_process_msg(o, code, userdata, offset, chunk);
}

/* Called from thread context */
static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    playback_stream *s;

    pa_sink_input_assert_ref(i);
    s = PLAYBACK_STREAM(i->userdata);
    playback_stream_assert_ref(s);
    pa_assert(chunk);

    if (pa_memblockq_get_length(s->memblockq) <= 0 && !s->underrun) {
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(s), PLAYBACK_STREAM_MESSAGE_UNDERFLOW, NULL, 0, NULL, NULL);
        s->underrun = 1;
    }

    if (pa_memblockq_peek(s->memblockq, chunk) < 0) {
/*         pa_log("peek: failure");     */
        return -1;
    }

/*     pa_log("peek: %u", chunk->length); */

    request_bytes(s);

    return 0;
}

/* Called from thread context */
static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    playback_stream *s;

    pa_sink_input_assert_ref(i);
    s = PLAYBACK_STREAM(i->userdata);
    playback_stream_assert_ref(s);
    pa_assert(length > 0);

    pa_memblockq_drop(s->memblockq, length);

    if (s->drain_request && !pa_memblockq_is_readable(s->memblockq)) {
        s->drain_request = 0;
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(s), PLAYBACK_STREAM_MESSAGE_DRAIN_ACK, PA_UINT_TO_PTR(s->drain_tag), 0, NULL, NULL);
    }

    request_bytes(s);

/*     pa_log("after_drop: %u %u", pa_memblockq_get_length(s->memblockq), pa_memblockq_is_readable(s->memblockq)); */
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    playback_stream *s;

    pa_sink_input_assert_ref(i);
    s = PLAYBACK_STREAM(i->userdata);
    playback_stream_assert_ref(s);

    send_playback_stream_killed(s);
    playback_stream_unlink(s);
}

/* Called from main context */
static void sink_input_suspend_cb(pa_sink_input *i, pa_bool_t suspend) {
    playback_stream *s;
    pa_tagstruct *t;

    pa_sink_input_assert_ref(i);
    s = PLAYBACK_STREAM(i->userdata);
    playback_stream_assert_ref(s);

    if (s->connection->version < 12)
      return;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_PLAYBACK_STREAM_SUSPENDED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_put_boolean(t, suspend);
    pa_pstream_send_tagstruct(s->connection->pstream, t);
}

/* Called from main context */
static void sink_input_moved_cb(pa_sink_input *i) {
    playback_stream *s;
    pa_tagstruct *t;

    pa_sink_input_assert_ref(i);
    s = PLAYBACK_STREAM(i->userdata);
    playback_stream_assert_ref(s);

    if (s->connection->version < 12)
      return;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_PLAYBACK_STREAM_MOVED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_putu32(t, i->sink->index);
    pa_tagstruct_puts(t, i->sink->name);
    pa_tagstruct_put_boolean(t, pa_sink_get_state(i->sink) == PA_SINK_SUSPENDED);
    pa_pstream_send_tagstruct(s->connection->pstream, t);
}

/*** source_output callbacks ***/

/* Called from thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    record_stream *s;

    pa_source_output_assert_ref(o);
    s = RECORD_STREAM(o->userdata);
    record_stream_assert_ref(s);
    pa_assert(chunk);

    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(s), RECORD_STREAM_MESSAGE_POST_DATA, NULL, 0, chunk, NULL);
}

static void source_output_kill_cb(pa_source_output *o) {
    record_stream *s;

    pa_source_output_assert_ref(o);
    s = RECORD_STREAM(o->userdata);
    record_stream_assert_ref(s);

    send_record_stream_killed(s);
    record_stream_unlink(s);
}

static pa_usec_t source_output_get_latency_cb(pa_source_output *o) {
    record_stream *s;

    pa_source_output_assert_ref(o);
    s = RECORD_STREAM(o->userdata);
    record_stream_assert_ref(s);

    /*pa_log("get_latency: %u", pa_memblockq_get_length(s->memblockq));*/

    return pa_bytes_to_usec(pa_memblockq_get_length(s->memblockq), &o->sample_spec);
}

/* Called from main context */
static void source_output_suspend_cb(pa_source_output *o, pa_bool_t suspend) {
    record_stream *s;
    pa_tagstruct *t;

    pa_source_output_assert_ref(o);
    s = RECORD_STREAM(o->userdata);
    record_stream_assert_ref(s);

    if (s->connection->version < 12)
      return;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_RECORD_STREAM_SUSPENDED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_put_boolean(t, suspend);
    pa_pstream_send_tagstruct(s->connection->pstream, t);
}

/* Called from main context */
static void source_output_moved_cb(pa_source_output *o) {
    record_stream *s;
    pa_tagstruct *t;

    pa_source_output_assert_ref(o);
    s = RECORD_STREAM(o->userdata);
    record_stream_assert_ref(s);

    if (s->connection->version < 12)
      return;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_RECORD_STREAM_MOVED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_putu32(t, o->source->index);
    pa_tagstruct_puts(t, o->source->name);
    pa_tagstruct_put_boolean(t, pa_source_get_state(o->source) == PA_SOURCE_SUSPENDED);
    pa_pstream_send_tagstruct(s->connection->pstream, t);
}

/*** pdispatch callbacks ***/

static void protocol_error(connection *c) {
    pa_log("protocol error, kicking client");
    connection_unlink(c);
}

#define CHECK_VALIDITY(pstream, expression, tag, error) do { \
if (!(expression)) { \
    pa_pstream_send_error((pstream), (tag), (error)); \
    return; \
} \
} while(0);

static pa_tagstruct *reply_new(uint32_t tag) {
    pa_tagstruct *reply;

    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    return reply;
}

static void command_create_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    playback_stream *s;
    uint32_t maxlength, tlength, prebuf, minreq, sink_index, syncid, missing;
    const char *name, *sink_name;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_tagstruct *reply;
    pa_sink *sink = NULL;
    pa_cvolume volume;
    int corked;
    int no_remap = 0, no_remix = 0, fix_format = 0, fix_rate = 0, fix_channels = 0, no_move = 0, variable_rate = 0;
    pa_sink_input_flags_t flags = 0;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_get(
            t,
            PA_TAG_STRING, &name,
            PA_TAG_SAMPLE_SPEC, &ss,
            PA_TAG_CHANNEL_MAP, &map,
            PA_TAG_U32, &sink_index,
            PA_TAG_STRING, &sink_name,
            PA_TAG_U32, &maxlength,
            PA_TAG_BOOLEAN, &corked,
            PA_TAG_U32, &tlength,
            PA_TAG_U32, &prebuf,
            PA_TAG_U32, &minreq,
            PA_TAG_U32, &syncid,
            PA_TAG_CVOLUME, &volume,
            PA_TAG_INVALID) < 0 || !name) {
        protocol_error(c);
        return;
    }

    if (c->version >= 12)  {
        /* Since 0.9.8 the user can ask for a couple of additional flags */

        if (pa_tagstruct_get_boolean(t, &no_remap) < 0 ||
            pa_tagstruct_get_boolean(t, &no_remix) < 0 ||
            pa_tagstruct_get_boolean(t, &fix_format) < 0 ||
            pa_tagstruct_get_boolean(t, &fix_rate) < 0 ||
            pa_tagstruct_get_boolean(t, &fix_channels) < 0 ||
            pa_tagstruct_get_boolean(t, &no_move) < 0 ||
            pa_tagstruct_get_boolean(t, &variable_rate) < 0) {
            protocol_error(c);
            return;
        }
    }

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name && pa_utf8_valid(name), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, sink_index != PA_INVALID_INDEX || !sink_name || (*sink_name && pa_utf8_valid(name)), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, pa_channel_map_valid(&map), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, pa_sample_spec_valid(&ss), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, pa_cvolume_valid(&volume), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, map.channels == ss.channels && volume.channels == ss.channels, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, maxlength > 0, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, maxlength <= MAX_MEMBLOCKQ_LENGTH, tag, PA_ERR_INVALID);

    if (sink_index != PA_INVALID_INDEX) {
        sink = pa_idxset_get_by_index(c->protocol->core->sinks, sink_index);
        CHECK_VALIDITY(c->pstream, sink, tag, PA_ERR_NOENTITY);
    } else if (sink_name) {
        sink = pa_namereg_get(c->protocol->core, sink_name, PA_NAMEREG_SINK, 1);
        CHECK_VALIDITY(c->pstream, sink, tag, PA_ERR_NOENTITY);
    }

    flags =
        (corked ?  PA_SINK_INPUT_START_CORKED : 0) |
        (no_remap ?  PA_SINK_INPUT_NO_REMAP : 0) |
        (no_remix ?  PA_SINK_INPUT_NO_REMIX : 0) |
        (fix_format ?  PA_SINK_INPUT_FIX_FORMAT : 0) |
        (fix_rate ?  PA_SINK_INPUT_FIX_RATE : 0) |
        (fix_channels ?  PA_SINK_INPUT_FIX_CHANNELS : 0) |
        (no_move ?  PA_SINK_INPUT_DONT_MOVE : 0) |
        (variable_rate ?  PA_SINK_INPUT_VARIABLE_RATE : 0);

    s = playback_stream_new(c, sink, &ss, &map, name, &maxlength, &tlength, &prebuf, &minreq, &volume, syncid, &missing, flags);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_INVALID);

    reply = reply_new(tag);
    pa_tagstruct_putu32(reply, s->index);
    pa_assert(s->sink_input);
    pa_tagstruct_putu32(reply, s->sink_input->index);
    pa_tagstruct_putu32(reply, missing);

/*     pa_log("initial request is %u", missing); */

    if (c->version >= 9) {
        /* Since 0.9.0 we support sending the buffer metrics back to the client */

        pa_tagstruct_putu32(reply, (uint32_t) maxlength);
        pa_tagstruct_putu32(reply, (uint32_t) tlength);
        pa_tagstruct_putu32(reply, (uint32_t) prebuf);
        pa_tagstruct_putu32(reply, (uint32_t) minreq);
    }

    if (c->version >= 12) {
        /* Since 0.9.8 we support sending the chosen sample
         * spec/channel map/device/suspend status back to the
         * client */

        pa_tagstruct_put_sample_spec(reply, &ss);
        pa_tagstruct_put_channel_map(reply, &map);

        pa_tagstruct_putu32(reply, s->sink_input->sink->index);
        pa_tagstruct_puts(reply, s->sink_input->sink->name);

        pa_tagstruct_put_boolean(reply, pa_sink_get_state(s->sink_input->sink) == PA_SINK_SUSPENDED);
    }

    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_delete_stream(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t channel;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    switch (command) {

        case PA_COMMAND_DELETE_PLAYBACK_STREAM: {
            playback_stream *s;
            if (!(s = pa_idxset_get_by_index(c->output_streams, channel)) || !playback_stream_isinstance(s)) {
                pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
                return;
            }

            playback_stream_unlink(s);
            break;
        }

        case PA_COMMAND_DELETE_RECORD_STREAM: {
            record_stream *s;
            if (!(s = pa_idxset_get_by_index(c->record_streams, channel))) {
                pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
                return;
            }

            record_stream_unlink(s);
            break;
        }

        case PA_COMMAND_DELETE_UPLOAD_STREAM: {
            upload_stream *s;

            if (!(s = pa_idxset_get_by_index(c->output_streams, channel)) || !upload_stream_isinstance(s)) {
                pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
                return;
            }

            upload_stream_unlink(s);
            break;
        }

        default:
            pa_assert_not_reached();
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_create_record_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    record_stream *s;
    uint32_t maxlength, fragment_size;
    uint32_t source_index;
    const char *name, *source_name;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_tagstruct *reply;
    pa_source *source = NULL;
    int corked;
    int no_remap = 0, no_remix = 0, fix_format = 0, fix_rate = 0, fix_channels = 0, no_move = 0, variable_rate = 0;
    pa_source_output_flags_t flags = 0;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
        pa_tagstruct_get_channel_map(t, &map) < 0 ||
        pa_tagstruct_getu32(t, &source_index) < 0 ||
        pa_tagstruct_gets(t, &source_name) < 0 ||
        pa_tagstruct_getu32(t, &maxlength) < 0 ||
        pa_tagstruct_get_boolean(t, &corked) < 0 ||
        pa_tagstruct_getu32(t, &fragment_size) < 0) {
        protocol_error(c);
        return;
    }

    if (c->version >= 12)  {
        /* Since 0.9.8 the user can ask for a couple of additional flags */

        if (pa_tagstruct_get_boolean(t, &no_remap) < 0 ||
            pa_tagstruct_get_boolean(t, &no_remix) < 0 ||
            pa_tagstruct_get_boolean(t, &fix_format) < 0 ||
            pa_tagstruct_get_boolean(t, &fix_rate) < 0 ||
            pa_tagstruct_get_boolean(t, &fix_channels) < 0 ||
            pa_tagstruct_get_boolean(t, &no_move) < 0 ||
            pa_tagstruct_get_boolean(t, &variable_rate) < 0) {
            protocol_error(c);
            return;
        }
    }

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    flags =
        (corked ?  PA_SOURCE_OUTPUT_START_CORKED : 0) |
        (no_remap ?  PA_SOURCE_OUTPUT_NO_REMAP : 0) |
        (no_remix ?  PA_SOURCE_OUTPUT_NO_REMIX : 0) |
        (fix_format ?  PA_SOURCE_OUTPUT_FIX_FORMAT : 0) |
        (fix_rate ?  PA_SOURCE_OUTPUT_FIX_RATE : 0) |
        (fix_channels ?  PA_SOURCE_OUTPUT_FIX_CHANNELS : 0) |
        (no_move ?  PA_SOURCE_OUTPUT_DONT_MOVE : 0) |
        (variable_rate ?  PA_SOURCE_OUTPUT_VARIABLE_RATE : 0);

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name && pa_utf8_valid(name), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, pa_sample_spec_valid(&ss), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, pa_channel_map_valid(&map), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, source_index != PA_INVALID_INDEX || !source_name || (*source_name && pa_utf8_valid(source_name)), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, map.channels == ss.channels, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, maxlength > 0, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, maxlength <= MAX_MEMBLOCKQ_LENGTH, tag, PA_ERR_INVALID);

    if (source_index != PA_INVALID_INDEX) {
        source = pa_idxset_get_by_index(c->protocol->core->sources, source_index);
        CHECK_VALIDITY(c->pstream, source, tag, PA_ERR_NOENTITY);
    } else if (source_name) {
        source = pa_namereg_get(c->protocol->core, source_name, PA_NAMEREG_SOURCE, 1);
        CHECK_VALIDITY(c->pstream, source, tag, PA_ERR_NOENTITY);
    }

    s = record_stream_new(c, source, &ss, &map, name, &maxlength, fragment_size, flags);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_INVALID);

    reply = reply_new(tag);
    pa_tagstruct_putu32(reply, s->index);
    pa_assert(s->source_output);
    pa_tagstruct_putu32(reply, s->source_output->index);

    if (c->version >= 9) {
        /* Since 0.9 we support sending the buffer metrics back to the client */

        pa_tagstruct_putu32(reply, (uint32_t) maxlength);
        pa_tagstruct_putu32(reply, (uint32_t) s->fragment_size);
    }

    if (c->version >= 12) {
        /* Since 0.9.8 we support sending the chosen sample
         * spec/channel map/device/suspend status back to the
         * client */

        pa_tagstruct_put_sample_spec(reply, &ss);
        pa_tagstruct_put_channel_map(reply, &map);

        pa_tagstruct_putu32(reply, s->source_output->source->index);
        pa_tagstruct_puts(reply, s->source_output->source->name);

        pa_tagstruct_put_boolean(reply, pa_source_get_state(s->source_output->source) == PA_SOURCE_SUSPENDED);
    }

    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_exit(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);

    connection_assert_ref(c);
    pa_assert(t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    c->protocol->core->mainloop->quit(c->protocol->core->mainloop, 0);
    pa_pstream_send_simple_ack(c->pstream, tag); /* nonsense */
}

static void command_auth(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const void*cookie;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &c->version) < 0 ||
        pa_tagstruct_get_arbitrary(t, &cookie, PA_NATIVE_COOKIE_LENGTH) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    /* Minimum supported version */
    if (c->version < 8) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_VERSION);
        return;
    }

    if (!c->authorized) {
        int success = 0;

#ifdef HAVE_CREDS
        const pa_creds *creds;

        if ((creds = pa_pdispatch_creds(pd))) {
            if (creds->uid == getuid())
                success = 1;
            else if (c->protocol->auth_group) {
                int r;
                gid_t gid;

                if ((gid = pa_get_gid_of_group(c->protocol->auth_group)) == (gid_t) -1)
                    pa_log_warn("failed to get GID of group '%s'", c->protocol->auth_group);
                else if (gid == creds->gid)
                    success = 1;

                if (!success) {
                    if ((r = pa_uid_in_group(creds->uid, c->protocol->auth_group)) < 0)
                        pa_log_warn("failed to check group membership.");
                    else if (r > 0)
                        success = 1;
                }
            }

            pa_log_info("Got credentials: uid=%lu gid=%lu success=%i",
                        (unsigned long) creds->uid,
                        (unsigned long) creds->gid,
                        success);

            if (c->version >= 10 &&
                pa_mempool_is_shared(c->protocol->core->mempool) &&
                creds->uid == getuid()) {

                pa_pstream_use_shm(c->pstream, 1);
                pa_log_info("Enabled SHM for new connection");
            }

        }
#endif

        if (!success && memcmp(c->protocol->auth_cookie, cookie, PA_NATIVE_COOKIE_LENGTH) == 0)
            success = 1;

        if (!success) {
            pa_log_warn("Denied access to client with invalid authorization data.");
            pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
            return;
        }

        c->authorized = 1;
        if (c->auth_timeout_event) {
            c->protocol->core->mainloop->time_free(c->auth_timeout_event);
            c->auth_timeout_event = NULL;
        }
    }

    reply = reply_new(tag);
    pa_tagstruct_putu32(reply, PA_PROTOCOL_VERSION);

#ifdef HAVE_CREDS
{
    /* SHM support is only enabled after both sides made sure they are the same user. */

    pa_creds ucred;

    ucred.uid = getuid();
    ucred.gid = getgid();

    pa_pstream_send_tagstruct_with_creds(c->pstream, reply, &ucred);
}
#else
    pa_pstream_send_tagstruct(c->pstream, reply);
#endif
}

static void command_set_client_name(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const char *name;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, name && pa_utf8_valid(name), tag, PA_ERR_INVALID);

    pa_client_set_name(c->client, name);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_lookup(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const char *name;
    uint32_t idx = PA_IDXSET_INVALID;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name && *name && pa_utf8_valid(name), tag, PA_ERR_INVALID);

    if (command == PA_COMMAND_LOOKUP_SINK) {
        pa_sink *sink;
        if ((sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1)))
            idx = sink->index;
    } else {
        pa_source *source;
        pa_assert(command == PA_COMMAND_LOOKUP_SOURCE);
        if ((source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1)))
            idx = source->index;
    }

    if (idx == PA_IDXSET_INVALID)
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
    else {
        pa_tagstruct *reply;
        reply = reply_new(tag);
        pa_tagstruct_putu32(reply, idx);
        pa_pstream_send_tagstruct(c->pstream, reply);
    }
}

static void command_drain_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    playback_stream *s;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    s = pa_idxset_get_by_index(c->output_streams, idx);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
    CHECK_VALIDITY(c->pstream, playback_stream_isinstance(s), tag, PA_ERR_NOENTITY);

    pa_asyncmsgq_post(s->sink_input->sink->asyncmsgq, PA_MSGOBJECT(s->sink_input), SINK_INPUT_MESSAGE_DRAIN, PA_UINT_TO_PTR(tag), 0, NULL, NULL);
}

static void command_stat(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_tagstruct *reply;
    const pa_mempool_stat *stat;

    connection_assert_ref(c);
    pa_assert(t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    stat = pa_mempool_get_stat(c->protocol->core->mempool);

    reply = reply_new(tag);
    pa_tagstruct_putu32(reply, (uint32_t) pa_atomic_load(&stat->n_allocated));
    pa_tagstruct_putu32(reply, (uint32_t) pa_atomic_load(&stat->allocated_size));
    pa_tagstruct_putu32(reply, (uint32_t) pa_atomic_load(&stat->n_accumulated));
    pa_tagstruct_putu32(reply, (uint32_t) pa_atomic_load(&stat->accumulated_size));
    pa_tagstruct_putu32(reply, pa_scache_total_size(c->protocol->core));
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_playback_latency(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_tagstruct *reply;
    playback_stream *s;
    struct timeval tv, now;
    uint32_t idx;
    pa_usec_t latency;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_timeval(t, &tv) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    s = pa_idxset_get_by_index(c->output_streams, idx);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
    CHECK_VALIDITY(c->pstream, playback_stream_isinstance(s), tag, PA_ERR_NOENTITY);
    CHECK_VALIDITY(c->pstream, pa_asyncmsgq_send(s->sink_input->sink->asyncmsgq, PA_MSGOBJECT(s->sink_input), SINK_INPUT_MESSAGE_UPDATE_LATENCY, s, 0, NULL) == 0, tag, PA_ERR_NOENTITY)

    reply = reply_new(tag);

    latency = pa_sink_get_latency(s->sink_input->sink);
    latency += pa_bytes_to_usec(s->resampled_chunk_length, &s->sink_input->sample_spec);

    pa_tagstruct_put_usec(reply, latency);

    pa_tagstruct_put_usec(reply, 0);
    pa_tagstruct_put_boolean(reply, pa_sink_input_get_state(s->sink_input) == PA_SINK_INPUT_RUNNING);
    pa_tagstruct_put_timeval(reply, &tv);
    pa_tagstruct_put_timeval(reply, pa_gettimeofday(&now));
    pa_tagstruct_puts64(reply, s->write_index);
    pa_tagstruct_puts64(reply, s->read_index);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_record_latency(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_tagstruct *reply;
    record_stream *s;
    struct timeval tv, now;
    uint32_t idx;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_timeval(t, &tv) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    s = pa_idxset_get_by_index(c->record_streams, idx);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

    reply = reply_new(tag);
    pa_tagstruct_put_usec(reply, s->source_output->source->monitor_of ? pa_sink_get_latency(s->source_output->source->monitor_of) : 0);
    pa_tagstruct_put_usec(reply, pa_source_get_latency(s->source_output->source));
    pa_tagstruct_put_boolean(reply, 0);
    pa_tagstruct_put_timeval(reply, &tv);
    pa_tagstruct_put_timeval(reply, pa_gettimeofday(&now));
    pa_tagstruct_puts64(reply, pa_memblockq_get_write_index(s->memblockq));
    pa_tagstruct_puts64(reply, pa_memblockq_get_read_index(s->memblockq));
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_create_upload_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    upload_stream *s;
    uint32_t length;
    const char *name;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
        pa_tagstruct_get_channel_map(t, &map) < 0 ||
        pa_tagstruct_getu32(t, &length) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, pa_sample_spec_valid(&ss), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, pa_channel_map_valid(&map), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, map.channels == ss.channels, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, (length % pa_frame_size(&ss)) == 0 && length > 0, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, length <= PA_SCACHE_ENTRY_SIZE_MAX, tag, PA_ERR_TOOLARGE);
    CHECK_VALIDITY(c->pstream, name && *name && pa_utf8_valid(name), tag, PA_ERR_INVALID);

    s = upload_stream_new(c, &ss, &map, name, length);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_INVALID);

    reply = reply_new(tag);
    pa_tagstruct_putu32(reply, s->index);
    pa_tagstruct_putu32(reply, length);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_finish_upload_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t channel;
    upload_stream *s;
    uint32_t idx;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    s = pa_idxset_get_by_index(c->output_streams, channel);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
    CHECK_VALIDITY(c->pstream, upload_stream_isinstance(s), tag, PA_ERR_NOENTITY);

    if (pa_scache_add_item(c->protocol->core, s->name, &s->sample_spec, &s->channel_map, &s->memchunk, &idx) < 0)
        pa_pstream_send_error(c->pstream, tag, PA_ERR_INTERNAL);
    else
        pa_pstream_send_simple_ack(c->pstream, tag);

    upload_stream_unlink(s);
}

static void command_play_sample(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t sink_index;
    pa_volume_t volume;
    pa_sink *sink;
    const char *name, *sink_name;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &sink_index) < 0 ||
        pa_tagstruct_gets(t, &sink_name) < 0 ||
        pa_tagstruct_getu32(t, &volume) < 0 ||
        pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, sink_index != PA_INVALID_INDEX || !sink_name || (*sink_name && pa_utf8_valid(name)), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, name && *name && pa_utf8_valid(name), tag, PA_ERR_INVALID);

    if (sink_index != PA_INVALID_INDEX)
        sink = pa_idxset_get_by_index(c->protocol->core->sinks, sink_index);
    else
        sink = pa_namereg_get(c->protocol->core, sink_name, PA_NAMEREG_SINK, 1);

    CHECK_VALIDITY(c->pstream, sink, tag, PA_ERR_NOENTITY);

    if (pa_scache_play_item(c->protocol->core, name, sink, volume) < 0) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_remove_sample(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const char *name;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name && *name && pa_utf8_valid(name), tag, PA_ERR_INVALID);

    if (pa_scache_remove_item(c->protocol->core, name) < 0) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void fixup_sample_spec(connection *c, pa_sample_spec *fixed, const pa_sample_spec *original) {
    pa_assert(c);
    pa_assert(fixed);
    pa_assert(original);

    *fixed = *original;

    if (c->version < 12) {
        /* Before protocol version 12 we didn't support S32 samples,
         * so we need to lie about this to the client */

        if (fixed->format == PA_SAMPLE_S32LE)
            fixed->format = PA_SAMPLE_FLOAT32LE;
        if (fixed->format == PA_SAMPLE_S32BE)
            fixed->format = PA_SAMPLE_FLOAT32BE;
    }
}

static void sink_fill_tagstruct(connection *c, pa_tagstruct *t, pa_sink *sink) {
    pa_sample_spec fixed_ss;

    pa_assert(t);
    pa_sink_assert_ref(sink);

    fixup_sample_spec(c, &fixed_ss, &sink->sample_spec);

    pa_tagstruct_put(
        t,
        PA_TAG_U32, sink->index,
        PA_TAG_STRING, sink->name,
        PA_TAG_STRING, sink->description,
        PA_TAG_SAMPLE_SPEC, &fixed_ss,
        PA_TAG_CHANNEL_MAP, &sink->channel_map,
        PA_TAG_U32, sink->module ? sink->module->index : PA_INVALID_INDEX,
        PA_TAG_CVOLUME, pa_sink_get_volume(sink),
        PA_TAG_BOOLEAN, pa_sink_get_mute(sink),
        PA_TAG_U32, sink->monitor_source ? sink->monitor_source->index : PA_INVALID_INDEX,
        PA_TAG_STRING, sink->monitor_source ? sink->monitor_source->name : NULL,
        PA_TAG_USEC, pa_sink_get_latency(sink),
        PA_TAG_STRING, sink->driver,
        PA_TAG_U32, sink->flags,
        PA_TAG_INVALID);
}

static void source_fill_tagstruct(connection *c, pa_tagstruct *t, pa_source *source) {
    pa_sample_spec fixed_ss;

    pa_assert(t);
    pa_source_assert_ref(source);

    fixup_sample_spec(c, &fixed_ss, &source->sample_spec);

    pa_tagstruct_put(
        t,
        PA_TAG_U32, source->index,
        PA_TAG_STRING, source->name,
        PA_TAG_STRING, source->description,
        PA_TAG_SAMPLE_SPEC, &fixed_ss,
        PA_TAG_CHANNEL_MAP, &source->channel_map,
        PA_TAG_U32, source->module ? source->module->index : PA_INVALID_INDEX,
        PA_TAG_CVOLUME, pa_source_get_volume(source),
        PA_TAG_BOOLEAN, pa_source_get_mute(source),
        PA_TAG_U32, source->monitor_of ? source->monitor_of->index : PA_INVALID_INDEX,
        PA_TAG_STRING, source->monitor_of ? source->monitor_of->name : NULL,
        PA_TAG_USEC, pa_source_get_latency(source),
        PA_TAG_STRING, source->driver,
        PA_TAG_U32, source->flags,
        PA_TAG_INVALID);
}

static void client_fill_tagstruct(pa_tagstruct *t, pa_client *client) {
    pa_assert(t);
    pa_assert(client);

    pa_tagstruct_putu32(t, client->index);
    pa_tagstruct_puts(t, client->name);
    pa_tagstruct_putu32(t, client->owner ? client->owner->index : PA_INVALID_INDEX);
    pa_tagstruct_puts(t, client->driver);
}

static void module_fill_tagstruct(pa_tagstruct *t, pa_module *module) {
    pa_assert(t);
    pa_assert(module);

    pa_tagstruct_putu32(t, module->index);
    pa_tagstruct_puts(t, module->name);
    pa_tagstruct_puts(t, module->argument);
    pa_tagstruct_putu32(t, module->n_used);
    pa_tagstruct_put_boolean(t, module->auto_unload);
}

static void sink_input_fill_tagstruct(connection *c, pa_tagstruct *t, pa_sink_input *s) {
    pa_sample_spec fixed_ss;

    pa_assert(t);
    pa_sink_input_assert_ref(s);

    fixup_sample_spec(c, &fixed_ss, &s->sample_spec);

    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_putu32(t, s->module ? s->module->index : PA_INVALID_INDEX);
    pa_tagstruct_putu32(t, s->client ? s->client->index : PA_INVALID_INDEX);
    pa_tagstruct_putu32(t, s->sink->index);
    pa_tagstruct_put_sample_spec(t, &fixed_ss);
    pa_tagstruct_put_channel_map(t, &s->channel_map);
    pa_tagstruct_put_cvolume(t, &s->volume);
    pa_tagstruct_put_usec(t, pa_sink_input_get_latency(s));
    pa_tagstruct_put_usec(t, pa_sink_get_latency(s->sink));
    pa_tagstruct_puts(t, pa_resample_method_to_string(pa_sink_input_get_resample_method(s)));
    pa_tagstruct_puts(t, s->driver);
    if (c->version >= 11)
        pa_tagstruct_put_boolean(t, pa_sink_input_get_mute(s));
}

static void source_output_fill_tagstruct(connection *c, pa_tagstruct *t, pa_source_output *s) {
    pa_sample_spec fixed_ss;

    pa_assert(t);
    pa_source_output_assert_ref(s);

    fixup_sample_spec(c, &fixed_ss, &s->sample_spec);

    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_putu32(t, s->module ? s->module->index : PA_INVALID_INDEX);
    pa_tagstruct_putu32(t, s->client ? s->client->index : PA_INVALID_INDEX);
    pa_tagstruct_putu32(t, s->source->index);
    pa_tagstruct_put_sample_spec(t, &fixed_ss);
    pa_tagstruct_put_channel_map(t, &s->channel_map);
    pa_tagstruct_put_usec(t, pa_source_output_get_latency(s));
    pa_tagstruct_put_usec(t, pa_source_get_latency(s->source));
    pa_tagstruct_puts(t, pa_resample_method_to_string(pa_source_output_get_resample_method(s)));
    pa_tagstruct_puts(t, s->driver);
}

static void scache_fill_tagstruct(connection *c, pa_tagstruct *t, pa_scache_entry *e) {
    pa_sample_spec fixed_ss;

    pa_assert(t);
    pa_assert(e);

    fixup_sample_spec(c, &fixed_ss, &e->sample_spec);

    pa_tagstruct_putu32(t, e->index);
    pa_tagstruct_puts(t, e->name);
    pa_tagstruct_put_cvolume(t, &e->volume);
    pa_tagstruct_put_usec(t, pa_bytes_to_usec(e->memchunk.length, &e->sample_spec));
    pa_tagstruct_put_sample_spec(t, &fixed_ss);
    pa_tagstruct_put_channel_map(t, &e->channel_map);
    pa_tagstruct_putu32(t, e->memchunk.length);
    pa_tagstruct_put_boolean(t, e->lazy);
    pa_tagstruct_puts(t, e->filename);
}

static void command_get_info(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    pa_sink *sink = NULL;
    pa_source *source = NULL;
    pa_client *client = NULL;
    pa_module *module = NULL;
    pa_sink_input *si = NULL;
    pa_source_output *so = NULL;
    pa_scache_entry *sce = NULL;
    const char *name;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        (command != PA_COMMAND_GET_CLIENT_INFO &&
         command != PA_COMMAND_GET_MODULE_INFO &&
         command != PA_COMMAND_GET_SINK_INPUT_INFO &&
         command != PA_COMMAND_GET_SOURCE_OUTPUT_INFO &&
         pa_tagstruct_gets(t, &name) < 0) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, idx != PA_INVALID_INDEX || !name || (*name && pa_utf8_valid(name)), tag, PA_ERR_INVALID);

    if (command == PA_COMMAND_GET_SINK_INFO) {
        if (idx != PA_INVALID_INDEX)
            sink = pa_idxset_get_by_index(c->protocol->core->sinks, idx);
        else
            sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1);
    } else if (command == PA_COMMAND_GET_SOURCE_INFO) {
        if (idx != PA_INVALID_INDEX)
            source = pa_idxset_get_by_index(c->protocol->core->sources, idx);
        else
            source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1);
    } else if (command == PA_COMMAND_GET_CLIENT_INFO)
        client = pa_idxset_get_by_index(c->protocol->core->clients, idx);
    else if (command == PA_COMMAND_GET_MODULE_INFO)
        module = pa_idxset_get_by_index(c->protocol->core->modules, idx);
    else if (command == PA_COMMAND_GET_SINK_INPUT_INFO)
        si = pa_idxset_get_by_index(c->protocol->core->sink_inputs, idx);
    else if (command == PA_COMMAND_GET_SOURCE_OUTPUT_INFO)
        so = pa_idxset_get_by_index(c->protocol->core->source_outputs, idx);
    else {
        pa_assert(command == PA_COMMAND_GET_SAMPLE_INFO);
        if (idx != PA_INVALID_INDEX)
            sce = pa_idxset_get_by_index(c->protocol->core->scache, idx);
        else
            sce = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SAMPLE, 0);
    }

    if (!sink && !source && !client && !module && !si && !so && !sce) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    reply = reply_new(tag);
    if (sink)
        sink_fill_tagstruct(c, reply, sink);
    else if (source)
        source_fill_tagstruct(c, reply, source);
    else if (client)
        client_fill_tagstruct(reply, client);
    else if (module)
        module_fill_tagstruct(reply, module);
    else if (si)
        sink_input_fill_tagstruct(c, reply, si);
    else if (so)
        source_output_fill_tagstruct(c, reply, so);
    else
        scache_fill_tagstruct(c, reply, sce);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_info_list(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_idxset *i;
    uint32_t idx;
    void *p;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    reply = reply_new(tag);

    if (command == PA_COMMAND_GET_SINK_INFO_LIST)
        i = c->protocol->core->sinks;
    else if (command == PA_COMMAND_GET_SOURCE_INFO_LIST)
        i = c->protocol->core->sources;
    else if (command == PA_COMMAND_GET_CLIENT_INFO_LIST)
        i = c->protocol->core->clients;
    else if (command == PA_COMMAND_GET_MODULE_INFO_LIST)
        i = c->protocol->core->modules;
    else if (command == PA_COMMAND_GET_SINK_INPUT_INFO_LIST)
        i = c->protocol->core->sink_inputs;
    else if (command == PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST)
        i = c->protocol->core->source_outputs;
    else {
        pa_assert(command == PA_COMMAND_GET_SAMPLE_INFO_LIST);
        i = c->protocol->core->scache;
    }

    if (i) {
        for (p = pa_idxset_first(i, &idx); p; p = pa_idxset_next(i, &idx)) {
            if (command == PA_COMMAND_GET_SINK_INFO_LIST)
                sink_fill_tagstruct(c, reply, p);
            else if (command == PA_COMMAND_GET_SOURCE_INFO_LIST)
                source_fill_tagstruct(c, reply, p);
            else if (command == PA_COMMAND_GET_CLIENT_INFO_LIST)
                client_fill_tagstruct(reply, p);
            else if (command == PA_COMMAND_GET_MODULE_INFO_LIST)
                module_fill_tagstruct(reply, p);
            else if (command == PA_COMMAND_GET_SINK_INPUT_INFO_LIST)
                sink_input_fill_tagstruct(c, reply, p);
            else if (command == PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST)
                source_output_fill_tagstruct(c, reply, p);
            else {
                pa_assert(command == PA_COMMAND_GET_SAMPLE_INFO_LIST);
                scache_fill_tagstruct(c, reply, p);
            }
        }
    }

    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_server_info(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_tagstruct *reply;
    char txt[256];
    const char *n;
    pa_sample_spec fixed_ss;

    connection_assert_ref(c);
    pa_assert(t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    reply = reply_new(tag);
    pa_tagstruct_puts(reply, PACKAGE_NAME);
    pa_tagstruct_puts(reply, PACKAGE_VERSION);
    pa_tagstruct_puts(reply, pa_get_user_name(txt, sizeof(txt)));
    pa_tagstruct_puts(reply, pa_get_fqdn(txt, sizeof(txt)));

    fixup_sample_spec(c, &fixed_ss, &c->protocol->core->default_sample_spec);
    pa_tagstruct_put_sample_spec(reply, &fixed_ss);

    n = pa_namereg_get_default_sink_name(c->protocol->core);
    pa_tagstruct_puts(reply, n);
    n = pa_namereg_get_default_source_name(c->protocol->core);
    pa_tagstruct_puts(reply, n);

    pa_tagstruct_putu32(reply, c->protocol->core->cookie);

    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void subscription_cb(pa_core *core, pa_subscription_event_type_t e, uint32_t idx, void *userdata) {
    pa_tagstruct *t;
    connection *c = CONNECTION(userdata);

    connection_assert_ref(c);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SUBSCRIBE_EVENT);
    pa_tagstruct_putu32(t, (uint32_t) -1);
    pa_tagstruct_putu32(t, e);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
}

static void command_subscribe(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_subscription_mask_t m;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &m) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, (m & ~PA_SUBSCRIPTION_MASK_ALL) == 0, tag, PA_ERR_INVALID);

    if (c->subscription)
        pa_subscription_free(c->subscription);

    if (m != 0) {
        c->subscription = pa_subscription_new(c->protocol->core, m, subscription_cb, c);
        pa_assert(c->subscription);
    } else
        c->subscription = NULL;

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_volume(
        PA_GCC_UNUSED pa_pdispatch *pd,
        uint32_t command,
        uint32_t tag,
        pa_tagstruct *t,
        void *userdata) {

    connection *c = CONNECTION(userdata);
    uint32_t idx;
    pa_cvolume volume;
    pa_sink *sink = NULL;
    pa_source *source = NULL;
    pa_sink_input *si = NULL;
    const char *name = NULL;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        (command == PA_COMMAND_SET_SINK_VOLUME && pa_tagstruct_gets(t, &name) < 0) ||
        (command == PA_COMMAND_SET_SOURCE_VOLUME && pa_tagstruct_gets(t, &name) < 0) ||
        pa_tagstruct_get_cvolume(t, &volume) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, idx != PA_INVALID_INDEX || !name || (*name && pa_utf8_valid(name)), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, pa_cvolume_valid(&volume), tag, PA_ERR_INVALID);

    switch (command) {

        case PA_COMMAND_SET_SINK_VOLUME:
            if (idx != PA_INVALID_INDEX)
                sink = pa_idxset_get_by_index(c->protocol->core->sinks, idx);
            else
                sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1);
            break;

        case PA_COMMAND_SET_SOURCE_VOLUME:
            if (idx != PA_INVALID_INDEX)
                source = pa_idxset_get_by_index(c->protocol->core->sources, idx);
            else
                source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1);
            break;

        case PA_COMMAND_SET_SINK_INPUT_VOLUME:
            si = pa_idxset_get_by_index(c->protocol->core->sink_inputs, idx);
            break;

        default:
            pa_assert_not_reached();
    }

    CHECK_VALIDITY(c->pstream, si || sink || source, tag, PA_ERR_NOENTITY);

    if (sink)
        pa_sink_set_volume(sink, &volume);
    else if (source)
        pa_source_set_volume(source, &volume);
    else if (si)
        pa_sink_input_set_volume(si, &volume);

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_mute(
        PA_GCC_UNUSED pa_pdispatch *pd,
        uint32_t command,
        uint32_t tag,
        pa_tagstruct *t,
        void *userdata) {

    connection *c = CONNECTION(userdata);
    uint32_t idx;
    int mute;
    pa_sink *sink = NULL;
    pa_source *source = NULL;
    pa_sink_input *si = NULL;
    const char *name = NULL;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        (command == PA_COMMAND_SET_SINK_MUTE && pa_tagstruct_gets(t, &name) < 0) ||
        (command == PA_COMMAND_SET_SOURCE_MUTE && pa_tagstruct_gets(t, &name) < 0) ||
        pa_tagstruct_get_boolean(t, &mute) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, idx != PA_INVALID_INDEX || !name || (*name && pa_utf8_valid(name)), tag, PA_ERR_INVALID);

    switch (command) {

        case PA_COMMAND_SET_SINK_MUTE:

            if (idx != PA_INVALID_INDEX)
                sink = pa_idxset_get_by_index(c->protocol->core->sinks, idx);
            else
                sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1);

            break;

        case PA_COMMAND_SET_SOURCE_MUTE:
            if (idx != PA_INVALID_INDEX)
                source = pa_idxset_get_by_index(c->protocol->core->sources, idx);
            else
                source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1);

            break;

        case PA_COMMAND_SET_SINK_INPUT_MUTE:
            si = pa_idxset_get_by_index(c->protocol->core->sink_inputs, idx);
            break;

        default:
            pa_assert_not_reached();
    }

    CHECK_VALIDITY(c->pstream, si || sink || source, tag, PA_ERR_NOENTITY);

    if (sink)
        pa_sink_set_mute(sink, mute);
    else if (source)
        pa_source_set_mute(source, mute);
    else if (si)
        pa_sink_input_set_mute(si, mute);

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_cork_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    int b;
    playback_stream *s;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_boolean(t, &b) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, idx != PA_INVALID_INDEX, tag, PA_ERR_INVALID);
    s = pa_idxset_get_by_index(c->output_streams, idx);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
    CHECK_VALIDITY(c->pstream, playback_stream_isinstance(s), tag, PA_ERR_NOENTITY);

    pa_sink_input_cork(s->sink_input, b);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_trigger_or_flush_or_prebuf_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    playback_stream *s;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, idx != PA_INVALID_INDEX, tag, PA_ERR_INVALID);
    s = pa_idxset_get_by_index(c->output_streams, idx);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
    CHECK_VALIDITY(c->pstream, playback_stream_isinstance(s), tag, PA_ERR_NOENTITY);

    switch (command) {
        case PA_COMMAND_FLUSH_PLAYBACK_STREAM:
            pa_asyncmsgq_send(s->sink_input->sink->asyncmsgq, PA_MSGOBJECT(s->sink_input), SINK_INPUT_MESSAGE_FLUSH, NULL, 0, NULL);
            break;

        case PA_COMMAND_PREBUF_PLAYBACK_STREAM:
            pa_asyncmsgq_send(s->sink_input->sink->asyncmsgq, PA_MSGOBJECT(s->sink_input), SINK_INPUT_MESSAGE_PREBUF_FORCE, NULL, 0, NULL);
            break;

        case PA_COMMAND_TRIGGER_PLAYBACK_STREAM:
            pa_asyncmsgq_send(s->sink_input->sink->asyncmsgq, PA_MSGOBJECT(s->sink_input), SINK_INPUT_MESSAGE_TRIGGER, NULL, 0, NULL);
            break;

        default:
            pa_assert_not_reached();
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_cork_record_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    record_stream *s;
    int b;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_boolean(t, &b) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    s = pa_idxset_get_by_index(c->record_streams, idx);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

    pa_source_output_cork(s->source_output, b);
    pa_memblockq_prebuf_force(s->memblockq);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_flush_record_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    record_stream *s;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    s = pa_idxset_get_by_index(c->record_streams, idx);
    CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

    pa_memblockq_flush(s->memblockq);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_stream_buffer_attr(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    uint32_t maxlength, tlength, prebuf, minreq, fragsize;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    if (command == PA_COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR) {
        playback_stream *s;

        s = pa_idxset_get_by_index(c->output_streams, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
        CHECK_VALIDITY(c->pstream, playback_stream_isinstance(s), tag, PA_ERR_NOENTITY);

        if (pa_tagstruct_get(
                    t,
                    PA_TAG_U32, &maxlength,
                    PA_TAG_U32, &tlength,
                    PA_TAG_U32, &prebuf,
                    PA_TAG_U32, &minreq,
                    PA_TAG_INVALID) < 0 ||
            !pa_tagstruct_eof(t)) {
            protocol_error(c);
            return;
        }

        CHECK_VALIDITY(c->pstream, maxlength > 0, tag, PA_ERR_INVALID);
        CHECK_VALIDITY(c->pstream, maxlength <= MAX_MEMBLOCKQ_LENGTH, tag, PA_ERR_INVALID);

        pa_memblockq_set_maxlength(s->memblockq, maxlength);
        pa_memblockq_set_tlength(s->memblockq, tlength);
        pa_memblockq_set_prebuf(s->memblockq, prebuf);
        pa_memblockq_set_minreq(s->memblockq, minreq);

        reply = reply_new(tag);
        pa_tagstruct_putu32(reply, (uint32_t) pa_memblockq_get_maxlength(s->memblockq));
        pa_tagstruct_putu32(reply, (uint32_t) pa_memblockq_get_tlength(s->memblockq));
        pa_tagstruct_putu32(reply, (uint32_t) pa_memblockq_get_prebuf(s->memblockq));
        pa_tagstruct_putu32(reply, (uint32_t) pa_memblockq_get_minreq(s->memblockq));

    } else {
        record_stream *s;
        size_t base;
        pa_assert(command == PA_COMMAND_SET_RECORD_STREAM_BUFFER_ATTR);

        s = pa_idxset_get_by_index(c->record_streams, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

        if (pa_tagstruct_get(
                    t,
                    PA_TAG_U32, &maxlength,
                    PA_TAG_U32, &fragsize,
                    PA_TAG_INVALID) < 0 ||
            !pa_tagstruct_eof(t)) {
            protocol_error(c);
            return;
        }

        CHECK_VALIDITY(c->pstream, maxlength > 0, tag, PA_ERR_INVALID);
        CHECK_VALIDITY(c->pstream, maxlength <= MAX_MEMBLOCKQ_LENGTH, tag, PA_ERR_INVALID);

        pa_memblockq_set_maxlength(s->memblockq, maxlength);

        base = pa_frame_size(&s->source_output->sample_spec);
        s->fragment_size = (fragsize/base)*base;
        if (s->fragment_size <= 0)
            s->fragment_size = base;

        if (s->fragment_size > pa_memblockq_get_maxlength(s->memblockq))
            s->fragment_size = pa_memblockq_get_maxlength(s->memblockq);

        reply = reply_new(tag);
        pa_tagstruct_putu32(reply, (uint32_t) pa_memblockq_get_maxlength(s->memblockq));
        pa_tagstruct_putu32(reply, s->fragment_size);
    }

    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_update_stream_sample_rate(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    uint32_t rate;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_getu32(t, &rate) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, rate > 0 && rate <= PA_RATE_MAX, tag, PA_ERR_INVALID);

    if (command == PA_COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE) {
        playback_stream *s;

        s = pa_idxset_get_by_index(c->output_streams, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
        CHECK_VALIDITY(c->pstream, playback_stream_isinstance(s), tag, PA_ERR_NOENTITY);

        pa_sink_input_set_rate(s->sink_input, rate);

    } else {
        record_stream *s;
        pa_assert(command == PA_COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE);

        s = pa_idxset_get_by_index(c->record_streams, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

        pa_source_output_set_rate(s->source_output, rate);
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_default_sink_or_source(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const char *s;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &s) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, !s || (*s && pa_utf8_valid(s)), tag, PA_ERR_INVALID);

    pa_namereg_set_default(c->protocol->core, s, command == PA_COMMAND_SET_DEFAULT_SOURCE ? PA_NAMEREG_SOURCE : PA_NAMEREG_SINK);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_stream_name(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    const char *name;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name && pa_utf8_valid(name), tag, PA_ERR_INVALID);

    if (command == PA_COMMAND_SET_PLAYBACK_STREAM_NAME) {
        playback_stream *s;

        s = pa_idxset_get_by_index(c->output_streams, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);
        CHECK_VALIDITY(c->pstream, playback_stream_isinstance(s), tag, PA_ERR_NOENTITY);

        pa_sink_input_set_name(s->sink_input, name);

    } else {
        record_stream *s;
        pa_assert(command == PA_COMMAND_SET_RECORD_STREAM_NAME);

        s = pa_idxset_get_by_index(c->record_streams, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

        pa_source_output_set_name(s->source_output, name);
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_kill(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    if (command == PA_COMMAND_KILL_CLIENT) {
        pa_client *client;

        client = pa_idxset_get_by_index(c->protocol->core->clients, idx);
        CHECK_VALIDITY(c->pstream, client, tag, PA_ERR_NOENTITY);

        connection_ref(c);
        pa_client_kill(client);

    } else if (command == PA_COMMAND_KILL_SINK_INPUT) {
        pa_sink_input *s;

        s = pa_idxset_get_by_index(c->protocol->core->sink_inputs, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

        connection_ref(c);
        pa_sink_input_kill(s);
    } else {
        pa_source_output *s;

        pa_assert(command == PA_COMMAND_KILL_SOURCE_OUTPUT);

        s = pa_idxset_get_by_index(c->protocol->core->source_outputs, idx);
        CHECK_VALIDITY(c->pstream, s, tag, PA_ERR_NOENTITY);

        connection_ref(c);
        pa_source_output_kill(s);
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
    connection_unref(c);
}

static void command_load_module(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_module *m;
    const char *name, *argument;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_gets(t, &argument) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name && *name && pa_utf8_valid(name) && !strchr(name, '/'), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, !argument || pa_utf8_valid(argument), tag, PA_ERR_INVALID);

    if (!(m = pa_module_load(c->protocol->core, name, argument))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_MODINITFAILED);
        return;
    }

    reply = reply_new(tag);
    pa_tagstruct_putu32(reply, m->index);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_unload_module(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx;
    pa_module *m;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    m = pa_idxset_get_by_index(c->protocol->core->modules, idx);
    CHECK_VALIDITY(c->pstream, m, tag, PA_ERR_NOENTITY);

    pa_module_unload_request(m);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_add_autoload(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const char *name, *module, *argument;
    uint32_t type;
    uint32_t idx;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_getu32(t, &type) < 0 ||
        pa_tagstruct_gets(t, &module) < 0 ||
        pa_tagstruct_gets(t, &argument) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name && *name && pa_utf8_valid(name), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, type == 0 || type == 1, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, module && *module && pa_utf8_valid(module), tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, !argument || pa_utf8_valid(argument), tag, PA_ERR_INVALID);

    if (pa_autoload_add(c->protocol->core, name, type == 0 ? PA_NAMEREG_SINK : PA_NAMEREG_SOURCE, module, argument, &idx) < 0) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
        return;
    }

    reply = reply_new(tag);
    pa_tagstruct_putu32(reply, idx);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_remove_autoload(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const char *name = NULL;
    uint32_t type, idx = PA_IDXSET_INVALID;
    int r;

    connection_assert_ref(c);
    pa_assert(t);

    if ((pa_tagstruct_getu32(t, &idx) < 0 &&
        (pa_tagstruct_gets(t, &name) < 0 ||
         pa_tagstruct_getu32(t, &type) < 0)) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name || idx != PA_IDXSET_INVALID, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, !name || (*name && pa_utf8_valid(name) && (type == 0 || type == 1)), tag, PA_ERR_INVALID);

    if (name)
        r = pa_autoload_remove_by_name(c->protocol->core, name, type == 0 ? PA_NAMEREG_SINK : PA_NAMEREG_SOURCE);
    else
        r = pa_autoload_remove_by_index(c->protocol->core, idx);

    CHECK_VALIDITY(c->pstream, r >= 0, tag, PA_ERR_NOENTITY);

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void autoload_fill_tagstruct(pa_tagstruct *t, const pa_autoload_entry *e) {
    pa_assert(t && e);

    pa_tagstruct_putu32(t, e->index);
    pa_tagstruct_puts(t, e->name);
    pa_tagstruct_putu32(t, e->type == PA_NAMEREG_SINK ? 0 : 1);
    pa_tagstruct_puts(t, e->module);
    pa_tagstruct_puts(t, e->argument);
}

static void command_get_autoload_info(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    const pa_autoload_entry *a = NULL;
    uint32_t type, idx;
    const char *name;
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if ((pa_tagstruct_getu32(t, &idx) < 0 &&
        (pa_tagstruct_gets(t, &name) < 0 ||
         pa_tagstruct_getu32(t, &type) < 0)) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, name || idx != PA_IDXSET_INVALID, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, !name || (*name && (type == 0 || type == 1) && pa_utf8_valid(name)), tag, PA_ERR_INVALID);

    if (name)
        a = pa_autoload_get_by_name(c->protocol->core, name, type == 0 ? PA_NAMEREG_SINK : PA_NAMEREG_SOURCE);
    else
        a = pa_autoload_get_by_index(c->protocol->core, idx);

    CHECK_VALIDITY(c->pstream, a, tag, PA_ERR_NOENTITY);

    reply = reply_new(tag);
    autoload_fill_tagstruct(reply, a);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_autoload_info_list(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    pa_tagstruct *reply;

    connection_assert_ref(c);
    pa_assert(t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);

    reply = reply_new(tag);

    if (c->protocol->core->autoload_hashmap) {
        pa_autoload_entry *a;
        void *state = NULL;

        while ((a = pa_hashmap_iterate(c->protocol->core->autoload_hashmap, &state, NULL)))
            autoload_fill_tagstruct(reply, a);
    }

    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_move_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx = PA_INVALID_INDEX, idx_device = PA_INVALID_INDEX;
    const char *name = NULL;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_getu32(t, &idx_device) < 0 ||
        pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, idx != PA_INVALID_INDEX, tag, PA_ERR_INVALID);
    CHECK_VALIDITY(c->pstream, idx_device != PA_INVALID_INDEX || !name || (*name && pa_utf8_valid(name)), tag, PA_ERR_INVALID);

    if (command == PA_COMMAND_MOVE_SINK_INPUT) {
        pa_sink_input *si = NULL;
        pa_sink *sink = NULL;

        si = pa_idxset_get_by_index(c->protocol->core->sink_inputs, idx);

        if (idx_device != PA_INVALID_INDEX)
            sink = pa_idxset_get_by_index(c->protocol->core->sinks, idx_device);
        else
            sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1);

        CHECK_VALIDITY(c->pstream, si && sink, tag, PA_ERR_NOENTITY);

        if (pa_sink_input_move_to(si, sink, 0) < 0) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
            return;
        }
    } else {
        pa_source_output *so = NULL;
        pa_source *source;

        pa_assert(command == PA_COMMAND_MOVE_SOURCE_OUTPUT);

        so = pa_idxset_get_by_index(c->protocol->core->source_outputs, idx);

        if (idx_device != PA_INVALID_INDEX)
            source = pa_idxset_get_by_index(c->protocol->core->sources, idx_device);
        else
            source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1);

        CHECK_VALIDITY(c->pstream, so && source, tag, PA_ERR_NOENTITY);

        if (pa_source_output_move_to(so, source) < 0) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
            return;
        }
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_suspend(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    connection *c = CONNECTION(userdata);
    uint32_t idx = PA_INVALID_INDEX;
    const char *name = NULL;
    int b;

    connection_assert_ref(c);
    pa_assert(t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_get_boolean(t, &b) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    CHECK_VALIDITY(c->pstream, c->authorized, tag, PA_ERR_ACCESS);
    CHECK_VALIDITY(c->pstream, idx != PA_INVALID_INDEX || !name || !*name || pa_utf8_valid(name), tag, PA_ERR_INVALID);

    if (command == PA_COMMAND_SUSPEND_SINK) {

        if (idx == PA_INVALID_INDEX && name && !*name) {

            if (pa_sink_suspend_all(c->protocol->core, b) < 0) {
                pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
                return;
            }
        } else {
            pa_sink *sink = NULL;

            if (idx != PA_INVALID_INDEX)
                sink = pa_idxset_get_by_index(c->protocol->core->sinks, idx);
            else
                sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1);

            CHECK_VALIDITY(c->pstream, sink, tag, PA_ERR_NOENTITY);

            if (pa_sink_suspend(sink, b) < 0) {
                pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
                return;
            }
        }
    } else {

        pa_assert(command == PA_COMMAND_SUSPEND_SOURCE);

        if (idx == PA_INVALID_INDEX && name && !*name) {

            if (pa_source_suspend_all(c->protocol->core, b) < 0) {
                pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
                return;
            }

        } else {
            pa_source *source;

            if (idx != PA_INVALID_INDEX)
                source = pa_idxset_get_by_index(c->protocol->core->sources, idx);
            else
                source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1);

            CHECK_VALIDITY(c->pstream, source, tag, PA_ERR_NOENTITY);

            if (pa_source_suspend(source, b) < 0) {
                pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
                return;
            }
        }
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

/*** pstream callbacks ***/

static void pstream_packet_callback(pa_pstream *p, pa_packet *packet, const pa_creds *creds, void *userdata) {
    connection *c = CONNECTION(userdata);

    pa_assert(p);
    pa_assert(packet);
    connection_assert_ref(c);

    if (pa_pdispatch_run(c->pdispatch, packet, creds, c) < 0) {
        pa_log("invalid packet.");
        connection_unlink(c);
    }
}

static void pstream_memblock_callback(pa_pstream *p, uint32_t channel, int64_t offset, pa_seek_mode_t seek, const pa_memchunk *chunk, void *userdata) {
    connection *c = CONNECTION(userdata);
    output_stream *stream;

    pa_assert(p);
    pa_assert(chunk);
    connection_assert_ref(c);

    if (!(stream = OUTPUT_STREAM(pa_idxset_get_by_index(c->output_streams, channel)))) {
        pa_log("client sent block for invalid stream.");
        /* Ignoring */
        return;
    }

    if (playback_stream_isinstance(stream)) {
        playback_stream *ps = PLAYBACK_STREAM(stream);

        if (seek != PA_SEEK_RELATIVE || offset != 0)
            pa_asyncmsgq_post(ps->sink_input->sink->asyncmsgq, PA_MSGOBJECT(ps->sink_input), SINK_INPUT_MESSAGE_SEEK, PA_UINT_TO_PTR(seek), offset, NULL, NULL);

        pa_asyncmsgq_post(ps->sink_input->sink->asyncmsgq, PA_MSGOBJECT(ps->sink_input), SINK_INPUT_MESSAGE_POST_DATA, NULL, 0, chunk, NULL);

    } else {
        upload_stream *u = UPLOAD_STREAM(stream);
        size_t l;

        if (!u->memchunk.memblock) {
            if (u->length == chunk->length) {
                u->memchunk = *chunk;
                pa_memblock_ref(u->memchunk.memblock);
                u->length = 0;
            } else {
                u->memchunk.memblock = pa_memblock_new(c->protocol->core->mempool, u->length);
                u->memchunk.index = u->memchunk.length = 0;
            }
        }

        pa_assert(u->memchunk.memblock);

        l = u->length;
        if (l > chunk->length)
            l = chunk->length;


        if (l > 0) {
            void *src, *dst;
            dst = pa_memblock_acquire(u->memchunk.memblock);
            src = pa_memblock_acquire(chunk->memblock);

            memcpy((uint8_t*) dst + u->memchunk.index + u->memchunk.length,
                   (uint8_t*) src+chunk->index, l);

            pa_memblock_release(u->memchunk.memblock);
            pa_memblock_release(chunk->memblock);

            u->memchunk.length += l;
            u->length -= l;
        }
    }
}

static void pstream_die_callback(pa_pstream *p, void *userdata) {
    connection *c = CONNECTION(userdata);

    pa_assert(p);
    connection_assert_ref(c);

    connection_unlink(c);
    pa_log_info("connection died.");
}

static void pstream_drain_callback(pa_pstream *p, void *userdata) {
    connection *c = CONNECTION(userdata);

    pa_assert(p);
    connection_assert_ref(c);

    send_memblock(c);
}

static void pstream_revoke_callback(pa_pstream *p, uint32_t block_id, void *userdata) {
    pa_thread_mq *q;

    if (!(q = pa_thread_mq_get()))
        pa_pstream_send_revoke(p, block_id);
    else
        pa_asyncmsgq_post(q->outq, PA_MSGOBJECT(userdata), CONNECTION_MESSAGE_REVOKE, PA_UINT_TO_PTR(block_id), 0, NULL, NULL);
}

static void pstream_release_callback(pa_pstream *p, uint32_t block_id, void *userdata) {
    pa_thread_mq *q;

    if (!(q = pa_thread_mq_get()))
        pa_pstream_send_release(p, block_id);
    else
        pa_asyncmsgq_post(q->outq, PA_MSGOBJECT(userdata), CONNECTION_MESSAGE_RELEASE, PA_UINT_TO_PTR(block_id), 0, NULL, NULL);
}

/*** client callbacks ***/

static void client_kill_cb(pa_client *c) {
    pa_assert(c);

    connection_unlink(CONNECTION(c->userdata));
}

/*** socket server callbacks ***/

static void auth_timeout(pa_mainloop_api*m, pa_time_event *e, const struct timeval *tv, void *userdata) {
    connection *c = CONNECTION(userdata);

    pa_assert(m);
    pa_assert(tv);
    connection_assert_ref(c);
    pa_assert(c->auth_timeout_event == e);

    if (!c->authorized)
        connection_unlink(c);
}

static void on_connection(PA_GCC_UNUSED pa_socket_server*s, pa_iochannel *io, void *userdata) {
    pa_protocol_native *p = userdata;
    connection *c;
    char cname[256], pname[128];

    pa_assert(s);
    pa_assert(io);
    pa_assert(p);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log_warn("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_msgobject_new(connection);
    c->parent.parent.free = connection_free;
    c->parent.process_msg = connection_process_msg;

    c->authorized = !!p->public;

    if (!c->authorized && p->auth_ip_acl && pa_ip_acl_check(p->auth_ip_acl, pa_iochannel_get_recv_fd(io)) > 0) {
        pa_log_info("Client authenticated by IP ACL.");
        c->authorized = 1;
    }

    if (!c->authorized) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec += AUTH_TIMEOUT;
        c->auth_timeout_event = p->core->mainloop->time_new(p->core->mainloop, &tv, auth_timeout, c);
    } else
        c->auth_timeout_event = NULL;

    c->version = 8;
    c->protocol = p;
    pa_iochannel_socket_peer_to_string(io, pname, sizeof(pname));
    pa_snprintf(cname, sizeof(cname), "Native client (%s)", pname);
    c->client = pa_client_new(p->core, __FILE__, cname);
    c->client->kill = client_kill_cb;
    c->client->userdata = c;
    c->client->owner = p->module;

    c->pstream = pa_pstream_new(p->core->mainloop, io, p->core->mempool);

    pa_pstream_set_recieve_packet_callback(c->pstream, pstream_packet_callback, c);
    pa_pstream_set_recieve_memblock_callback(c->pstream, pstream_memblock_callback, c);
    pa_pstream_set_die_callback(c->pstream, pstream_die_callback, c);
    pa_pstream_set_drain_callback(c->pstream, pstream_drain_callback, c);
    pa_pstream_set_revoke_callback(c->pstream, pstream_revoke_callback, c);
    pa_pstream_set_release_callback(c->pstream, pstream_release_callback, c);

    c->pdispatch = pa_pdispatch_new(p->core->mainloop, command_table, PA_COMMAND_MAX);

    c->record_streams = pa_idxset_new(NULL, NULL);
    c->output_streams = pa_idxset_new(NULL, NULL);

    c->rrobin_index = PA_IDXSET_INVALID;
    c->subscription = NULL;

    pa_idxset_put(p->connections, c, NULL);

#ifdef HAVE_CREDS
    if (pa_iochannel_creds_supported(io))
        pa_iochannel_creds_enable(io);

#endif
}

/*** module entry points ***/

static int load_key(pa_protocol_native*p, const char*fn) {
    pa_assert(p);

    p->auth_cookie_in_property = 0;

    if (!fn && pa_authkey_prop_get(p->core, PA_NATIVE_COOKIE_PROPERTY_NAME, p->auth_cookie, sizeof(p->auth_cookie)) >= 0) {
        pa_log_info("using already loaded auth cookie.");
        pa_authkey_prop_ref(p->core, PA_NATIVE_COOKIE_PROPERTY_NAME);
        p->auth_cookie_in_property = 1;
        return 0;
    }

    if (!fn)
        fn = PA_NATIVE_COOKIE_FILE;

    if (pa_authkey_load_auto(fn, p->auth_cookie, sizeof(p->auth_cookie)) < 0)
        return -1;

    pa_log_info("loading cookie from disk.");

    if (pa_authkey_prop_put(p->core, PA_NATIVE_COOKIE_PROPERTY_NAME, p->auth_cookie, sizeof(p->auth_cookie)) >= 0)
        p->auth_cookie_in_property = 1;

    return 0;
}

static pa_protocol_native* protocol_new_internal(pa_core *c, pa_module *m, pa_modargs *ma) {
    pa_protocol_native *p;
    pa_bool_t public = FALSE;
    const char *acl;

    pa_assert(c);
    pa_assert(ma);

    if (pa_modargs_get_value_boolean(ma, "auth-anonymous", &public) < 0) {
        pa_log("auth-anonymous= expects a boolean argument.");
        return NULL;
    }

    p = pa_xnew(pa_protocol_native, 1);
    p->core = c;
    p->module = m;
    p->public = public;
    p->server = NULL;
    p->auth_ip_acl = NULL;

#ifdef HAVE_CREDS
    {
        pa_bool_t a = 1;
        if (pa_modargs_get_value_boolean(ma, "auth-group-enabled", &a) < 0) {
            pa_log("auth-group-enabled= expects a boolean argument.");
            return NULL;
        }
        p->auth_group = a ? pa_xstrdup(pa_modargs_get_value(ma, "auth-group", c->is_system_instance ? PA_ACCESS_GROUP : NULL)) : NULL;

        if (p->auth_group)
            pa_log_info("Allowing access to group '%s'.", p->auth_group);
    }
#endif


    if ((acl = pa_modargs_get_value(ma, "auth-ip-acl", NULL))) {

        if (!(p->auth_ip_acl = pa_ip_acl_new(acl))) {
            pa_log("Failed to parse IP ACL '%s'", acl);
            goto fail;
        }
    }

    if (load_key(p, pa_modargs_get_value(ma, "cookie", NULL)) < 0)
        goto fail;

    p->connections = pa_idxset_new(NULL, NULL);

    return p;

fail:
#ifdef HAVE_CREDS
    pa_xfree(p->auth_group);
#endif
    if (p->auth_ip_acl)
        pa_ip_acl_free(p->auth_ip_acl);
    pa_xfree(p);
    return NULL;
}

pa_protocol_native* pa_protocol_native_new(pa_core *core, pa_socket_server *server, pa_module *m, pa_modargs *ma) {
    char t[256];
    pa_protocol_native *p;

    if (!(p = protocol_new_internal(core, m, ma)))
        return NULL;

    p->server = server;
    pa_socket_server_set_callback(p->server, on_connection, p);

    if (pa_socket_server_get_address(p->server, t, sizeof(t))) {
        pa_strlist *l;
        l = pa_property_get(core, PA_NATIVE_SERVER_PROPERTY_NAME);
        l = pa_strlist_prepend(l, t);
        pa_property_replace(core, PA_NATIVE_SERVER_PROPERTY_NAME, l);
    }

    return p;
}

void pa_protocol_native_free(pa_protocol_native *p) {
    connection *c;
    pa_assert(p);

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_unlink(c);
    pa_idxset_free(p->connections, NULL, NULL);

    if (p->server) {
        char t[256];

        if (pa_socket_server_get_address(p->server, t, sizeof(t))) {
            pa_strlist *l;
            l = pa_property_get(p->core, PA_NATIVE_SERVER_PROPERTY_NAME);
            l = pa_strlist_remove(l, t);

            if (l)
                pa_property_replace(p->core, PA_NATIVE_SERVER_PROPERTY_NAME, l);
            else
                pa_property_remove(p->core, PA_NATIVE_SERVER_PROPERTY_NAME);
        }

        pa_socket_server_unref(p->server);
    }

    if (p->auth_cookie_in_property)
        pa_authkey_prop_unref(p->core, PA_NATIVE_COOKIE_PROPERTY_NAME);

    if (p->auth_ip_acl)
        pa_ip_acl_free(p->auth_ip_acl);

#ifdef HAVE_CREDS
    pa_xfree(p->auth_group);
#endif
    pa_xfree(p);
}

pa_protocol_native* pa_protocol_native_new_iochannel(
        pa_core*core,
        pa_iochannel *io,
        pa_module *m,
        pa_modargs *ma) {

    pa_protocol_native *p;

    if (!(p = protocol_new_internal(core, m, ma)))
        return NULL;

    on_connection(NULL, io, p);

    return p;
}
