/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
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

#include "protocol-esound.h"
#include "esound.h"
#include "memblock.h"
#include "client.h"
#include "sink-input.h"
#include "sink.h"
#include "source-output.h"
#include "source.h"
#include "sample.h"
#include "scache.h"
#include "sample-util.h"
#include "authkey.h"
#include "namereg.h"
#include "xmalloc.h"
#include "log.h"

/* Don't accept more connection than this */
#define MAX_CONNECTIONS 10

/* Kick a client if it doesn't authenticate within this time */
#define AUTH_TIMEOUT 5

#define DEFAULT_COOKIE_FILE ".esd_auth"

#define PLAYBACK_BUFFER_SECONDS (.5)
#define PLAYBACK_BUFFER_FRAGMENTS (10)
#define RECORD_BUFFER_SECONDS (5)
#define RECORD_BUFFER_FRAGMENTS (100)

#define MAX_CACHE_SAMPLE_SIZE (1024000)

#define SCACHE_PREFIX "esound."

#define PA_TYPEID_ESOUND PA_TYPEID_MAKE('E', 'S', 'D', 'P')

/* This is heavily based on esound's code */

struct connection {
    uint32_t index;
    int dead;
    struct pa_protocol_esound *protocol;
    struct pa_iochannel *io;
    struct pa_client *client;
    int authorized, swap_byte_order;
    void *write_data;
    size_t write_data_alloc, write_data_index, write_data_length;
    void *read_data;
    size_t read_data_alloc, read_data_length;
    esd_proto_t request;
    esd_client_state_t state;
    struct pa_sink_input *sink_input;
    struct pa_source_output *source_output;
    struct pa_memblockq *input_memblockq, *output_memblockq;
    struct pa_defer_event *defer_event;
    
    struct {
        struct pa_memblock *current_memblock;
        size_t memblock_index, fragment_size;
    } playback;

    struct {
        struct pa_memchunk memchunk;
        char *name;
        struct pa_sample_spec sample_spec;
    } scache;

    struct pa_time_event *auth_timeout_event;
};

struct pa_protocol_esound {
    int public;
    struct pa_module *module;
    struct pa_core *core;
    struct pa_socket_server *server;
    struct pa_idxset *connections;
    char *sink_name, *source_name;
    unsigned n_player;
    uint8_t esd_key[ESD_KEY_LEN];
};

typedef struct proto_handler {
    size_t data_length;
    int (*proc)(struct connection *c, esd_proto_t request, const void *data, size_t length);
    const char *description;
} esd_proto_handler_info_t;

static void sink_input_drop_cb(struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length);
static int sink_input_peek_cb(struct pa_sink_input *i, struct pa_memchunk *chunk);
static void sink_input_kill_cb(struct pa_sink_input *i);
static pa_usec_t sink_input_get_latency_cb(struct pa_sink_input *i);
static pa_usec_t source_output_get_latency_cb(struct pa_source_output *o);

static void source_output_push_cb(struct pa_source_output *o, const struct pa_memchunk *chunk);
static void source_output_kill_cb(struct pa_source_output *o);

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
    
    pa_xfree(c);
}

static void* connection_write(struct connection *c, size_t length) {
    size_t t, i;
    assert(c);

    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);
    c->protocol->core->mainloop->defer_enable(c->defer_event, 1);

    t = c->write_data_length+length;
    
    if (c->write_data_alloc < t)
        c->write_data = pa_xrealloc(c->write_data, c->write_data_alloc = t);

    assert(c->write_data);

    i = c->write_data_length;
    c->write_data_length += length;
    
    return (uint8_t*) c->write_data+i;
}

static void format_esd2native(int format, struct pa_sample_spec *ss) {
    assert(ss);

    ss->channels = ((format & ESD_MASK_CHAN) == ESD_STEREO) ? 2 : 1;
    ss->format = ((format & ESD_MASK_BITS) == ESD_BITS16) ? PA_SAMPLE_S16NE : PA_SAMPLE_U8;
}

static int format_native2esd(struct pa_sample_spec *ss) {
    int format = 0;
    
    format = (ss->format == PA_SAMPLE_U8) ? ESD_BITS8 : ESD_BITS16;
    format |= (ss->channels >= 2) ? ESD_STEREO : ESD_MONO;

    return format;
}

/*** esound commands ***/

static int esd_proto_connect(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    uint32_t ekey;
    int *ok;
    assert(length == (ESD_KEY_LEN + sizeof(uint32_t)));

    if (!c->authorized) {
        if (memcmp(data, c->protocol->esd_key, ESD_KEY_LEN) != 0) {
            pa_log(__FILE__": kicked client with invalid authorization key.\n");
            return -1;
        }

        c->authorized = 1;
        if (c->auth_timeout_event) {
            c->protocol->core->mainloop->time_free(c->auth_timeout_event);
            c->auth_timeout_event = NULL;
        }
    }
    
    ekey = *(uint32_t*)((uint8_t*) data+ESD_KEY_LEN);
    if (ekey == ESD_ENDIAN_KEY)
        c->swap_byte_order = 0;
    else if (ekey == ESD_SWAP_ENDIAN_KEY)
        c->swap_byte_order = 1;
    else {
        pa_log(__FILE__": client sent invalid endian key\n");
        return -1;
    }

    ok = connection_write(c, sizeof(int));
    assert(ok);
    *ok = 1;
    return 0;
}

static int esd_proto_stream_play(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    char name[ESD_NAME_MAX];
    int format, rate;
    struct pa_sink *sink;
    struct pa_sample_spec ss;
    size_t l;
    assert(c && length == (sizeof(int)*2+ESD_NAME_MAX));
    
    format = maybe_swap_endian_32(c->swap_byte_order, *(int*)data);
    rate = maybe_swap_endian_32(c->swap_byte_order, *((int*)data + 1));

    ss.rate = rate;
    format_esd2native(format, &ss);

    if (!pa_sample_spec_valid(&ss)) {
        pa_log(__FILE__": invalid sample specification\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1))) {
        pa_log(__FILE__": no such sink\n");
        return -1;
    }
    
    strncpy(name, (char*) data + sizeof(int)*2, sizeof(name));
    name[sizeof(name)-1] = 0;

    pa_client_set_name(c->client, name);

    assert(!c->sink_input && !c->input_memblockq);

    if (!(c->sink_input = pa_sink_input_new(sink, PA_TYPEID_ESOUND, name, &ss, 0, -1))) {
        pa_log(__FILE__": failed to create sink input.\n");
        return -1;
    }

    l = (size_t) (pa_bytes_per_second(&ss)*PLAYBACK_BUFFER_SECONDS); 
    c->input_memblockq = pa_memblockq_new(l, 0, pa_frame_size(&ss), l/2, l/PLAYBACK_BUFFER_FRAGMENTS, c->protocol->core->memblock_stat);
    pa_iochannel_socket_set_rcvbuf(c->io, l/PLAYBACK_BUFFER_FRAGMENTS*2);
    c->playback.fragment_size = l/10;

    c->sink_input->owner = c->protocol->module;
    c->sink_input->client = c->client;
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
    char name[ESD_NAME_MAX];
    int format, rate;
    struct pa_source *source;
    struct pa_sample_spec ss;
    size_t l;
    assert(c && length == (sizeof(int)*2+ESD_NAME_MAX));
    
    format = maybe_swap_endian_32(c->swap_byte_order, *(int*)data);
    rate = maybe_swap_endian_32(c->swap_byte_order, *((int*)data + 1));

    ss.rate = rate;
    format_esd2native(format, &ss);

    if (!pa_sample_spec_valid(&ss)) {
        pa_log(__FILE__": invalid sample specification.\n");
        return -1;
    }

    if (request == ESD_PROTO_STREAM_MON) {
        struct pa_sink* sink;

        if (!(sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1))) {
            pa_log(__FILE__": no such sink.\n");
            return -1;
        }

        if (!(source = sink->monitor_source)) {
            pa_log(__FILE__": no such monitor source.\n");
            return -1;
        }
    } else {
        assert(request == ESD_PROTO_STREAM_REC);
        
        if (!(source = pa_namereg_get(c->protocol->core, c->protocol->source_name, PA_NAMEREG_SOURCE, 1))) {
            pa_log(__FILE__": no such source.\n");
            return -1;
        }
    }
    
    strncpy(name, (char*) data + sizeof(int)*2, sizeof(name));
    name[sizeof(name)-1] = 0;

    pa_client_set_name(c->client, name);

    assert(!c->output_memblockq && !c->source_output);

    if (!(c->source_output = pa_source_output_new(source, PA_TYPEID_ESOUND, name, &ss, -1))) {
        pa_log(__FILE__": failed to create source output\n");
        return -1;
    }

    l = (size_t) (pa_bytes_per_second(&ss)*RECORD_BUFFER_SECONDS); 
    c->output_memblockq = pa_memblockq_new(l, 0, pa_frame_size(&ss), 0, 0, c->protocol->core->memblock_stat);
    pa_iochannel_socket_set_sndbuf(c->io, l/RECORD_BUFFER_FRAGMENTS*2);
    
    c->source_output->owner = c->protocol->module;
    c->source_output->client = c->client;
    c->source_output->push = source_output_push_cb;
    c->source_output->kill = source_output_kill_cb;
    c->source_output->get_latency = source_output_get_latency_cb;
    c->source_output->userdata = c;

    c->state = ESD_STREAMING_DATA;

    c->protocol->n_player++;
    
    return 0;
}

static int esd_proto_get_latency(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    struct pa_sink *sink;
    int latency, *lag;
    assert(c && !data && length == 0);

    if (!(sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1)))
        latency = 0;
    else {
        double usec = pa_sink_get_latency(sink);
        usec += PLAYBACK_BUFFER_SECONDS*1000000;          /* A better estimation would be a good idea! */
        latency = (int) ((usec*44100)/1000000);
    }
    
    lag = connection_write(c, sizeof(int));
    assert(lag);
    *lag = c->swap_byte_order ? swap_endian_32(latency) : latency;
    return 0;
}

static int esd_proto_server_info(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    int rate = 44100, format = ESD_STEREO|ESD_BITS16;
    int *response;
    struct pa_sink *sink;
    assert(c && data && length == sizeof(int));

    if ((sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1))) {
        rate = sink->sample_spec.rate;
        format = format_native2esd(&sink->sample_spec);
    }
    
    response = connection_write(c, sizeof(int)*3);
    assert(response);
    *(response++) = 0;
    *(response++) = maybe_swap_endian_32(c->swap_byte_order, rate);
    *(response++) = maybe_swap_endian_32(c->swap_byte_order, format);
    return 0;
}

static int esd_proto_all_info(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    uint8_t *response;
    size_t t, k, s;
    struct connection *conn;
    size_t index = PA_IDXSET_INVALID;
    unsigned nsamples;
    assert(c && data && length == sizeof(int));
    
    if (esd_proto_server_info(c, request, data, length) < 0)
        return -1;

    k = sizeof(int)*5+ESD_NAME_MAX;
    s = sizeof(int)*6+ESD_NAME_MAX;
    nsamples = c->protocol->core->scache ? pa_idxset_ncontents(c->protocol->core->scache) : 0;
    response = connection_write(c, (t = s*(nsamples+1) + k*(c->protocol->n_player+1)));
    assert(k);

    for (conn = pa_idxset_first(c->protocol->connections, &index); conn; conn = pa_idxset_next(c->protocol->connections, &index)) {
        int format = ESD_BITS16 | ESD_STEREO, rate = 44100, volume = 0xFF;

        if (conn->state != ESD_STREAMING_DATA)
            continue;

        assert(t >= s+k+k);
        
        if (conn->sink_input) {
            rate = conn->sink_input->sample_spec.rate;
            volume = (conn->sink_input->volume*0xFF)/0x100;
            format = format_native2esd(&conn->sink_input->sample_spec);
        }
        
        /* id */
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, (int) (conn->index+1));
        response += sizeof(int);

        /* name */
        assert(conn->client);
        strncpy((char*) response, conn->client->name, ESD_NAME_MAX);
        response += ESD_NAME_MAX;

        /* rate */
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order,  rate);
        response += sizeof(int);

        /* left */
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order,  volume);
        response += sizeof(int);

        /*right*/
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order,  volume);
        response += sizeof(int);

        /*format*/
        *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, format);
        response += sizeof(int);

        t-= k;
    }

    assert(t == s*(nsamples+1)+k);
    memset(response, 0, k);
    response += k;
    t -= k;

    if (nsamples) {
        struct pa_scache_entry *ce;
        
        index = PA_IDXSET_INVALID;
        for (ce = pa_idxset_first(c->protocol->core->scache, &index); ce; ce = pa_idxset_next(c->protocol->core->scache, &index)) {
            assert(t >= s*2);
            
            /* id */
            *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, (int) (ce->index+1));
            response += sizeof(int);
            
            /* name */
            if (strncmp(ce->name, SCACHE_PREFIX, sizeof(SCACHE_PREFIX)-1) == 0)
                strncpy((char*) response, ce->name+sizeof(SCACHE_PREFIX)-1, ESD_NAME_MAX);
            else
                snprintf((char*) response, ESD_NAME_MAX, "native.%s", ce->name);
            response += ESD_NAME_MAX;
            
            /* rate */
            *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, ce->sample_spec.rate);
            response += sizeof(int);
            
            /* left */
            *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, (ce->volume*0xFF)/0x100);
            response += sizeof(int);
            
            /*right*/
            *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, (ce->volume*0xFF)/0x100);
            response += sizeof(int);
            
            /*format*/
            *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, format_native2esd(&ce->sample_spec));
            response += sizeof(int);

            /*length*/
            *((int*) response) = maybe_swap_endian_32(c->swap_byte_order, (int) ce->memchunk.length);
            response += sizeof(int);

            t -= s;
        }
    }

    assert(t == s);
    memset(response, 0, s);

    return 0;
}

static int esd_proto_stream_pan(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    int *ok;
    uint32_t index, volume;
    struct connection *conn;
    assert(c && data && length == sizeof(int)*3);
    
    index = (uint32_t) maybe_swap_endian_32(c->swap_byte_order, *(int*)data)-1;
    volume = (uint32_t) maybe_swap_endian_32(c->swap_byte_order, *((int*)data + 1));
    volume = (volume*0x100)/0xFF;

    ok = connection_write(c, sizeof(int));
    assert(ok);

    if ((conn = pa_idxset_get_by_index(c->protocol->connections, index))) {
        assert(conn->sink_input);
        conn->sink_input->volume = volume;
        *ok = 1;
    } else
        *ok = 0;
    
    return 0;
}

static int esd_proto_sample_cache(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    struct pa_sample_spec ss;
    int format, rate;
    size_t sc_length;
    uint32_t index;
    int *ok;
    char name[ESD_NAME_MAX+sizeof(SCACHE_PREFIX)-1];
    assert(c && data && length == (ESD_NAME_MAX+3*sizeof(int)));

    format = maybe_swap_endian_32(c->swap_byte_order, *(int*)data);
    rate = maybe_swap_endian_32(c->swap_byte_order, *((int*)data + 1));
    
    ss.rate = rate;
    format_esd2native(format, &ss);

    sc_length = (size_t) maybe_swap_endian_32(c->swap_byte_order, (*((int*)data + 2)));

    if (sc_length >= MAX_CACHE_SAMPLE_SIZE)
        return -1;

    strcpy(name, SCACHE_PREFIX);
    strncpy(name+sizeof(SCACHE_PREFIX)-1, (char*) data+3*sizeof(int), ESD_NAME_MAX);
    name[sizeof(name)-1] = 0;
    
    assert(!c->scache.memchunk.memblock);
    c->scache.memchunk.memblock = pa_memblock_new(sc_length, c->protocol->core->memblock_stat);
    c->scache.memchunk.index = 0;
    c->scache.memchunk.length = sc_length;
    c->scache.sample_spec = ss;
    assert(!c->scache.name);
    c->scache.name = pa_xstrdup(name);

    c->state = ESD_CACHING_SAMPLE;

    pa_scache_add_item(c->protocol->core, c->scache.name, NULL, NULL, &index);

    ok = connection_write(c, sizeof(int));
    assert(ok);
    
    *ok = index+1;
    
    return 0;
}

static int esd_proto_sample_get_id(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    int *ok;
    uint32_t index;
    char name[ESD_NAME_MAX+sizeof(SCACHE_PREFIX)-1];
    assert(c && data && length == ESD_NAME_MAX);

    ok = connection_write(c, sizeof(int));
    assert(ok);

    *ok = -1;

    strcpy(name, SCACHE_PREFIX);
    strncpy(name+sizeof(SCACHE_PREFIX)-1, data, ESD_NAME_MAX);
    name[sizeof(name)-1] = 0;

    if ((index = pa_scache_get_id_by_name(c->protocol->core, name)) != PA_IDXSET_INVALID)
        *ok = (int) index +1;

    return 0;
}

static int esd_proto_sample_free_or_play(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    int *ok;
    const char *name;
    uint32_t index;
    assert(c && data && length == sizeof(int));

    index = (uint32_t) maybe_swap_endian_32(c->swap_byte_order, *(int*)data)-1;

    ok = connection_write(c, sizeof(int));
    assert(ok);

    *ok = 0;
    
    if ((name = pa_scache_get_name_by_id(c->protocol->core, index))) {
        if (request == ESD_PROTO_SAMPLE_PLAY) {
            struct pa_sink *sink;
        
            if ((sink = pa_namereg_get(c->protocol->core, c->protocol->sink_name, PA_NAMEREG_SINK, 1)))
                if (pa_scache_play_item(c->protocol->core, name, sink, PA_VOLUME_NORM) >= 0)
                    *ok = (int) index+1;
        } else {
            assert(request == ESD_PROTO_SAMPLE_FREE);

            if (pa_scache_remove_item(c->protocol->core, name) >= 0)
                *ok = (int) index+1;
        }
    }
    
    return 0;
}

static int esd_proto_standby_or_resume(struct connection *c, esd_proto_t request, const void *data, size_t length) {
    int *ok;
    ok = connection_write(c, sizeof(int)*2);
    assert(ok);
    ok[0] = 1;
    ok[1] = 1;
    return 0;
}

/*** client callbacks ***/

static void client_kill_cb(struct pa_client *c) {
    assert(c && c->userdata);
    connection_free(c->userdata);
}

/*** pa_iochannel callbacks ***/

static int do_read(struct connection *c) {
    assert(c && c->io);

/*      pa_log("READ\n");  */
    
    if (c->state == ESD_NEXT_REQUEST) {
        ssize_t r;
        assert(c->read_data_length < sizeof(c->request));

        if ((r = pa_iochannel_read(c->io, ((uint8_t*) &c->request) + c->read_data_length, sizeof(c->request) - c->read_data_length)) <= 0) {
            if (r != 0)
                pa_log_warn(__FILE__": read() failed: %s\n", strerror(errno));
            return -1;
        }

        if ((c->read_data_length+= r) >= sizeof(c->request)) {
            struct proto_handler *handler;
            
            if (c->swap_byte_order)
                c->request = swap_endian_32(c->request);

            if (c->request < ESD_PROTO_CONNECT || c->request > ESD_PROTO_MAX) {
                pa_log(__FILE__": recieved invalid request.\n");
                return -1;
            }

            handler = proto_map+c->request;

/*             pa_log(__FILE__": executing request #%u\n", c->request); */

            if (!handler->proc) {
                pa_log(__FILE__": recieved unimplemented request #%u.\n", c->request);
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
            if (r != 0)
                pa_log_warn(__FILE__": read() failed: %s\n", strerror(errno));
            return -1;
        }

        if ((c->read_data_length+= r) >= handler->data_length) {
            size_t l = c->read_data_length;
            assert(handler->proc);

            c->state = ESD_NEXT_REQUEST;
            c->read_data_length = 0;
            
            if (handler->proc(c, c->request, c->read_data, l) < 0)
                return -1;
        }
    } else if (c->state == ESD_CACHING_SAMPLE) {
        ssize_t r;

        assert(c->scache.memchunk.memblock && c->scache.name && c->scache.memchunk.index < c->scache.memchunk.length);
        
        if ((r = pa_iochannel_read(c->io, (uint8_t*) c->scache.memchunk.memblock->data+c->scache.memchunk.index, c->scache.memchunk.length-c->scache.memchunk.index)) <= 0) {
            if (r!= 0)
                pa_log_warn(__FILE__": read() failed: %s\n", strerror(errno));
            return -1;
        }

        c->scache.memchunk.index += r;
        assert(c->scache.memchunk.index <= c->scache.memchunk.length);
        
        if (c->scache.memchunk.index == c->scache.memchunk.length) {
            uint32_t index;
            int *ok;
            
            c->scache.memchunk.index = 0;
            pa_scache_add_item(c->protocol->core, c->scache.name, &c->scache.sample_spec, &c->scache.memchunk, &index);

            pa_memblock_unref(c->scache.memchunk.memblock);
            c->scache.memchunk.memblock = NULL;
            c->scache.memchunk.index = c->scache.memchunk.length = 0;

            pa_xfree(c->scache.name);
            c->scache.name = NULL;

            c->state = ESD_NEXT_REQUEST;

            ok = connection_write(c, sizeof(int));
            assert(ok);
            *ok = index+1;
        }
        
    } else if (c->state == ESD_STREAMING_DATA && c->sink_input) {
        struct pa_memchunk chunk;
        ssize_t r;
        size_t l;

        assert(c->input_memblockq);

/*         pa_log("STREAMING_DATA\n"); */

        if (!(l = pa_memblockq_missing(c->input_memblockq)))
            return 0;

        if (l > c->playback.fragment_size)
            l = c->playback.fragment_size;

        if (c->playback.current_memblock) 
            if (c->playback.current_memblock->length - c->playback.memblock_index < l) {
                pa_memblock_unref(c->playback.current_memblock);
                c->playback.current_memblock = NULL;
                c->playback.memblock_index = 0;
            }
        
        if (!c->playback.current_memblock) {
            c->playback.current_memblock = pa_memblock_new(c->playback.fragment_size*2, c->protocol->core->memblock_stat);
            assert(c->playback.current_memblock && c->playback.current_memblock->length >= l);
            c->playback.memblock_index = 0;
        }

        if ((r = pa_iochannel_read(c->io, (uint8_t*) c->playback.current_memblock->data+c->playback.memblock_index, l)) <= 0) {
            if (r != 0)
                pa_log(__FILE__": read() failed: %s\n", strerror(errno));
            return -1;
        }
        
/*         pa_log(__FILE__": read %u\n", r);  */
        
        chunk.memblock = c->playback.current_memblock;
        chunk.index = c->playback.memblock_index;
        chunk.length = r;
        assert(chunk.memblock);

        c->playback.memblock_index += r;
        
        assert(c->input_memblockq);
        pa_memblockq_push_align(c->input_memblockq, &chunk, 0);
        assert(c->sink_input);
        pa_sink_notify(c->sink_input->sink);
    }
    
    return 0;
}

static int do_write(struct connection *c) {
    assert(c && c->io);

/*     pa_log("WRITE\n"); */
    
    if (c->write_data_length) {
        ssize_t r;
        
        assert(c->write_data_index < c->write_data_length);
        if ((r = pa_iochannel_write(c->io, (uint8_t*) c->write_data+c->write_data_index, c->write_data_length-c->write_data_index)) < 0) {
            pa_log(__FILE__": write() failed: %s\n", strerror(errno));
            return -1;
        }
        
        if ((c->write_data_index +=r) >= c->write_data_length)
            c->write_data_length = c->write_data_index = 0;
        
    } else if (c->state == ESD_STREAMING_DATA && c->source_output) {
        struct pa_memchunk chunk;
        ssize_t r;

        assert(c->output_memblockq);
        if (pa_memblockq_peek(c->output_memblockq, &chunk) < 0)
            return 0;
        
        assert(chunk.memblock && chunk.length);
        
        if ((r = pa_iochannel_write(c->io, (uint8_t*) chunk.memblock->data+chunk.index, chunk.length)) < 0) {
            pa_memblock_unref(chunk.memblock);
            pa_log(__FILE__": write(): %s\n", strerror(errno));
            return -1;
        }

        pa_memblockq_drop(c->output_memblockq, &chunk, r);
        pa_memblock_unref(chunk.memblock);
    }
    
    return 0;
}

static void do_work(struct connection *c) {
    assert(c);

    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);
    c->protocol->core->mainloop->defer_enable(c->defer_event, 0);

/*     pa_log("DOWORK %i\n", pa_iochannel_is_hungup(c->io));   */

    if (!c->dead && pa_iochannel_is_readable(c->io))
        if (do_read(c) < 0)
            goto fail;

    if (!c->dead && pa_iochannel_is_writable(c->io))
        if (do_write(c) < 0)
            goto fail;

    /* In case the line was hungup, make sure to rerun this function
       as soon as possible, until all data has been read. */

    if (!c->dead && pa_iochannel_is_hungup(c->io))
        c->protocol->core->mainloop->defer_enable(c->defer_event, 1);
    
    return;

fail:

    if (c->state == ESD_STREAMING_DATA && c->sink_input) {
        c->dead = 1;
        pa_memblockq_prebuf_disable(c->input_memblockq);

        pa_iochannel_free(c->io);
        c->io = NULL;
        
    } else
        connection_free(c);
}

static void io_callback(struct pa_iochannel*io, void *userdata) {
    struct connection *c = userdata;
    assert(io && c && c->io == io);

/*     pa_log("IO\n");  */
    
    do_work(c);
}

/*** defer callback ***/

static void defer_callback(struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata) {
    struct connection *c = userdata;
    assert(a && c && c->defer_event == e);

/*     pa_log("DEFER\n"); */
    
    do_work(c);
}

/*** sink_input callbacks ***/

static int sink_input_peek_cb(struct pa_sink_input *i, struct pa_memchunk *chunk) {
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

static void sink_input_drop_cb(struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length) {
    struct connection*c = i->userdata;
    assert(i && c && length);

/*     pa_log("DROP\n"); */
    
    pa_memblockq_drop(c->input_memblockq, chunk, length);

    /* do something */
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);

    if (!c->dead)
        c->protocol->core->mainloop->defer_enable(c->defer_event, 1);

/*     assert(pa_memblockq_get_length(c->input_memblockq) > 2048); */
}

static void sink_input_kill_cb(struct pa_sink_input *i) {
    assert(i && i->userdata);
    connection_free((struct connection *) i->userdata);
}

static pa_usec_t sink_input_get_latency_cb(struct pa_sink_input *i) {
    struct connection*c = i->userdata;
    assert(i && c);
    return pa_bytes_to_usec(pa_memblockq_get_length(c->input_memblockq), &c->sink_input->sample_spec);
}

/*** source_output callbacks ***/

static void source_output_push_cb(struct pa_source_output *o, const struct pa_memchunk *chunk) {
    struct connection *c = o->userdata;
    assert(o && c && chunk);

    pa_memblockq_push(c->output_memblockq, chunk, 0);

    /* do something */
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);

    if (!c->dead)
        c->protocol->core->mainloop->defer_enable(c->defer_event, 1);
}

static void source_output_kill_cb(struct pa_source_output *o) {
    assert(o && o->userdata);
    connection_free((struct connection *) o->userdata);
}

static pa_usec_t source_output_get_latency_cb(struct pa_source_output *o) {
    struct connection*c = o->userdata;
    assert(o && c);
    return pa_bytes_to_usec(pa_memblockq_get_length(c->output_memblockq), &c->source_output->sample_spec);
}

/*** socket server callback ***/

static void auth_timeout(struct pa_mainloop_api*m, struct pa_time_event *e, const struct timeval *tv, void *userdata) {
    struct connection *c = userdata;
    assert(m && tv && c && c->auth_timeout_event == e);

    if (!c->authorized)
        connection_free(c);
}

static void on_connection(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata) {
    struct connection *c;
    struct pa_protocol_esound *p = userdata;
    char cname[256];
    assert(s && io && p);

    if (pa_idxset_ncontents(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log(__FILE__": Warning! Too many connections (%u), dropping incoming connection.\n", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }
    
    c = pa_xmalloc(sizeof(struct connection));
    c->protocol = p;
    c->io = io;
    pa_iochannel_set_callback(c->io, io_callback, c);

    pa_iochannel_socket_peer_to_string(io, cname, sizeof(cname));
    assert(p->core);
    c->client = pa_client_new(p->core, PA_TYPEID_ESOUND, cname);
    assert(c->client);
    c->client->owner = p->module;
    c->client->kill = client_kill_cb;
    c->client->userdata = c;
    
    c->authorized = p->public;
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

struct pa_protocol_esound* pa_protocol_esound_new(struct pa_core*core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma) {
    struct pa_protocol_esound *p;
    int public = 0;
    assert(core && server && ma);

    p = pa_xmalloc(sizeof(struct pa_protocol_esound));

    if (pa_modargs_get_value_boolean(ma, "public", &public) < 0) {
        pa_log(__FILE__": public= expects a boolean argument.\n");
        return NULL;
    }

    if (pa_authkey_load_auto(pa_modargs_get_value(ma, "cookie", DEFAULT_COOKIE_FILE), p->esd_key, sizeof(p->esd_key)) < 0) {
        pa_xfree(p);
        return NULL;
    }

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
}

void pa_protocol_esound_free(struct pa_protocol_esound *p) {
    struct connection *c;
    assert(p);

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_free(c);

    pa_idxset_free(p->connections, NULL, NULL);
    pa_socket_server_unref(p->server);
    pa_xfree(p);
}
