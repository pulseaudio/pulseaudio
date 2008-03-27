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

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include <pulse/sample.h>
#include <pulse/timeval.h>
#include <pulse/utf8.h>
#include <pulse/xmalloc.h>

#include <pulsecore/esound.h>
#include <pulsecore/memblock.h>
#include <pulsecore/client.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/sink.h>
#include <pulsecore/source-output.h>
#include <pulsecore/source.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/authkey.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/ipacl.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread-mq.h>

#include "endianmacros.h"

#include "protocol-esound.h"

/* Don't accept more connection than this */
#define MAX_CONNECTIONS 64

/* Kick a client if it doesn't authenticate within this time */
#define AUTH_TIMEOUT 5

#define DEFAULT_COOKIE_FILE ".esd_auth"

#define PLAYBACK_BUFFER_SECONDS (.25)
#define PLAYBACK_BUFFER_FRAGMENTS (10)
#define RECORD_BUFFER_SECONDS (5)
#define RECORD_BUFFER_FRAGMENTS (100)

#define MAX_CACHE_SAMPLE_SIZE (2048000)

#define SCACHE_PREFIX "esound."

/* This is heavily based on esound's code */

typedef struct connection {
    pa_msgobject parent;

    uint32_t index;
    pa_bool_t dead;
    pa_protocol_esound *protocol;
    pa_iochannel *io;
    pa_client *client;
    pa_bool_t authorized, swap_byte_order;
    void *write_data;
    size_t write_data_alloc, write_data_index, write_data_length;
    void *read_data;
    size_t read_data_alloc, read_data_length;
    esd_proto_t request;
    esd_client_state_t state;
    pa_sink_input *sink_input;
    pa_source_output *source_output;
    pa_memblockq *input_memblockq, *output_memblockq;
    pa_defer_event *defer_event;

    char *original_name;

    struct {
        pa_memblock *current_memblock;
        size_t memblock_index, fragment_size;
        pa_atomic_t missing;
    } playback;

    struct {
        pa_memchunk memchunk;
        char *name;
        pa_sample_spec sample_spec;
    } scache;

    pa_time_event *auth_timeout_event;
} connection;

PA_DECLARE_CLASS(connection);
#define CONNECTION(o) (connection_cast(o))
static PA_DEFINE_CHECK_TYPE(connection, pa_msgobject);

struct pa_protocol_esound {
    pa_module *module;
    pa_core *core;
    int public;
    pa_socket_server *server;
    pa_idxset *connections;

    char *sink_name, *source_name;
    unsigned n_player;
    uint8_t esd_key[ESD_KEY_LEN];
    pa_ip_acl *auth_ip_acl;
};

enum {
    SINK_INPUT_MESSAGE_POST_DATA = PA_SINK_INPUT_MESSAGE_MAX, /* data from main loop to sink input */
    SINK_INPUT_MESSAGE_DISABLE_PREBUF
};

enum {
    CONNECTION_MESSAGE_REQUEST_DATA,
    CONNECTION_MESSAGE_POST_DATA,
    CONNECTION_MESSAGE_UNLINK_CONNECTION
};

typedef struct proto_handler {
    size_t data_length;
    int (*proc)(connection *c, esd_proto_t request, const void *data, size_t length);
    const char *description;
} esd_proto_handler_info_t;

static void sink_input_drop_cb(pa_sink_input *i, size_t length);
static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk);
static void sink_input_kill_cb(pa_sink_input *i);
static int sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk);
static pa_usec_t source_output_get_latency_cb(pa_source_output *o);

static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk);
static void source_output_kill_cb(pa_source_output *o);

static int esd_proto_connect(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_stream_play(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_stream_record(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_get_latency(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_server_info(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_all_info(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_stream_pan(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_sample_cache(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_sample_free_or_play(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_sample_get_id(connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_standby_or_resume(connection *c, esd_proto_t request, const void *data, size_t length);

/* the big map of protocol handler info */
static struct proto_handler proto_map[ESD_PROTO_MAX] = {
    { ESD_KEY_LEN + sizeof(int),      esd_proto_connect, "connect" },
    { ESD_KEY_LEN + sizeof(int),      NULL, "lock" },
    { ESD_KEY_LEN + sizeof(int),      NULL, "unlock" },

    { ESD_NAME_MAX + 2 * sizeof(int), esd_proto_stream_play, "stream play" },
    { ESD_NAME_MAX + 2 * sizeof(int), esd_proto_stream_record, "stream rec" },
    { ESD_NAME_MAX + 2 * sizeof(int), esd_proto_stream_record, "stream mon" },

    { ESD_NAME_MAX + 3 * sizeof(int), esd_proto_sample_cache, "sample cache" },                      /* 6 */
    { sizeof(int),                    esd_proto_sample_free_or_play, "sample free" },
    { sizeof(int),                    esd_proto_sample_free_or_play, "sample play" },                /* 8 */
    { sizeof(int),                    NULL, "sample loop" },
    { sizeof(int),                    NULL, "sample stop" },
    { -1,                             NULL, "TODO: sample kill" },

    { ESD_KEY_LEN + sizeof(int),      esd_proto_standby_or_resume, "standby" },  /* NOOP! */
    { ESD_KEY_LEN + sizeof(int),      esd_proto_standby_or_resume, "resume" },   /* NOOP! */         /* 13 */

    { ESD_NAME_MAX,                   esd_proto_sample_get_id, "sample getid" },                     /* 14 */
    { ESD_NAME_MAX + 2 * sizeof(int), NULL, "stream filter" },

    { sizeof(int),                    esd_proto_server_info, "server info" },
    { sizeof(int),                    esd_proto_all_info, "all info" },
    { -1,                             NULL, "TODO: subscribe" },
    { -1,                             NULL, "TODO: unsubscribe" },

    { 3 * sizeof(int),                esd_proto_stream_pan, "stream pan"},
    { 3 * sizeof(int),                NULL, "sample pan" },

    { sizeof(int),                    NULL, "standby mode" },
    { 0,                              esd_proto_get_latency, "get latency" }
};

static void connection_unlink(connection *c) {
    pa_assert(c);

    if (!c->protocol)
        return;

    if (c->sink_input) {
        pa_sink_input_unlink(c->sink_input);
        pa_sink_input_unref(c->sink_input);
        c->sink_input = NULL;
    }

    if (c->source_output) {
        pa_source_output_unlink(c->source_output);
        pa_source_output_unref(c->source_output);
        c->source_output = NULL;
    }

    if (c->client) {
        pa_client_free(c->client);
        c->client = NULL;
    }

    if (c->state == ESD_STREAMING_DATA)
        c->protocol->n_player--;

    if (c->io) {
        pa_iochannel_free(c->io);
        c->io = NULL;
    }

    if (c->defer_event) {
        c->protocol->core->mainloop->defer_free(c->defer_event);
        c->defer_event = NULL;
    }

    if (c->auth_timeout_event) {
        c->protocol->core->mainloop->time_free(c->auth_timeout_event);
        c->auth_timeout_event = NULL;
    }

    pa_assert_se(pa_idxset_remove_by_data(c->protocol->connections, c, NULL) == c);
    c->protocol = NULL;
    connection_unref(c);
}

static void connection_free(pa_object *obj) {
    connection *c = CONNECTION(obj);
    pa_assert(c);

    if (c->input_memblockq)
        pa_memblockq_free(c->input_memblockq);
    if (c->output_memblockq)
        pa_memblockq_free(c->output_memblockq);

    if (c->playback.current_memblock)
        pa_memblock_unref(c->playback.current_memblock);

    pa_xfree(c->read_data);
    pa_xfree(c->write_data);

    if (c->scache.memchunk.memblock)
        pa_memblock_unref(c->scache.memchunk.memblock);
    pa_xfree(c->scache.name);

    pa_xfree(c->original_name);
    pa_xfree(c);
}

static void connection_write_prepare(connection *c, size_t length) {
    size_t t;
    pa_assert(c);

    t = c->write_data_length+length;

    if (c->write_data_alloc < t)
        c->write_data = pa_xrealloc(c->write_data, c->write_data_alloc = t);

    pa_assert(c->write_data);
}

static void connection_write(connection *c, const void *data, size_t length) {
    size_t i;
    pa_assert(c);

    c->protocol->core->mainloop->defer_enable(c->defer_event, 1);

    connection_write_prepare(c, length);

    pa_assert(c->write_data);

    i = c->write_data_length;
    c->write_data_length += length;

    memcpy((uint8_t*) c->write_data + i, data, length);
}

static void format_esd2native(int format, pa_bool_t swap_bytes, pa_sample_spec *ss) {
    pa_assert(ss);

    ss->channels = ((format & ESD_MASK_CHAN) == ESD_STEREO) ? 2 : 1;
    if ((format & ESD_MASK_BITS) == ESD_BITS16)
        ss->format = swap_bytes ? PA_SAMPLE_S16RE : PA_SAMPLE_S16NE;
    else
        ss->format = PA_SAMPLE_U8;
}

static int format_native2esd(pa_sample_spec *ss) {
    int format = 0;

    format = (ss->format == PA_SAMPLE_U8) ? ESD_BITS8 : ESD_BITS16;
    format |= (ss->channels >= 2) ? ESD_STEREO : ESD_MONO;

    return format;
}

#define CHECK_VALIDITY(expression, ...) do { \
    if (!(expression)) { \
        pa_log_warn(__FILE__ ": " __VA_ARGS__); \
        return -1; \
    } \
} while(0);

/*** esound commands ***/

static int esd_proto_connect(connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    uint32_t ekey;
    int ok;

    connection_assert_ref(c);
    pa_assert(data);
    pa_assert(length == (ESD_KEY_LEN + sizeof(uint32_t)));

    if (!c->authorized) {
        if (memcmp(data, c->protocol->esd_key, ESD_KEY_LEN) != 0) {
            pa_log("kicked client with invalid authorization key.");
            return -1;
        }

        c->authorized = TRUE;
        if (c->auth_timeout_event) {
            c->protocol->core->mainloop->time_free(c->auth_timeout_event);
            c->auth_timeout_event = NULL;
        }
    }

    data = (const char*)data + ESD_KEY_LEN;

    memcpy(&ekey, data, sizeof(uint32_t));
    if (ekey == ESD_ENDIAN_KEY)
        c->swap_byte_order = FALSE;
    else if (ekey == ESD_SWAP_ENDIAN_KEY)
        c->swap_byte_order = TRUE;
    else {
        pa_log_warn("Client sent invalid endian key");
        return -1;
    }

    ok = 1;
    connection_write(c, &ok, sizeof(int));
    return 0;
}

static int esd_proto_stream_play(connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    char name[ESD_NAME_MAX], *utf8_name;
    int32_t format, rate;
    pa_sample_spec ss;
    size_t l;
    pa_sink *sink = NULL;
    pa_sink_input_new_data sdata;

    connection_assert_ref(c);
    pa_assert(data);
    pa_assert(length == (sizeof(int32_t)*2+ESD_NAME_MAX));

    memcpy(&format, data, sizeof(int32_t));
    format = PA_MAYBE_INT32_SWAP(c->swap_byte_order, format);
    data = (const char*) data + sizeof(int32_t);

    memcpy(&rate, data, sizeof(int32_t));
    rate = PA_MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    data = (const char*) data + sizeof(int32_t);

    ss.rate = rate;
    format_esd2native(format, c->swap_byte_order, &ss);

    CHECK_VALIDITY(pa_sample_spec_valid(&ss), "Invalid sample specification");

    if (c->protocol->sink_name) {
        sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1);
        CHECK_VALIDITY(sink, "No such sink: %s", c->protocol->sink_name);
    }

    strncpy(name, data, sizeof(name));
    name[sizeof(name)-1] = 0;

    utf8_name = pa_utf8_filter(name);
    pa_client_set_name(c->client, utf8_name);
    pa_xfree(utf8_name);

    c->original_name = pa_xstrdup(name);

    pa_assert(!c->sink_input && !c->input_memblockq);

    pa_sink_input_new_data_init(&sdata);
    sdata.sink = sink;
    sdata.driver = __FILE__;
    sdata.name = c->client->name;
    pa_sink_input_new_data_set_sample_spec(&sdata, &ss);
    sdata.module = c->protocol->module;
    sdata.client = c->client;

    c->sink_input = pa_sink_input_new(c->protocol->core, &sdata, 0);
    CHECK_VALIDITY(c->sink_input, "Failed to create sink input.");

    l = (size_t) (pa_bytes_per_second(&ss)*PLAYBACK_BUFFER_SECONDS);
    c->input_memblockq = pa_memblockq_new(
            0,
            l,
            0,
            pa_frame_size(&ss),
            (size_t) -1,
            l/PLAYBACK_BUFFER_FRAGMENTS,
            NULL);
    pa_iochannel_socket_set_rcvbuf(c->io, l/PLAYBACK_BUFFER_FRAGMENTS*2);
    c->playback.fragment_size = l/PLAYBACK_BUFFER_FRAGMENTS;

    c->sink_input->parent.process_msg = sink_input_process_msg;
    c->sink_input->peek = sink_input_peek_cb;
    c->sink_input->drop = sink_input_drop_cb;
    c->sink_input->kill = sink_input_kill_cb;
    c->sink_input->userdata = c;

    c->state = ESD_STREAMING_DATA;

    c->protocol->n_player++;

    pa_atomic_store(&c->playback.missing, pa_memblockq_missing(c->input_memblockq));

    pa_sink_input_put(c->sink_input);

    return 0;
}

static int esd_proto_stream_record(connection *c, esd_proto_t request, const void *data, size_t length) {
    char name[ESD_NAME_MAX], *utf8_name;
    int32_t format, rate;
    pa_source *source = NULL;
    pa_sample_spec ss;
    size_t l;
    pa_source_output_new_data sdata;

    connection_assert_ref(c);
    pa_assert(data);
    pa_assert(length == (sizeof(int32_t)*2+ESD_NAME_MAX));

    memcpy(&format, data, sizeof(int32_t));
    format = PA_MAYBE_INT32_SWAP(c->swap_byte_order, format);
    data = (const char*) data + sizeof(int32_t);

    memcpy(&rate, data, sizeof(int32_t));
    rate = PA_MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    data = (const char*) data + sizeof(int32_t);

    ss.rate = rate;
    format_esd2native(format, c->swap_byte_order, &ss);

    CHECK_VALIDITY(pa_sample_spec_valid(&ss), "Invalid sample specification.");

    if (request == ESD_PROTO_STREAM_MON) {
        pa_sink* sink;

        if (!(sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1))) {
            pa_log("no such sink.");
            return -1;
        }

        if (!(source = sink->monitor_source)) {
            pa_log("no such monitor source.");
            return -1;
        }
    } else {
        pa_assert(request == ESD_PROTO_STREAM_REC);

        if (c->protocol->source_name) {
            if (!(source = pa_namereg_get(c->protocol->core, c->protocol->source_name, PA_NAMEREG_SOURCE, 1))) {
                pa_log("no such source.");
                return -1;
            }
        }
    }

    strncpy(name, data, sizeof(name));
    name[sizeof(name)-1] = 0;

    utf8_name = pa_utf8_filter(name);
    pa_client_set_name(c->client, utf8_name);
    pa_xfree(utf8_name);

    c->original_name = pa_xstrdup(name);

    pa_assert(!c->output_memblockq && !c->source_output);

    pa_source_output_new_data_init(&sdata);
    sdata.source = source;
    sdata.driver = __FILE__;
    sdata.name = c->client->name;
    pa_source_output_new_data_set_sample_spec(&sdata, &ss);
    sdata.module = c->protocol->module;
    sdata.client = c->client;

    c->source_output = pa_source_output_new(c->protocol->core, &sdata, 9);
    CHECK_VALIDITY(c->source_output, "Failed to create source_output.");

    l = (size_t) (pa_bytes_per_second(&ss)*RECORD_BUFFER_SECONDS);
    c->output_memblockq = pa_memblockq_new(
            0,
            l,
            0,
            pa_frame_size(&ss),
            1,
            0,
            NULL);
    pa_iochannel_socket_set_sndbuf(c->io, l/RECORD_BUFFER_FRAGMENTS*2);

    c->source_output->push = source_output_push_cb;
    c->source_output->kill = source_output_kill_cb;
    c->source_output->get_latency = source_output_get_latency_cb;
    c->source_output->userdata = c;

    c->state = ESD_STREAMING_DATA;

    c->protocol->n_player++;

    pa_source_output_put(c->source_output);

    return 0;
}

static int esd_proto_get_latency(connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    pa_sink *sink;
    int32_t latency;

    connection_ref(c);
    pa_assert(!data);
    pa_assert(length == 0);

    if (!(sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1)))
        latency = 0;
    else {
        double usec = pa_sink_get_latency(sink);
        latency = (int) ((usec*44100)/1000000);
    }

    latency = PA_MAYBE_INT32_SWAP(c->swap_byte_order, latency);
    connection_write(c, &latency, sizeof(int32_t));
    return 0;
}

static int esd_proto_server_info(connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    int32_t rate = 44100, format = ESD_STEREO|ESD_BITS16;
    int32_t response;
    pa_sink *sink;

    connection_ref(c);
    pa_assert(data);
    pa_assert(length == sizeof(int32_t));

    if ((sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1))) {
        rate = sink->sample_spec.rate;
        format = format_native2esd(&sink->sample_spec);
    }

    connection_write_prepare(c, sizeof(int32_t) * 3);

    response = 0;
    connection_write(c, &response, sizeof(int32_t));
    rate = PA_MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    connection_write(c, &rate, sizeof(int32_t));
    format = PA_MAYBE_INT32_SWAP(c->swap_byte_order, format);
    connection_write(c, &format, sizeof(int32_t));

    return 0;
}

static int esd_proto_all_info(connection *c, esd_proto_t request, const void *data, size_t length) {
    size_t t, k, s;
    connection *conn;
    uint32_t idx = PA_IDXSET_INVALID;
    unsigned nsamples;
    char terminator[sizeof(int32_t)*6+ESD_NAME_MAX];

    connection_ref(c);
    pa_assert(data);
    pa_assert(length == sizeof(int32_t));

    if (esd_proto_server_info(c, request, data, length) < 0)
        return -1;

    k = sizeof(int32_t)*5+ESD_NAME_MAX;
    s = sizeof(int32_t)*6+ESD_NAME_MAX;
    nsamples = c->protocol->core->scache ? pa_idxset_size(c->protocol->core->scache) : 0;
    t = s*(nsamples+1) + k*(c->protocol->n_player+1);

    connection_write_prepare(c, t);

    memset(terminator, 0, sizeof(terminator));

    for (conn = pa_idxset_first(c->protocol->connections, &idx); conn; conn = pa_idxset_next(c->protocol->connections, &idx)) {
        int32_t id, format = ESD_BITS16 | ESD_STEREO, rate = 44100, lvolume = ESD_VOLUME_BASE, rvolume = ESD_VOLUME_BASE;
        char name[ESD_NAME_MAX];

        if (conn->state != ESD_STREAMING_DATA)
            continue;

        pa_assert(t >= k*2+s);

        if (conn->sink_input) {
            pa_cvolume volume = *pa_sink_input_get_volume(conn->sink_input);
            rate = conn->sink_input->sample_spec.rate;
            lvolume = (volume.values[0]*ESD_VOLUME_BASE)/PA_VOLUME_NORM;
            rvolume = (volume.values[1]*ESD_VOLUME_BASE)/PA_VOLUME_NORM;
            format = format_native2esd(&conn->sink_input->sample_spec);
        }

        /* id */
        id = PA_MAYBE_INT32_SWAP(c->swap_byte_order, (int32_t) (conn->index+1));
        connection_write(c, &id, sizeof(int32_t));

        /* name */
        memset(name, 0, ESD_NAME_MAX); /* don't leak old data */
        if (conn->original_name)
            strncpy(name, conn->original_name, ESD_NAME_MAX);
        else if (conn->client && conn->client->name)
            strncpy(name, conn->client->name, ESD_NAME_MAX);
        connection_write(c, name, ESD_NAME_MAX);

        /* rate */
        rate = PA_MAYBE_INT32_SWAP(c->swap_byte_order, rate);
        connection_write(c, &rate, sizeof(int32_t));

        /* left */
        lvolume = PA_MAYBE_INT32_SWAP(c->swap_byte_order, lvolume);
        connection_write(c, &lvolume, sizeof(int32_t));

        /*right*/
        rvolume = PA_MAYBE_INT32_SWAP(c->swap_byte_order, rvolume);
        connection_write(c, &rvolume, sizeof(int32_t));

        /*format*/
        format = PA_MAYBE_INT32_SWAP(c->swap_byte_order, format);
        connection_write(c, &format, sizeof(int32_t));

        t -= k;
    }

    pa_assert(t == s*(nsamples+1)+k);
    t -= k;

    connection_write(c, terminator, k);

    if (nsamples) {
        pa_scache_entry *ce;

        idx = PA_IDXSET_INVALID;
        for (ce = pa_idxset_first(c->protocol->core->scache, &idx); ce; ce = pa_idxset_next(c->protocol->core->scache, &idx)) {
            int32_t id, rate, lvolume, rvolume, format, len;
            char name[ESD_NAME_MAX];

            pa_assert(t >= s*2);

            /* id */
            id = PA_MAYBE_INT32_SWAP(c->swap_byte_order, (int) (ce->index+1));
            connection_write(c, &id, sizeof(int32_t));

            /* name */
            memset(name, 0, ESD_NAME_MAX); /* don't leak old data */
            if (strncmp(ce->name, SCACHE_PREFIX, sizeof(SCACHE_PREFIX)-1) == 0)
                strncpy(name, ce->name+sizeof(SCACHE_PREFIX)-1, ESD_NAME_MAX);
            else
                pa_snprintf(name, ESD_NAME_MAX, "native.%s", ce->name);
            connection_write(c, name, ESD_NAME_MAX);

            /* rate */
            rate = PA_MAYBE_UINT32_SWAP(c->swap_byte_order, ce->sample_spec.rate);
            connection_write(c, &rate, sizeof(int32_t));

            /* left */
            lvolume = PA_MAYBE_UINT32_SWAP(c->swap_byte_order, (ce->volume.values[0]*ESD_VOLUME_BASE)/PA_VOLUME_NORM);
            connection_write(c, &lvolume, sizeof(int32_t));

            /*right*/
            rvolume = PA_MAYBE_UINT32_SWAP(c->swap_byte_order, (ce->volume.values[0]*ESD_VOLUME_BASE)/PA_VOLUME_NORM);
            connection_write(c, &rvolume, sizeof(int32_t));

            /*format*/
            format = PA_MAYBE_INT32_SWAP(c->swap_byte_order, format_native2esd(&ce->sample_spec));
            connection_write(c, &format, sizeof(int32_t));

            /*length*/
            len = PA_MAYBE_INT32_SWAP(c->swap_byte_order, (int) ce->memchunk.length);
            connection_write(c, &len, sizeof(int32_t));

            t -= s;
        }
    }

    pa_assert(t == s);

    connection_write(c, terminator, s);

    return 0;
}

static int esd_proto_stream_pan(connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    int32_t ok;
    uint32_t idx, lvolume, rvolume;
    connection *conn;

    connection_assert_ref(c);
    pa_assert(data);
    pa_assert(length == sizeof(int32_t)*3);

    memcpy(&idx, data, sizeof(uint32_t));
    idx = PA_MAYBE_UINT32_SWAP(c->swap_byte_order, idx) - 1;
    data = (const char*)data + sizeof(uint32_t);

    memcpy(&lvolume, data, sizeof(uint32_t));
    lvolume = PA_MAYBE_UINT32_SWAP(c->swap_byte_order, lvolume);
    data = (const char*)data + sizeof(uint32_t);

    memcpy(&rvolume, data, sizeof(uint32_t));
    rvolume = PA_MAYBE_UINT32_SWAP(c->swap_byte_order, rvolume);
    data = (const char*)data + sizeof(uint32_t);

    if ((conn = pa_idxset_get_by_index(c->protocol->connections, idx)) && conn->sink_input) {
        pa_cvolume volume;
        volume.values[0] = (lvolume*PA_VOLUME_NORM)/ESD_VOLUME_BASE;
        volume.values[1] = (rvolume*PA_VOLUME_NORM)/ESD_VOLUME_BASE;
        volume.channels = 2;
        pa_sink_input_set_volume(conn->sink_input, &volume);
        ok = 1;
    } else
        ok = 0;

    connection_write(c, &ok, sizeof(int32_t));

    return 0;
}

static int esd_proto_sample_cache(connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    pa_sample_spec ss;
    int32_t format, rate, sc_length;
    uint32_t idx;
    char name[ESD_NAME_MAX+sizeof(SCACHE_PREFIX)-1];

    connection_assert_ref(c);
    pa_assert(data);
    pa_assert(length == (ESD_NAME_MAX+3*sizeof(int32_t)));

    memcpy(&format, data, sizeof(int32_t));
    format = PA_MAYBE_INT32_SWAP(c->swap_byte_order, format);
    data = (const char*)data + sizeof(int32_t);

    memcpy(&rate, data, sizeof(int32_t));
    rate = PA_MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    data = (const char*)data + sizeof(int32_t);

    ss.rate = rate;
    format_esd2native(format, c->swap_byte_order, &ss);

    CHECK_VALIDITY(pa_sample_spec_valid(&ss), "Invalid sample specification.");

    memcpy(&sc_length, data, sizeof(int32_t));
    sc_length = PA_MAYBE_INT32_SWAP(c->swap_byte_order, sc_length);
    data = (const char*)data + sizeof(int32_t);

    CHECK_VALIDITY(sc_length <= MAX_CACHE_SAMPLE_SIZE, "Sample too large (%d bytes).", (int)sc_length);

    strcpy(name, SCACHE_PREFIX);
    strncpy(name+sizeof(SCACHE_PREFIX)-1, data, ESD_NAME_MAX);
    name[sizeof(name)-1] = 0;

    CHECK_VALIDITY(pa_utf8_valid(name), "Invalid UTF8 in sample name.");

    pa_assert(!c->scache.memchunk.memblock);
    c->scache.memchunk.memblock = pa_memblock_new(c->protocol->core->mempool, sc_length);
    c->scache.memchunk.index = 0;
    c->scache.memchunk.length = sc_length;
    c->scache.sample_spec = ss;
    pa_assert(!c->scache.name);
    c->scache.name = pa_xstrdup(name);

    c->state = ESD_CACHING_SAMPLE;

    pa_scache_add_item(c->protocol->core, c->scache.name, NULL, NULL, NULL, &idx);

    idx += 1;
    connection_write(c, &idx, sizeof(uint32_t));

    return 0;
}

static int esd_proto_sample_get_id(connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    int32_t ok;
    uint32_t idx;
    char name[ESD_NAME_MAX+sizeof(SCACHE_PREFIX)-1];

    connection_assert_ref(c);
    pa_assert(data);
    pa_assert(length == ESD_NAME_MAX);

    strcpy(name, SCACHE_PREFIX);
    strncpy(name+sizeof(SCACHE_PREFIX)-1, data, ESD_NAME_MAX);
    name[sizeof(name)-1] = 0;

    CHECK_VALIDITY(pa_utf8_valid(name), "Invalid UTF8 in sample name.");

    ok = -1;
    if ((idx = pa_scache_get_id_by_name(c->protocol->core, name)) != PA_IDXSET_INVALID)
        ok = idx + 1;

    connection_write(c, &ok, sizeof(int32_t));

    return 0;
}

static int esd_proto_sample_free_or_play(connection *c, esd_proto_t request, const void *data, size_t length) {
    int32_t ok;
    const char *name;
    uint32_t idx;

    connection_assert_ref(c);
    pa_assert(data);
    pa_assert(length == sizeof(int32_t));

    memcpy(&idx, data, sizeof(uint32_t));
    idx = PA_MAYBE_UINT32_SWAP(c->swap_byte_order, idx) - 1;

    ok = 0;

    if ((name = pa_scache_get_name_by_id(c->protocol->core, idx))) {
        if (request == ESD_PROTO_SAMPLE_PLAY) {
            pa_sink *sink;

            if ((sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1)))
                if (pa_scache_play_item(c->protocol->core, name, sink, PA_VOLUME_NORM) >= 0)
                    ok = idx + 1;
        } else {
            pa_assert(request == ESD_PROTO_SAMPLE_FREE);

            if (pa_scache_remove_item(c->protocol->core, name) >= 0)
                ok = idx + 1;
        }
    }

    connection_write(c, &ok, sizeof(int32_t));

    return 0;
}

static int esd_proto_standby_or_resume(connection *c, PA_GCC_UNUSED esd_proto_t request, PA_GCC_UNUSED const void *data, PA_GCC_UNUSED size_t length) {
    int32_t ok;

    connection_assert_ref(c);

    connection_write_prepare(c, sizeof(int32_t) * 2);

    ok = 1;
    connection_write(c, &ok, sizeof(int32_t));
    connection_write(c, &ok, sizeof(int32_t));

    return 0;
}

/*** client callbacks ***/

static void client_kill_cb(pa_client *c) {
    pa_assert(c);

    connection_unlink(CONNECTION(c->userdata));
}

/*** pa_iochannel callbacks ***/

static int do_read(connection *c) {
    connection_assert_ref(c);

/*     pa_log("READ"); */

    if (c->state == ESD_NEXT_REQUEST) {
        ssize_t r;
        pa_assert(c->read_data_length < sizeof(c->request));

        if ((r = pa_iochannel_read(c->io, ((uint8_t*) &c->request) + c->read_data_length, sizeof(c->request) - c->read_data_length)) <= 0) {
            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        if ((c->read_data_length+= r) >= sizeof(c->request)) {
            struct proto_handler *handler;

            c->request = PA_MAYBE_INT32_SWAP(c->swap_byte_order, c->request);

            if (c->request < ESD_PROTO_CONNECT || c->request > ESD_PROTO_MAX) {
                pa_log("recieved invalid request.");
                return -1;
            }

            handler = proto_map+c->request;

/*             pa_log("executing request #%u", c->request); */

            if (!handler->proc) {
                pa_log("recieved unimplemented request #%u.", c->request);
                return -1;
            }

            if (handler->data_length == 0) {
                c->read_data_length = 0;

                if (handler->proc(c, c->request, NULL, 0) < 0)
                    return -1;

            } else {
                if (c->read_data_alloc < handler->data_length)
                    c->read_data = pa_xrealloc(c->read_data, c->read_data_alloc = handler->data_length);
                pa_assert(c->read_data);

                c->state = ESD_NEEDS_REQDATA;
                c->read_data_length = 0;
            }
        }

    } else if (c->state == ESD_NEEDS_REQDATA) {
        ssize_t r;
        struct proto_handler *handler = proto_map+c->request;

        pa_assert(handler->proc);

        pa_assert(c->read_data && c->read_data_length < handler->data_length);

        if ((r = pa_iochannel_read(c->io, (uint8_t*) c->read_data + c->read_data_length, handler->data_length - c->read_data_length)) <= 0) {
            if (r < 0 && (errno == EINTR || errno == EAGAIN))
                return 0;

            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        if ((c->read_data_length += r) >= handler->data_length) {
            size_t l = c->read_data_length;
            pa_assert(handler->proc);

            c->state = ESD_NEXT_REQUEST;
            c->read_data_length = 0;

            if (handler->proc(c, c->request, c->read_data, l) < 0)
                return -1;
        }
    } else if (c->state == ESD_CACHING_SAMPLE) {
        ssize_t r;
        void *p;

        pa_assert(c->scache.memchunk.memblock);
        pa_assert(c->scache.name);
        pa_assert(c->scache.memchunk.index < c->scache.memchunk.length);

        p = pa_memblock_acquire(c->scache.memchunk.memblock);
        r = pa_iochannel_read(c->io, (uint8_t*) p+c->scache.memchunk.index, c->scache.memchunk.length-c->scache.memchunk.index);
        pa_memblock_release(c->scache.memchunk.memblock);

        if (r <= 0) {
            if (r < 0 && (errno == EINTR || errno == EAGAIN))
                return 0;

            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        c->scache.memchunk.index += r;
        pa_assert(c->scache.memchunk.index <= c->scache.memchunk.length);

        if (c->scache.memchunk.index == c->scache.memchunk.length) {
            uint32_t idx;

            c->scache.memchunk.index = 0;
            pa_scache_add_item(c->protocol->core, c->scache.name, &c->scache.sample_spec, NULL, &c->scache.memchunk, &idx);

            pa_memblock_unref(c->scache.memchunk.memblock);
            c->scache.memchunk.memblock = NULL;
            c->scache.memchunk.index = c->scache.memchunk.length = 0;

            pa_xfree(c->scache.name);
            c->scache.name = NULL;

            c->state = ESD_NEXT_REQUEST;

            idx += 1;
            connection_write(c, &idx, sizeof(uint32_t));
        }

    } else if (c->state == ESD_STREAMING_DATA && c->sink_input) {
        pa_memchunk chunk;
        ssize_t r;
        size_t l;
        void *p;

        pa_assert(c->input_memblockq);

/*         pa_log("STREAMING_DATA"); */

        if (!(l = pa_atomic_load(&c->playback.missing)))
            return 0;

        if (l > c->playback.fragment_size)
            l = c->playback.fragment_size;

        if (c->playback.current_memblock)
            if (pa_memblock_get_length(c->playback.current_memblock) - c->playback.memblock_index < l) {
                pa_memblock_unref(c->playback.current_memblock);
                c->playback.current_memblock = NULL;
                c->playback.memblock_index = 0;
            }

        if (!c->playback.current_memblock) {
            pa_assert_se(c->playback.current_memblock = pa_memblock_new(c->protocol->core->mempool, c->playback.fragment_size*2));
            c->playback.memblock_index = 0;
        }

        p = pa_memblock_acquire(c->playback.current_memblock);
        r = pa_iochannel_read(c->io, (uint8_t*) p+c->playback.memblock_index, l);
        pa_memblock_release(c->playback.current_memblock);

        if (r <= 0) {

            if (r < 0 && (errno == EINTR || errno == EAGAIN))
                return 0;

            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        chunk.memblock = c->playback.current_memblock;
        chunk.index = c->playback.memblock_index;
        chunk.length = r;

        c->playback.memblock_index += r;

        pa_asyncmsgq_post(c->sink_input->sink->asyncmsgq, PA_MSGOBJECT(c->sink_input), SINK_INPUT_MESSAGE_POST_DATA, NULL, 0, &chunk, NULL);
        pa_atomic_sub(&c->playback.missing, r);
    }

    return 0;
}

static int do_write(connection *c) {
    connection_assert_ref(c);

/*     pa_log("WRITE"); */

    if (c->write_data_length) {
        ssize_t r;

        pa_assert(c->write_data_index < c->write_data_length);
        if ((r = pa_iochannel_write(c->io, (uint8_t*) c->write_data+c->write_data_index, c->write_data_length-c->write_data_index)) < 0) {

            if (r < 0 && (errno == EINTR || errno == EAGAIN))
                return 0;

            pa_log("write(): %s", pa_cstrerror(errno));
            return -1;
        }

        if ((c->write_data_index +=r) >= c->write_data_length)
            c->write_data_length = c->write_data_index = 0;

    } else if (c->state == ESD_STREAMING_DATA && c->source_output) {
        pa_memchunk chunk;
        ssize_t r;
        void *p;

        if (pa_memblockq_peek(c->output_memblockq, &chunk) < 0)
            return 0;

        pa_assert(chunk.memblock);
        pa_assert(chunk.length);

        p = pa_memblock_acquire(chunk.memblock);
        r = pa_iochannel_write(c->io, (uint8_t*) p+chunk.index, chunk.length);
        pa_memblock_release(chunk.memblock);

        pa_memblock_unref(chunk.memblock);

        if (r < 0) {

            if (r < 0 && (errno == EINTR || errno == EAGAIN))
                return 0;

            pa_log("write(): %s", pa_cstrerror(errno));
            return -1;
        }

        pa_memblockq_drop(c->output_memblockq, r);
    }

    return 0;
}

static void do_work(connection *c) {
    connection_assert_ref(c);

    c->protocol->core->mainloop->defer_enable(c->defer_event, 0);

    if (c->dead)
        return;

    if (pa_iochannel_is_readable(c->io)) {
        if (do_read(c) < 0)
            goto fail;
    }

    if (c->state == ESD_STREAMING_DATA && c->source_output && pa_iochannel_is_hungup(c->io))
        /* In case we are in capture mode we will never call read()
         * on the socket, hence we need to detect the hangup manually
         * here, instead of simply waiting for read() to return 0. */
        goto fail;

    if (pa_iochannel_is_writable(c->io))
        if (do_write(c) < 0)
            goto fail;

    return;

fail:

    if (c->state == ESD_STREAMING_DATA && c->sink_input) {
        c->dead = TRUE;

        pa_iochannel_free(c->io);
        c->io = NULL;

        pa_asyncmsgq_post(c->sink_input->sink->asyncmsgq, PA_MSGOBJECT(c->sink_input), SINK_INPUT_MESSAGE_DISABLE_PREBUF, NULL, 0, NULL, NULL);
    } else
        connection_unlink(c);
}

static void io_callback(pa_iochannel*io, void *userdata) {
    connection *c = CONNECTION(userdata);

    connection_assert_ref(c);
    pa_assert(io);

    do_work(c);
}

static void defer_callback(pa_mainloop_api*a, pa_defer_event *e, void *userdata) {
    connection *c = CONNECTION(userdata);

    connection_assert_ref(c);
    pa_assert(e);

    do_work(c);
}

static int connection_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    connection *c = CONNECTION(o);
    connection_assert_ref(c);

    switch (code) {
        case CONNECTION_MESSAGE_REQUEST_DATA:
            do_work(c);
            break;

        case CONNECTION_MESSAGE_POST_DATA:
/*             pa_log("got data %u", chunk->length); */
            pa_memblockq_push_align(c->output_memblockq, chunk);
            do_work(c);
            break;

        case CONNECTION_MESSAGE_UNLINK_CONNECTION:
            connection_unlink(c);
            break;
    }

    return 0;
}

/*** sink_input callbacks ***/

/* Called from thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink_input *i = PA_SINK_INPUT(o);
    connection*c;

    pa_sink_input_assert_ref(i);
    c = CONNECTION(i->userdata);
    connection_assert_ref(c);

    switch (code) {

        case SINK_INPUT_MESSAGE_POST_DATA: {
            pa_assert(chunk);

            /* New data from the main loop */
            pa_memblockq_push_align(c->input_memblockq, chunk);

/*             pa_log("got data, %u", pa_memblockq_get_length(c->input_memblockq)); */

            return 0;
        }

        case SINK_INPUT_MESSAGE_DISABLE_PREBUF: {
            pa_memblockq_prebuf_disable(c->input_memblockq);
            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            *r = pa_bytes_to_usec(pa_memblockq_get_length(c->input_memblockq), &c->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
        }

        default:
            return pa_sink_input_process_msg(o, code, userdata, offset, chunk);
    }
}

/* Called from thread context */
static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    connection*c;
    int r;

    pa_assert(i);
    c = CONNECTION(i->userdata);
    connection_assert_ref(c);
    pa_assert(chunk);

    if ((r = pa_memblockq_peek(c->input_memblockq, chunk)) < 0 && c->dead)
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(c), CONNECTION_MESSAGE_UNLINK_CONNECTION, NULL, 0, NULL, NULL);

    return r;
}

/* Called from thread context */
static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    connection*c;
    size_t old, new;

    pa_assert(i);
    c = CONNECTION(i->userdata);
    connection_assert_ref(c);
    pa_assert(length);

    /*     pa_log("DROP"); */

    old = pa_memblockq_missing(c->input_memblockq);
    pa_memblockq_drop(c->input_memblockq, length);
    new = pa_memblockq_missing(c->input_memblockq);

    if (new > old) {
        if (pa_atomic_add(&c->playback.missing, new - old) <= 0)
            pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(c), CONNECTION_MESSAGE_REQUEST_DATA, NULL, 0, NULL, NULL);
    }
}

static void sink_input_kill_cb(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    connection_unlink(CONNECTION(i->userdata));
}

/*** source_output callbacks ***/

/* Called from thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    connection *c;

    pa_assert(o);
    c = CONNECTION(o->userdata);
    pa_assert(c);
    pa_assert(chunk);

    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(c), CONNECTION_MESSAGE_POST_DATA, NULL, 0, chunk, NULL);
}

static void source_output_kill_cb(pa_source_output *o) {
    pa_source_output_assert_ref(o);

    connection_unlink(CONNECTION(o->userdata));
}

static pa_usec_t source_output_get_latency_cb(pa_source_output *o) {
    connection*c;

    pa_assert(o);
    c = CONNECTION(o->userdata);
    pa_assert(c);

    return pa_bytes_to_usec(pa_memblockq_get_length(c->output_memblockq), &c->source_output->sample_spec);
}

/*** socket server callback ***/

static void auth_timeout(pa_mainloop_api*m, pa_time_event *e, const struct timeval *tv, void *userdata) {
    connection *c = CONNECTION(userdata);

    pa_assert(m);
    pa_assert(tv);
    connection_assert_ref(c);
    pa_assert(c->auth_timeout_event == e);

    if (!c->authorized)
        connection_unlink(c);
}

static void on_connection(pa_socket_server*s, pa_iochannel *io, void *userdata) {
    connection *c;
    pa_protocol_esound *p = userdata;
    char cname[256], pname[128];

    pa_assert(s);
    pa_assert(io);
    pa_assert(p);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_msgobject_new(connection);
    c->parent.parent.free = connection_free;
    c->parent.process_msg = connection_process_msg;
    c->protocol = p;
    c->io = io;
    pa_iochannel_set_callback(c->io, io_callback, c);

    pa_iochannel_socket_peer_to_string(io, pname, sizeof(pname));
    pa_snprintf(cname, sizeof(cname), "EsounD client (%s)", pname);
    c->client = pa_client_new(p->core, __FILE__, cname);
    c->client->owner = p->module;
    c->client->kill = client_kill_cb;
    c->client->userdata = c;

    c->authorized = !!p->public;
    c->swap_byte_order = FALSE;
    c->dead = FALSE;

    c->read_data_length = 0;
    c->read_data = pa_xmalloc(c->read_data_alloc = proto_map[ESD_PROTO_CONNECT].data_length);

    c->write_data_length = c->write_data_index = c->write_data_alloc = 0;
    c->write_data = NULL;

    c->state = ESD_NEEDS_REQDATA;
    c->request = ESD_PROTO_CONNECT;

    c->sink_input = NULL;
    c->input_memblockq = NULL;

    c->source_output = NULL;
    c->output_memblockq = NULL;

    c->playback.current_memblock = NULL;
    c->playback.memblock_index = 0;
    c->playback.fragment_size = 0;
    pa_atomic_store(&c->playback.missing, 0);

    c->scache.memchunk.length = c->scache.memchunk.index = 0;
    c->scache.memchunk.memblock = NULL;
    c->scache.name = NULL;

    c->original_name = NULL;

    if (!c->authorized && p->auth_ip_acl && pa_ip_acl_check(p->auth_ip_acl, pa_iochannel_get_recv_fd(io)) > 0) {
        pa_log_info("Client authenticated by IP ACL.");
        c->authorized = TRUE;
    }

    if (!c->authorized) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec += AUTH_TIMEOUT;
        c->auth_timeout_event = p->core->mainloop->time_new(p->core->mainloop, &tv, auth_timeout, c);
    } else
        c->auth_timeout_event = NULL;

    c->defer_event = p->core->mainloop->defer_new(p->core->mainloop, defer_callback, c);
    p->core->mainloop->defer_enable(c->defer_event, 0);

    pa_idxset_put(p->connections, c, &c->index);
}

/*** entry points ***/

pa_protocol_esound* pa_protocol_esound_new(pa_core*core, pa_socket_server *server, pa_module *m, pa_modargs *ma) {
    pa_protocol_esound *p = NULL;
    pa_bool_t public = FALSE;
    const char *acl;

    pa_assert(core);
    pa_assert(server);
    pa_assert(m);
    pa_assert(ma);

    if (pa_modargs_get_value_boolean(ma, "auth-anonymous", &public) < 0) {
        pa_log("auth-anonymous= expects a boolean argument.");
        goto fail;
    }

    p = pa_xnew(pa_protocol_esound, 1);

    if (pa_authkey_load_auto(pa_modargs_get_value(ma, "cookie", DEFAULT_COOKIE_FILE), p->esd_key, sizeof(p->esd_key)) < 0)
        goto fail;

    if ((acl = pa_modargs_get_value(ma, "auth-ip-acl", NULL))) {

        if (!(p->auth_ip_acl = pa_ip_acl_new(acl))) {
            pa_log("Failed to parse IP ACL '%s'", acl);
            goto fail;
        }
    } else
        p->auth_ip_acl = NULL;

    p->core = core;
    p->module = m;
    p->public = public;
    p->server = server;
    pa_socket_server_set_callback(p->server, on_connection, p);
    p->connections = pa_idxset_new(NULL, NULL);

    p->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));
    p->source_name = pa_xstrdup(pa_modargs_get_value(ma, "source", NULL));
    p->n_player = 0;

    return p;

fail:
    pa_xfree(p);
    return NULL;
}

void pa_protocol_esound_free(pa_protocol_esound *p) {
    connection *c;
    pa_assert(p);

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_unlink(c);
    pa_idxset_free(p->connections, NULL, NULL);

    pa_socket_server_unref(p->server);

    if (p->auth_ip_acl)
        pa_ip_acl_free(p->auth_ip_acl);

    pa_xfree(p->sink_name);
    pa_xfree(p->source_name);

    pa_xfree(p);
}
