/* $Id$ */

/***
  This file is part of PulseAudio.
 
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
#include <assert.h>
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

#define MAX_CACHE_SAMPLE_SIZE (1024000)

#define SCACHE_PREFIX "esound."

/* This is heavily based on esound's code */

struct connection {
    uint32_t index;
    int dead;
    pa_protocol_esound *protocol;
    pa_iochannel *io;
    pa_client *client;
    int authorized, swap_byte_order;
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
    } playback;

    struct {
        pa_memchunk memchunk;
        char *name;
        pa_sample_spec sample_spec;
    } scache;

    pa_time_event *auth_timeout_event;
};

struct pa_protocol_esound {
    int public;
    pa_module *module;
    pa_core *core;
    pa_socket_server *server;
    pa_idxset *connections;
    char *sink_name, *source_name;
    unsigned n_player;
    uint8_t esd_key[ESD_KEY_LEN];
    pa_ip_acl *auth_ip_acl;
};

typedef struct proto_handler {
    size_t data_length;
    int (*proc)(struct connection *c, esd_proto_t request, const void *data, size_t length);
    const char *description;
} esd_proto_handler_info_t;

static void sink_input_drop_cb(pa_sink_input *i, const pa_memchunk *chunk, size_t length);
static int sink_input_peek_cb(pa_sink_input *i, pa_memchunk *chunk);
static void sink_input_kill_cb(pa_sink_input *i);
static pa_usec_t sink_input_get_latency_cb(pa_sink_input *i);
static pa_usec_t source_output_get_latency_cb(pa_source_output *o);

static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk);
static void source_output_kill_cb(pa_source_output *o);

static int esd_proto_connect(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_stream_play(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_stream_record(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_get_latency(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_server_info(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_all_info(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_stream_pan(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_sample_cache(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_sample_free_or_play(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_sample_get_id(struct connection *c, esd_proto_t request, const void *data, size_t length);
static int esd_proto_standby_or_resume(struct connection *c, esd_proto_t request, const void *data, size_t length);

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

static void connection_free(struct connection *c) {
    assert(c);
    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);

    if (c->state == ESD_STREAMING_DATA)
        c->protocol->n_player--;
    
    pa_client_free(c->client);

    if (c->sink_input) {
        pa_sink_input_disconnect(c->sink_input);
        pa_sink_input_unref(c->sink_input);
    }
    
    if (c->source_output) {
        pa_source_output_disconnect(c->source_output);
        pa_source_output_unref(c->source_output);
    }
    
    if (c->input_memblockq)
        pa_memblockq_free(c->input_memblockq);
    if (c->output_memblockq)
        pa_memblockq_free(c->output_memblockq);

    if (c->playback.current_memblock)
        pa_memblock_unref(c->playback.current_memblock);
    
    pa_xfree(c->read_data);
    pa_xfree(c->write_data);

    if (c->io)
        pa_iochannel_free(c->io);
    
    if (c->defer_event)
        c->protocol->core->mainloop->defer_free(c->defer_event);

    if (c->scache.memchunk.memblock)
        pa_memblock_unref(c->scache.memchunk.memblock);
    pa_xfree(c->scache.name);

    if (c->auth_timeout_event)
        c->protocol->core->mainloop->time_free(c->auth_timeout_event);

    pa_xfree(c->original_name);
    pa_xfree(c);
}

static void connection_write_prepare(struct connection *c, size_t length) {
    size_t t;
    assert(c);

    t = c->write_data_length+length;

    if (c->write_data_alloc < t)
        c->write_data = pa_xrealloc(c->write_data, c->write_data_alloc = t);

    assert(c->write_data);
}

static void connection_write(struct connection *c, const void *data, size_t length) {
    size_t i;
    assert(c);

    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);
    c->protocol->core->mainloop->defer_enable(c->defer_event, 1);

    connection_write_prepare(c, length);

    assert(c->write_data);

    i = c->write_data_length;
    c->write_data_length += length;
    
    memcpy((char*)c->write_data + i, data, length);
}

static void format_esd2native(int format, int swap_bytes, pa_sample_spec *ss) {
    assert(ss);

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

#define CHECK_VALIDITY(expression, string) do { \
    if (!(expression)) { \
        pa_log_warn(__FILE__ ": " string); \
        return -1; \
    } \
} while(0);

/*** esound commands ***/

static int esd_proto_connect(struct connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    uint32_t ekey;
    int ok;

    assert(length == (ESD_KEY_LEN + sizeof(uint32_t)));

    if (!c->authorized) {
        if (memcmp(data, c->protocol->esd_key, ESD_KEY_LEN) != 0) {
            pa_log("kicked client with invalid authorization key.");
            return -1;
        }

        c->authorized = 1;
        if (c->auth_timeout_event) {
            c->protocol->core->mainloop->time_free(c->auth_timeout_event);
            c->auth_timeout_event = NULL;
        }
    }

    data = (const char*)data + ESD_KEY_LEN;

    memcpy(&ekey, data, sizeof(uint32_t));
    if (ekey == ESD_ENDIAN_KEY)
        c->swap_byte_order = 0;
    else if (ekey == ESD_SWAP_ENDIAN_KEY)
        c->swap_byte_order = 1;
    else {
        pa_log("client sent invalid endian key");
        return -1;
    }

    ok = 1;
    connection_write(c, &ok, sizeof(int));
    return 0;
}

static int esd_proto_stream_play(struct connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    char name[ESD_NAME_MAX], *utf8_name;
    int32_t format, rate;
    pa_sample_spec ss;
    size_t l;
    pa_sink *sink = NULL;
    pa_sink_input_new_data sdata;

    assert(c && length == (sizeof(int32_t)*2+ESD_NAME_MAX));
    
    memcpy(&format, data, sizeof(int32_t));
    format = MAYBE_INT32_SWAP(c->swap_byte_order, format);
    data = (const char*)data + sizeof(int32_t);

    memcpy(&rate, data, sizeof(int32_t));
    rate = MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    data = (const char*)data + sizeof(int32_t);

    ss.rate = rate;
    format_esd2native(format, c->swap_byte_order, &ss);

    CHECK_VALIDITY(pa_sample_spec_valid(&ss), "Invalid sample specification");

    if (c->protocol->sink_name) {
        sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1);
        CHECK_VALIDITY(sink, "No such sink");
    }

    strncpy(name, data, sizeof(name));
    name[sizeof(name)-1] = 0;

    utf8_name = pa_utf8_filter(name);
    pa_client_set_name(c->client, utf8_name);
    pa_xfree(utf8_name);
    
    c->original_name = pa_xstrdup(name);

    assert(!c->sink_input && !c->input_memblockq);

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
    c->playback.fragment_size = l/10;

    c->sink_input->peek = sink_input_peek_cb;
    c->sink_input->drop = sink_input_drop_cb;
    c->sink_input->kill = sink_input_kill_cb;
    c->sink_input->get_latency = sink_input_get_latency_cb;
    c->sink_input->userdata = c;

    c->state = ESD_STREAMING_DATA;

    c->protocol->n_player++;
    
    return 0;
}

static int esd_proto_stream_record(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    char name[ESD_NAME_MAX], *utf8_name;
    int32_t format, rate;
    pa_source *source = NULL;
    pa_sample_spec ss;
    size_t l;
    pa_source_output_new_data sdata;

    assert(c && length == (sizeof(int32_t)*2+ESD_NAME_MAX));
    
    memcpy(&format, data, sizeof(int32_t));
    format = MAYBE_INT32_SWAP(c->swap_byte_order, format);
    data = (const char*)data + sizeof(int32_t);

    memcpy(&rate, data, sizeof(int32_t));
    rate = MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    data = (const char*)data + sizeof(int32_t);

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
        assert(request == ESD_PROTO_STREAM_REC);

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

    assert(!c->output_memblockq && !c->source_output);

    pa_source_output_new_data_init(&sdata);
    sdata.source = source;
    sdata.driver = __FILE__;
    sdata.name = c->client->name;
    pa_source_output_new_data_set_sample_spec(&sdata, &ss);
    sdata.module = c->protocol->module;
    sdata.client = c->client;
    
    c->source_output = pa_source_output_new(c->protocol->core, &sdata, 9);
    CHECK_VALIDITY(c->sink_input, "Failed to create source_output.");

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
    
    return 0;
}

static int esd_proto_get_latency(struct connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    pa_sink *sink;
    int32_t latency;

    assert(c && !data && length == 0);

    if (!(sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1)))
        latency = 0;
    else {
        double usec = pa_sink_get_latency(sink);
        latency = (int) ((usec*44100)/1000000);
    }
    
    latency = MAYBE_INT32_SWAP(c->swap_byte_order, latency);
    connection_write(c, &latency, sizeof(int32_t));
    return 0;
}

static int esd_proto_server_info(struct connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    int32_t rate = 44100, format = ESD_STEREO|ESD_BITS16;
    int32_t response;
    pa_sink *sink;

    assert(c && data && length == sizeof(int32_t));

    if ((sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1))) {
        rate = sink->sample_spec.rate;
        format = format_native2esd(&sink->sample_spec);
    }

    connection_write_prepare(c, sizeof(int32_t) * 3);

    response = 0;
    connection_write(c, &response, sizeof(int32_t));
    rate = MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    connection_write(c, &rate, sizeof(int32_t));
    format = MAYBE_INT32_SWAP(c->swap_byte_order, format);
    connection_write(c, &format, sizeof(int32_t));

    return 0;
}

static int esd_proto_all_info(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    size_t t, k, s;
    struct connection *conn;
    uint32_t idx = PA_IDXSET_INVALID;
    unsigned nsamples;
    char terminator[sizeof(int32_t)*6+ESD_NAME_MAX];

    assert(c && data && length == sizeof(int32_t));
    
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

        assert(t >= k*2+s);
        
        if (conn->sink_input) {
            pa_cvolume volume = *pa_sink_input_get_volume(conn->sink_input);
            rate = conn->sink_input->sample_spec.rate;
            lvolume = (volume.values[0]*ESD_VOLUME_BASE)/PA_VOLUME_NORM;
            rvolume = (volume.values[1]*ESD_VOLUME_BASE)/PA_VOLUME_NORM;
            format = format_native2esd(&conn->sink_input->sample_spec);
        }
        
        /* id */
        id = MAYBE_INT32_SWAP(c->swap_byte_order, (int32_t) (conn->index+1));
        connection_write(c, &id, sizeof(int32_t));

        /* name */
        memset(name, 0, ESD_NAME_MAX); /* don't leak old data */
        if (conn->original_name)
            strncpy(name, conn->original_name, ESD_NAME_MAX);
        else if (conn->client && conn->client->name)
            strncpy(name, conn->client->name, ESD_NAME_MAX);
        connection_write(c, name, ESD_NAME_MAX);

        /* rate */
        rate = MAYBE_INT32_SWAP(c->swap_byte_order, rate);
        connection_write(c, &rate, sizeof(int32_t));

        /* left */
        lvolume = MAYBE_INT32_SWAP(c->swap_byte_order, lvolume);
        connection_write(c, &lvolume, sizeof(int32_t));

        /*right*/
        rvolume = MAYBE_INT32_SWAP(c->swap_byte_order, rvolume);
        connection_write(c, &rvolume, sizeof(int32_t));

        /*format*/
        format = MAYBE_INT32_SWAP(c->swap_byte_order, format);
        connection_write(c, &format, sizeof(int32_t));

        t -= k;
    }

    assert(t == s*(nsamples+1)+k);
    t -= k;

    connection_write(c, terminator, k);

    if (nsamples) {
        pa_scache_entry *ce;
        
        idx = PA_IDXSET_INVALID;
        for (ce = pa_idxset_first(c->protocol->core->scache, &idx); ce; ce = pa_idxset_next(c->protocol->core->scache, &idx)) {
            int32_t id, rate, lvolume, rvolume, format, len;
            char name[ESD_NAME_MAX];

            assert(t >= s*2);

            /* id */
            id = MAYBE_INT32_SWAP(c->swap_byte_order, (int) (ce->index+1));
            connection_write(c, &id, sizeof(int32_t));
            
            /* name */
            memset(name, 0, ESD_NAME_MAX); /* don't leak old data */
            if (strncmp(ce->name, SCACHE_PREFIX, sizeof(SCACHE_PREFIX)-1) == 0)
                strncpy(name, ce->name+sizeof(SCACHE_PREFIX)-1, ESD_NAME_MAX);
            else
                snprintf(name, ESD_NAME_MAX, "native.%s", ce->name);
            connection_write(c, name, ESD_NAME_MAX);
            
            /* rate */
            rate = MAYBE_UINT32_SWAP(c->swap_byte_order, ce->sample_spec.rate);
            connection_write(c, &rate, sizeof(int32_t));
            
            /* left */
            lvolume = MAYBE_UINT32_SWAP(c->swap_byte_order, (ce->volume.values[0]*ESD_VOLUME_BASE)/PA_VOLUME_NORM);
            connection_write(c, &lvolume, sizeof(int32_t));
            
            /*right*/
            rvolume = MAYBE_UINT32_SWAP(c->swap_byte_order, (ce->volume.values[0]*ESD_VOLUME_BASE)/PA_VOLUME_NORM);
            connection_write(c, &rvolume, sizeof(int32_t));
            
            /*format*/
            format = MAYBE_INT32_SWAP(c->swap_byte_order, format_native2esd(&ce->sample_spec));
            connection_write(c, &format, sizeof(int32_t));

            /*length*/
            len = MAYBE_INT32_SWAP(c->swap_byte_order, (int) ce->memchunk.length);
            connection_write(c, &len, sizeof(int32_t));

            t -= s;
        }
    }

    assert(t == s);

    connection_write(c, terminator, s);

    return 0;
}

static int esd_proto_stream_pan(struct connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    int32_t ok;
    uint32_t idx, lvolume, rvolume;
    struct connection *conn;

    assert(c && data && length == sizeof(int32_t)*3);
    
    memcpy(&idx, data, sizeof(uint32_t));
    idx = MAYBE_UINT32_SWAP(c->swap_byte_order, idx) - 1;
    data = (const char*)data + sizeof(uint32_t);

    memcpy(&lvolume, data, sizeof(uint32_t));
    lvolume = MAYBE_UINT32_SWAP(c->swap_byte_order, lvolume);
    data = (const char*)data + sizeof(uint32_t);

    memcpy(&rvolume, data, sizeof(uint32_t));
    rvolume = MAYBE_UINT32_SWAP(c->swap_byte_order, rvolume);
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

static int esd_proto_sample_cache(struct connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    pa_sample_spec ss;
    int32_t format, rate, sc_length;
    uint32_t idx;
    char name[ESD_NAME_MAX+sizeof(SCACHE_PREFIX)-1];

    assert(c && data && length == (ESD_NAME_MAX+3*sizeof(int32_t)));

    memcpy(&format, data, sizeof(int32_t));
    format = MAYBE_INT32_SWAP(c->swap_byte_order, format);
    data = (const char*)data + sizeof(int32_t);

    memcpy(&rate, data, sizeof(int32_t));
    rate = MAYBE_INT32_SWAP(c->swap_byte_order, rate);
    data = (const char*)data + sizeof(int32_t);
    
    ss.rate = rate;
    format_esd2native(format, c->swap_byte_order, &ss);

    CHECK_VALIDITY(pa_sample_spec_valid(&ss), "Invalid sample specification.");

    memcpy(&sc_length, data, sizeof(int32_t));
    sc_length = MAYBE_INT32_SWAP(c->swap_byte_order, sc_length);
    data = (const char*)data + sizeof(int32_t);

    CHECK_VALIDITY(sc_length <= MAX_CACHE_SAMPLE_SIZE, "Sample too large.");

    strcpy(name, SCACHE_PREFIX);
    strncpy(name+sizeof(SCACHE_PREFIX)-1, data, ESD_NAME_MAX);
    name[sizeof(name)-1] = 0;

    CHECK_VALIDITY(pa_utf8_valid(name), "Invalid UTF8 in sample name.");
    
    assert(!c->scache.memchunk.memblock);
    c->scache.memchunk.memblock = pa_memblock_new(c->protocol->core->mempool, sc_length);
    c->scache.memchunk.index = 0;
    c->scache.memchunk.length = sc_length;
    c->scache.sample_spec = ss;
    assert(!c->scache.name);
    c->scache.name = pa_xstrdup(name);
    
    c->state = ESD_CACHING_SAMPLE;

    pa_scache_add_item(c->protocol->core, c->scache.name, NULL, NULL, NULL, &idx);

    idx += 1;
    connection_write(c, &idx, sizeof(uint32_t));
    
    return 0;
}

static int esd_proto_sample_get_id(struct connection *c, PA_GCC_UNUSED esd_proto_t request, const void *data, size_t length) {
    int32_t ok;
    uint32_t idx;
    char name[ESD_NAME_MAX+sizeof(SCACHE_PREFIX)-1];

    assert(c && data && length == ESD_NAME_MAX);

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

static int esd_proto_sample_free_or_play(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    int32_t ok;
    const char *name;
    uint32_t idx;

    assert(c && data && length == sizeof(int32_t));

    memcpy(&idx, data, sizeof(uint32_t));
    idx = MAYBE_UINT32_SWAP(c->swap_byte_order, idx) - 1;

    ok = 0;
    
    if ((name = pa_scache_get_name_by_id(c->protocol->core, idx))) {
        if (request == ESD_PROTO_SAMPLE_PLAY) {
            pa_sink *sink;
        
            if ((sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1)))
                if (pa_scache_play_item(c->protocol->core, name, sink, PA_VOLUME_NORM) >= 0)
                    ok = idx + 1;
        } else {
            assert(request == ESD_PROTO_SAMPLE_FREE);

            if (pa_scache_remove_item(c->protocol->core, name) >= 0)
                ok = idx + 1;
        }
    }
    
    connection_write(c, &ok, sizeof(int32_t));

    return 0;
}

static int esd_proto_standby_or_resume(struct connection *c, PA_GCC_UNUSED esd_proto_t request, PA_GCC_UNUSED const void *data, PA_GCC_UNUSED size_t length) {
    int32_t ok;

    connection_write_prepare(c, sizeof(int32_t) * 2);

    ok = 1;
    connection_write(c, &ok, sizeof(int32_t));
    connection_write(c, &ok, sizeof(int32_t));

    return 0;
}

/*** client callbacks ***/

static void client_kill_cb(pa_client *c) {
    assert(c && c->userdata);
    connection_free(c->userdata);
}

/*** pa_iochannel callbacks ***/

static int do_read(struct connection *c) {
    assert(c && c->io);

/*      pa_log("READ");  */
    
    if (c->state == ESD_NEXT_REQUEST) {
        ssize_t r;
        assert(c->read_data_length < sizeof(c->request));

        if ((r = pa_iochannel_read(c->io, ((uint8_t*) &c->request) + c->read_data_length, sizeof(c->request) - c->read_data_length)) <= 0) {
            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        if ((c->read_data_length+= r) >= sizeof(c->request)) {
            struct proto_handler *handler;
            
            c->request = MAYBE_INT32_SWAP(c->swap_byte_order, c->request);

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
                assert(c->read_data);
                
                c->state = ESD_NEEDS_REQDATA;
                c->read_data_length = 0;
            }
        }

    } else if (c->state == ESD_NEEDS_REQDATA) {
        ssize_t r;
        struct proto_handler *handler = proto_map+c->request;

        assert(handler->proc);
        
        assert(c->read_data && c->read_data_length < handler->data_length);

        if ((r = pa_iochannel_read(c->io, (uint8_t*) c->read_data + c->read_data_length, handler->data_length - c->read_data_length)) <= 0) {
            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        if ((c->read_data_length += r) >= handler->data_length) {
            size_t l = c->read_data_length;
            assert(handler->proc);

            c->state = ESD_NEXT_REQUEST;
            c->read_data_length = 0;
            
            if (handler->proc(c, c->request, c->read_data, l) < 0)
                return -1;
        }
    } else if (c->state == ESD_CACHING_SAMPLE) {
        ssize_t r;
        void *p;

        assert(c->scache.memchunk.memblock);
        assert(c->scache.name);
        assert(c->scache.memchunk.index < c->scache.memchunk.length);

        p = pa_memblock_acquire(c->scache.memchunk.memblock);
        
        if ((r = pa_iochannel_read(c->io, (uint8_t*) p+c->scache.memchunk.index, c->scache.memchunk.length-c->scache.memchunk.index)) <= 0) {
            pa_memblock_release(c->scache.memchunk.memblock);
            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        pa_memblock_release(c->scache.memchunk.memblock);
        
        c->scache.memchunk.index += r;
        assert(c->scache.memchunk.index <= c->scache.memchunk.length);
        
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

        assert(c->input_memblockq);

/*         pa_log("STREAMING_DATA"); */

        if (!(l = pa_memblockq_missing(c->input_memblockq)))
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
            c->playback.current_memblock = pa_memblock_new(c->protocol->core->mempool, c->playback.fragment_size*2);
            assert(c->playback.current_memblock);
            assert(pa_memblock_get_length(c->playback.current_memblock) >= l);
            c->playback.memblock_index = 0;
        }

        p = pa_memblock_acquire(c->playback.current_memblock);
        
        if ((r = pa_iochannel_read(c->io, (uint8_t*) p+c->playback.memblock_index, l)) <= 0) {
            pa_memblock_release(c->playback.current_memblock);
            pa_log_debug("read(): %s", r < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        pa_memblock_release(c->playback.current_memblock);

        chunk.memblock = c->playback.current_memblock;
        chunk.index = c->playback.memblock_index;
        chunk.length = r;
        assert(chunk.memblock);

        c->playback.memblock_index += r;
        
        assert(c->input_memblockq);
        pa_memblockq_push_align(c->input_memblockq, &chunk);
        assert(c->sink_input);
        pa_sink_notify(c->sink_input->sink);
    }
    
    return 0;
}

static int do_write(struct connection *c) {
    assert(c && c->io);

/*     pa_log("WRITE"); */
    
    if (c->write_data_length) {
        ssize_t r;
        
        assert(c->write_data_index < c->write_data_length);
        if ((r = pa_iochannel_write(c->io, (uint8_t*) c->write_data+c->write_data_index, c->write_data_length-c->write_data_index)) < 0) {
            pa_log("write(): %s", pa_cstrerror(errno));
            return -1;
        }
        
        if ((c->write_data_index +=r) >= c->write_data_length)
            c->write_data_length = c->write_data_index = 0;
        
    } else if (c->state == ESD_STREAMING_DATA && c->source_output) {
        pa_memchunk chunk;
        ssize_t r;
        void *p;

        assert(c->output_memblockq);
        if (pa_memblockq_peek(c->output_memblockq, &chunk) < 0)
            return 0;
        
        assert(chunk.memblock);
        assert(chunk.length);

        p = pa_memblock_acquire(chunk.memblock);
        
        if ((r = pa_iochannel_write(c->io, (uint8_t*) p+chunk.index, chunk.length)) < 0) {
            pa_memblock_release(chunk.memblock);
            pa_memblock_unref(chunk.memblock);
            pa_log("write(): %s", pa_cstrerror(errno));
            return -1;
        }

        pa_memblock_release(chunk.memblock);

        pa_memblockq_drop(c->output_memblockq, &chunk, r);
        pa_memblock_unref(chunk.memblock);

        pa_source_notify(c->source_output->source);
    }
    
    return 0;
}

static void do_work(struct connection *c) {
    assert(c);

    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);
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
        c->dead = 1;

        pa_iochannel_free(c->io);
        c->io = NULL;

        pa_memblockq_prebuf_disable(c->input_memblockq);
        pa_sink_notify(c->sink_input->sink);
    } else
        connection_free(c);
}

static void io_callback(pa_iochannel*io, void *userdata) {
    struct connection *c = userdata;
    assert(io && c && c->io == io);

    do_work(c);
}

/*** defer callback ***/

static void defer_callback(pa_mainloop_api*a, pa_defer_event *e, void *userdata) {
    struct connection *c = userdata;
    assert(a && c && c->defer_event == e);

/*     pa_log("DEFER"); */
    
    do_work(c);
}

/*** sink_input callbacks ***/

static int sink_input_peek_cb(pa_sink_input *i, pa_memchunk *chunk) {
    struct connection*c;
    assert(i && i->userdata && chunk);
    c = i->userdata;
    
    if (pa_memblockq_peek(c->input_memblockq, chunk) < 0) {

        if (c->dead)
            connection_free(c);
        
        return -1;
    }

    return 0;
}

static void sink_input_drop_cb(pa_sink_input *i, const pa_memchunk *chunk, size_t length) {
    struct connection*c = i->userdata;
    assert(i && c && length);

/*     pa_log("DROP"); */
    
    pa_memblockq_drop(c->input_memblockq, chunk, length);

    /* do something */
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);

    if (!c->dead)
        c->protocol->core->mainloop->defer_enable(c->defer_event, 1);

/*     assert(pa_memblockq_get_length(c->input_memblockq) > 2048); */
}

static void sink_input_kill_cb(pa_sink_input *i) {
    assert(i && i->userdata);
    connection_free((struct connection *) i->userdata);
}

static pa_usec_t sink_input_get_latency_cb(pa_sink_input *i) {
    struct connection*c = i->userdata;
    assert(i && c);
    return pa_bytes_to_usec(pa_memblockq_get_length(c->input_memblockq), &c->sink_input->sample_spec);
}

/*** source_output callbacks ***/

static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct connection *c = o->userdata;
    assert(o && c && chunk);

    pa_memblockq_push(c->output_memblockq, chunk);

    /* do something */
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);

    if (!c->dead)
        c->protocol->core->mainloop->defer_enable(c->defer_event, 1);
}

static void source_output_kill_cb(pa_source_output *o) {
    assert(o && o->userdata);
    connection_free((struct connection *) o->userdata);
}

static pa_usec_t source_output_get_latency_cb(pa_source_output *o) {
    struct connection*c = o->userdata;
    assert(o && c);
    return pa_bytes_to_usec(pa_memblockq_get_length(c->output_memblockq), &c->source_output->sample_spec);
}

/*** socket server callback ***/

static void auth_timeout(pa_mainloop_api*m, pa_time_event *e, const struct timeval *tv, void *userdata) {
    struct connection *c = userdata;
    assert(m && tv && c && c->auth_timeout_event == e);

    if (!c->authorized)
        connection_free(c);
}

static void on_connection(pa_socket_server*s, pa_iochannel *io, void *userdata) {
    struct connection *c;
    pa_protocol_esound *p = userdata;
    char cname[256], pname[128];
    assert(s && io && p);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }
    
    c = pa_xnew(struct connection, 1);
    c->protocol = p;
    c->io = io;
    pa_iochannel_set_callback(c->io, io_callback, c);

    pa_iochannel_socket_peer_to_string(io, pname, sizeof(pname));
    snprintf(cname, sizeof(cname), "EsounD client (%s)", pname);
    assert(p->core);
    c->client = pa_client_new(p->core, __FILE__, cname);
    assert(c->client);
    c->client->owner = p->module;
    c->client->kill = client_kill_cb;
    c->client->userdata = c;
    
    c->authorized = !!p->public;
    c->swap_byte_order = 0;
    c->dead = 0;

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

    c->scache.memchunk.length = c->scache.memchunk.index = 0;
    c->scache.memchunk.memblock = NULL;
    c->scache.name = NULL;

    c->original_name = NULL;

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
    
    c->defer_event = p->core->mainloop->defer_new(p->core->mainloop, defer_callback, c);
    assert(c->defer_event);
    p->core->mainloop->defer_enable(c->defer_event, 0);

    pa_idxset_put(p->connections, c, &c->index);
}

/*** entry points ***/

pa_protocol_esound* pa_protocol_esound_new(pa_core*core, pa_socket_server *server, pa_module *m, pa_modargs *ma) {
    pa_protocol_esound *p;
    int public = 0;
    const char *acl;
    
    assert(core);
    assert(server);
    assert(m);
    assert(ma);

    p = pa_xnew(pa_protocol_esound, 1);

    if (pa_modargs_get_value_boolean(ma, "auth-anonymous", &public) < 0) {
        pa_log("auth-anonymous= expects a boolean argument.");
        goto fail;
    }

    if (pa_authkey_load_auto(pa_modargs_get_value(ma, "cookie", DEFAULT_COOKIE_FILE), p->esd_key, sizeof(p->esd_key)) < 0)
        goto fail;

    if ((acl = pa_modargs_get_value(ma, "auth-ip-acl", NULL))) {

        if (!(p->auth_ip_acl = pa_ip_acl_new(acl))) {
            pa_log("Failed to parse IP ACL '%s'", acl);
            goto fail;
        }
    } else
        p->auth_ip_acl = NULL;
    
    p->module = m;
    p->public = public;
    p->server = server;
    pa_socket_server_set_callback(p->server, on_connection, p);
    p->core = core;
    p->connections = pa_idxset_new(NULL, NULL);
    assert(p->connections);

    p->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));
    p->source_name = pa_xstrdup(pa_modargs_get_value(ma, "source", NULL));
    p->n_player = 0;

    return p;

fail:
    pa_xfree(p);
    return NULL;
}

void pa_protocol_esound_free(pa_protocol_esound *p) {
    struct connection *c;
    assert(p);

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_free(c);

    pa_idxset_free(p->connections, NULL, NULL);
    pa_socket_server_unref(p->server);

    if (p->auth_ip_acl)
        pa_ip_acl_free(p->auth_ip_acl);

    pa_xfree(p);
}
