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

#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#include <polypcore/native-common.h>
#include <polypcore/packet.h>
#include <polypcore/client.h>
#include <polypcore/source-output.h>
#include <polypcore/sink-input.h>
#include <polypcore/pstream.h>
#include <polypcore/tagstruct.h>
#include <polypcore/pdispatch.h>
#include <polypcore/pstream-util.h>
#include <polypcore/authkey.h>
#include <polypcore/namereg.h>
#include <polypcore/core-scache.h>
#include <polypcore/xmalloc.h>
#include <polypcore/util.h>
#include <polypcore/core-subscribe.h>
#include <polypcore/log.h>
#include <polypcore/autoload.h>
#include <polypcore/authkey-prop.h>
#include <polypcore/strlist.h>
#include <polypcore/props.h>
#include <polypcore/sample-util.h>
#include <polypcore/llist.h>

#include "protocol-native.h"

/* Kick a client if it doesn't authenticate within this time */
#define AUTH_TIMEOUT 5

/* Don't accept more connection than this */
#define MAX_CONNECTIONS 10

struct connection;
struct pa_protocol_native;

struct record_stream {
    struct connection *connection;
    uint32_t index;
    pa_source_output *source_output;
    pa_memblockq *memblockq;
    size_t fragment_size;
};

struct playback_stream {
    int type;
    struct connection *connection;
    uint32_t index;
    pa_sink_input *sink_input;
    pa_memblockq *memblockq;
    size_t requested_bytes;
    int drain_request;
    uint32_t drain_tag;
    uint32_t syncid;
    int underrun;

    /* Sync group members */
    PA_LLIST_FIELDS(struct playback_stream);
};

struct upload_stream {
    int type;
    struct connection *connection;
    uint32_t index;
    pa_memchunk memchunk;
    size_t length;
    char *name;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
};

struct output_stream {
    int type;
};

enum {
    UPLOAD_STREAM,
    PLAYBACK_STREAM
};

struct connection {
    int authorized;
    pa_protocol_native *protocol;
    pa_client *client;
    pa_pstream *pstream;
    pa_pdispatch *pdispatch;
    pa_idxset *record_streams, *output_streams;
    uint32_t rrobin_index;
    pa_subscription *subscription;
    pa_time_event *auth_timeout_event;
};

struct pa_protocol_native {
    pa_module *module;
    int public;
    pa_core *core;
    pa_socket_server *server;
    pa_idxset *connections;
    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];
    int auth_cookie_in_property;
};

static int sink_input_peek_cb(pa_sink_input *i, pa_memchunk *chunk);
static void sink_input_drop_cb(pa_sink_input *i, const pa_memchunk *chunk, size_t length);
static void sink_input_kill_cb(pa_sink_input *i);
static pa_usec_t sink_input_get_latency_cb(pa_sink_input *i);

static void request_bytes(struct playback_stream*s);

static void source_output_kill_cb(pa_source_output *o);
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk);
static pa_usec_t source_output_get_latency_cb(pa_source_output *o);

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
static void command_cork_playback_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_flush_playback_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
static void command_trigger_or_prebuf_playback_stream(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);
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
    
    [PA_COMMAND_CORK_PLAYBACK_STREAM] = command_cork_playback_stream,
    [PA_COMMAND_FLUSH_PLAYBACK_STREAM] = command_flush_playback_stream,
    [PA_COMMAND_TRIGGER_PLAYBACK_STREAM] = command_trigger_or_prebuf_playback_stream,
    [PA_COMMAND_PREBUF_PLAYBACK_STREAM] = command_trigger_or_prebuf_playback_stream,
    
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

};

/* structure management */

static struct upload_stream* upload_stream_new(
    struct connection *c,
    const pa_sample_spec *ss,
    const pa_channel_map *map,
    const char *name, size_t length) {
    
    struct upload_stream *s;
    assert(c && ss && name && length);
    
    s = pa_xnew(struct upload_stream, 1);
    s->type = UPLOAD_STREAM;
    s->connection = c;
    s->sample_spec = *ss;
    s->channel_map = *map;
    s->name = pa_xstrdup(name);

    s->memchunk.memblock = NULL;
    s->memchunk.index = 0;
    s->memchunk.length = 0;

    s->length = length;
    
    pa_idxset_put(c->output_streams, s, &s->index);
    return s;
}

static void upload_stream_free(struct upload_stream *o) {
    assert(o && o->connection);

    pa_idxset_remove_by_data(o->connection->output_streams, o, NULL);

    pa_xfree(o->name);
    
    if (o->memchunk.memblock)
        pa_memblock_unref(o->memchunk.memblock);
    
    pa_xfree(o);
}

static struct record_stream* record_stream_new(
    struct connection *c,
    pa_source *source,
    const pa_sample_spec *ss,
    const pa_channel_map *map,
    const char *name,
    size_t maxlength,
    size_t fragment_size) {
    
    struct record_stream *s;
    pa_source_output *source_output;
    size_t base;
    assert(c && source && ss && name && maxlength);

    if (!(source_output = pa_source_output_new(source, __FILE__, name, ss, map, -1)))
        return NULL;

    s = pa_xnew(struct record_stream, 1);
    s->connection = c;
    s->source_output = source_output;
    s->source_output->push = source_output_push_cb;
    s->source_output->kill = source_output_kill_cb;
    s->source_output->get_latency = source_output_get_latency_cb;
    s->source_output->userdata = s;
    s->source_output->owner = c->protocol->module;
    s->source_output->client = c->client;

    s->memblockq = pa_memblockq_new(
            0,
            maxlength,
            0,
            base = pa_frame_size(ss),
            1,
            0,
            NULL,
            c->protocol->core->memblock_stat);
    assert(s->memblockq);

    s->fragment_size = (fragment_size/base)*base;
    if (!s->fragment_size)
        s->fragment_size = base;

    pa_idxset_put(c->record_streams, s, &s->index);
    return s;
}

static void record_stream_free(struct record_stream* r) {
    assert(r && r->connection);

    pa_idxset_remove_by_data(r->connection->record_streams, r, NULL);
    pa_source_output_disconnect(r->source_output);
    pa_source_output_unref(r->source_output);
    pa_memblockq_free(r->memblockq);
    pa_xfree(r);
}

static struct playback_stream* playback_stream_new(
    struct connection *c,
    pa_sink *sink,
    const pa_sample_spec *ss,
    const pa_channel_map *map,
    const char *name,
    size_t maxlength,
    size_t tlength,
    size_t prebuf,
    size_t minreq,
    pa_cvolume *volume,
    uint32_t syncid) {
    
    struct playback_stream *s, *sync;
    pa_sink_input *sink_input;
    pa_memblock *silence;
    uint32_t idx;
    int64_t start_index;
    
    assert(c && sink && ss && name && maxlength);

    /* Find syncid group */
    for (sync = pa_idxset_first(c->output_streams, &idx); sync; sync = pa_idxset_next(c->output_streams, &idx)) {
        
        if (sync->type != PLAYBACK_STREAM)
            continue;

        if (sync->syncid == syncid)
            break;
    }

    /* Synced streams must connect to the same sink */
    if (sync && sync->sink_input->sink != sink)
        return NULL;
    
    if (!(sink_input = pa_sink_input_new(sink, __FILE__, name, ss, map, 0, -1)))
        return NULL;
    
    s = pa_xnew(struct playback_stream, 1);
    s->type = PLAYBACK_STREAM;
    s->connection = c;
    s->syncid = syncid;
    s->sink_input = sink_input;
    s->underrun = 1;
    
    s->sink_input->peek = sink_input_peek_cb;
    s->sink_input->drop = sink_input_drop_cb;
    s->sink_input->kill = sink_input_kill_cb;
    s->sink_input->get_latency = sink_input_get_latency_cb;
    s->sink_input->userdata = s;
    s->sink_input->owner = c->protocol->module;
    s->sink_input->client = c->client;

    if (sync) {
        /* Sync id found, now find head of list */
        PA_LLIST_FIND_HEAD(struct playback_stream, sync, &sync);

        /* Prepend ourselves */
        PA_LLIST_PREPEND(struct playback_stream, sync, s);

        /* Set our start index to the current read index of the other grozp member(s) */
        assert(sync->next);
        start_index = pa_memblockq_get_read_index(sync->next->memblockq);
    } else {
        /* This ia a new sync group */
        PA_LLIST_INIT(struct playback_stream, s);
        start_index = 0;
    }
    
    silence = pa_silence_memblock_new(ss, 0, c->protocol->core->memblock_stat);
    
    s->memblockq = pa_memblockq_new(
            start_index,
            maxlength,
            tlength,
            pa_frame_size(ss),
            prebuf,
            minreq,
            silence,
            c->protocol->core->memblock_stat);

    pa_memblock_unref(silence);
    
    s->requested_bytes = 0;
    s->drain_request = 0;

    s->sink_input->volume = *volume;
    
    pa_idxset_put(c->output_streams, s, &s->index);

    return s;
}

static void playback_stream_free(struct playback_stream* p) {
    struct playback_stream *head;
    assert(p && p->connection);

    if (p->drain_request)
        pa_pstream_send_error(p->connection->pstream, p->drain_tag, PA_ERR_NOENTITY);

    PA_LLIST_FIND_HEAD(struct playback_stream, p, &head);
    PA_LLIST_REMOVE(struct playback_stream, head, p);

    pa_idxset_remove_by_data(p->connection->output_streams, p, NULL);
    pa_sink_input_disconnect(p->sink_input);
    pa_sink_input_unref(p->sink_input);
    pa_memblockq_free(p->memblockq);
    pa_xfree(p);
}

static void connection_free(struct connection *c) {
    struct record_stream *r;
    struct output_stream *o;
    assert(c && c->protocol);

    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);
    while ((r = pa_idxset_first(c->record_streams, NULL)))
        record_stream_free(r);
    pa_idxset_free(c->record_streams, NULL, NULL);

    while ((o = pa_idxset_first(c->output_streams, NULL)))
        if (o->type == PLAYBACK_STREAM)
            playback_stream_free((struct playback_stream*) o);
        else
            upload_stream_free((struct upload_stream*) o);
    pa_idxset_free(c->output_streams, NULL, NULL);

    pa_pdispatch_unref(c->pdispatch);
    pa_pstream_close(c->pstream);
    pa_pstream_unref(c->pstream);
    pa_client_free(c->client);

    if (c->subscription)
        pa_subscription_free(c->subscription);

    if (c->auth_timeout_event)
        c->protocol->core->mainloop->time_free(c->auth_timeout_event);
    
    pa_xfree(c);
}

static void request_bytes(struct playback_stream *s) {
    pa_tagstruct *t;
    size_t l;
    assert(s);

    if (!(l = pa_memblockq_missing(s->memblockq)))
        return;
    
    if (l <= s->requested_bytes)
        return;

    l -= s->requested_bytes;

    if (l < pa_memblockq_get_minreq(s->memblockq))
        return;
    
    s->requested_bytes += l;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_REQUEST);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_putu32(t, l);
    pa_pstream_send_tagstruct(s->connection->pstream, t);

/*     pa_log(__FILE__": Requesting %u bytes\n", l);  */
}

static void send_memblock(struct connection *c) {
    uint32_t start;
    struct record_stream *r;

    start = PA_IDXSET_INVALID;
    for (;;) {
        pa_memchunk chunk;
        
        if (!(r = pa_idxset_rrobin(c->record_streams, &c->rrobin_index)))
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
            pa_memblockq_drop(r->memblockq, &chunk, schunk.length);
            pa_memblock_unref(schunk.memblock);
            
            return;
        }
    }
}

static void send_playback_stream_killed(struct playback_stream *p) {
    pa_tagstruct *t;
    assert(p);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_PLAYBACK_STREAM_KILLED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, p->index);
    pa_pstream_send_tagstruct(p->connection->pstream, t);
}

static void send_record_stream_killed(struct record_stream *r) {
    pa_tagstruct *t;
    assert(r);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_RECORD_STREAM_KILLED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, r->index);
    pa_pstream_send_tagstruct(r->connection->pstream, t);
}

/*** sinkinput callbacks ***/

static int sink_input_peek_cb(pa_sink_input *i, pa_memchunk *chunk) {
    struct playback_stream *s;
    assert(i && i->userdata && chunk);
    s = i->userdata;

    if (pa_memblockq_get_length(s->memblockq) <= 0 && !s->underrun) {
        pa_tagstruct *t;

        /* Report that we're empty */

        t = pa_tagstruct_new(NULL, 0);
        pa_tagstruct_putu32(t, PA_COMMAND_UNDERFLOW);
        pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
        pa_tagstruct_putu32(t, s->index);
        pa_pstream_send_tagstruct(s->connection->pstream, t);

        s->underrun = 1;
    }
    
    if (pa_memblockq_peek(s->memblockq, chunk) < 0) {
/*         pa_log(__FILE__": peek: failure\n");    */
        return -1;
    }

/*     pa_log(__FILE__": peek: %u\n", chunk->length);    */
    
    return 0;
}

static void sink_input_drop_cb(pa_sink_input *i, const pa_memchunk *chunk, size_t length) {
    struct playback_stream *s;
    assert(i && i->userdata && length);
    s = i->userdata;

    pa_memblockq_drop(s->memblockq, chunk, length);

    request_bytes(s);

    if (s->drain_request && !pa_memblockq_is_readable(s->memblockq)) {
        pa_pstream_send_simple_ack(s->connection->pstream, s->drain_tag);
        s->drain_request = 0;
    }

/*     pa_log(__FILE__": after_drop: %u %u\n", pa_memblockq_get_length(s->memblockq), pa_memblockq_is_readable(s->memblockq));   */
}

static void sink_input_kill_cb(pa_sink_input *i) {
    assert(i && i->userdata);
    send_playback_stream_killed((struct playback_stream *) i->userdata);
    playback_stream_free((struct playback_stream *) i->userdata);
}

static pa_usec_t sink_input_get_latency_cb(pa_sink_input *i) {
    struct playback_stream *s;
    assert(i && i->userdata);
    s = i->userdata;

    /*pa_log(__FILE__": get_latency: %u\n", pa_memblockq_get_length(s->memblockq));*/
    
    return pa_bytes_to_usec(pa_memblockq_get_length(s->memblockq), &s->sink_input->sample_spec);
}

/*** source_output callbacks ***/

static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct record_stream *s;
    assert(o && o->userdata && chunk);
    s = o->userdata;
    
    if (pa_memblockq_push_align(s->memblockq, chunk) < 0) {
        pa_log_warn(__FILE__": Failed to push data into output queue.\n");
        return;
    } 
        
    if (!pa_pstream_is_pending(s->connection->pstream))
        send_memblock(s->connection);
}

static void source_output_kill_cb(pa_source_output *o) {
    assert(o && o->userdata);
    send_record_stream_killed((struct record_stream *) o->userdata);
    record_stream_free((struct record_stream *) o->userdata);
}

static pa_usec_t source_output_get_latency_cb(pa_source_output *o) {
    struct record_stream *s;
    assert(o && o->userdata);
    s = o->userdata;

    /*pa_log(__FILE__": get_latency: %u\n", pa_memblockq_get_length(s->memblockq));*/
    
    return pa_bytes_to_usec(pa_memblockq_get_length(s->memblockq), &o->sample_spec);
}

/*** pdispatch callbacks ***/

static void protocol_error(struct connection *c) {
    pa_log(__FILE__": protocol error, kicking client\n");
    connection_free(c);
}

static void command_create_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *s;
    size_t maxlength, tlength, prebuf, minreq;
    uint32_t sink_index, syncid;
    const char *name, *sink_name;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_tagstruct *reply;
    pa_sink *sink;
    pa_cvolume volume;
    int corked;
    
    assert(c && t && c->protocol && c->protocol->core);
    
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
            PA_TAG_INVALID) < 0 ||
        !pa_tagstruct_eof(t) ||
        !name) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (sink_index != PA_INVALID_INDEX)
        sink = pa_idxset_get_by_index(c->protocol->core->sinks, sink_index);
    else
        sink = pa_namereg_get(c->protocol->core, sink_name, PA_NAMEREG_SINK, 1);

    if (!sink) {
        pa_log_warn(__FILE__": Can't find a suitable sink.\n");
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }
    
    if (!(s = playback_stream_new(c, sink, &ss, &map, name, maxlength, tlength, prebuf, minreq, &volume, syncid))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
        return;
    }

    pa_sink_input_cork(s->sink_input, corked);
    
    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, s->index);
    assert(s->sink_input);
    pa_tagstruct_putu32(reply, s->sink_input->index);
    pa_tagstruct_putu32(reply, s->requested_bytes = pa_memblockq_missing(s->memblockq));
    pa_pstream_send_tagstruct(c->pstream, reply);
    request_bytes(s);
}

static void command_delete_stream(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t channel;
    assert(c && t);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (command == PA_COMMAND_DELETE_PLAYBACK_STREAM) {
        struct playback_stream *s;
        if (!(s = pa_idxset_get_by_index(c->output_streams, channel)) || (s->type != PLAYBACK_STREAM)) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
            return;
        }

        playback_stream_free(s);
    } else if (command == PA_COMMAND_DELETE_RECORD_STREAM) {
        struct record_stream *s;
        if (!(s = pa_idxset_get_by_index(c->record_streams, channel))) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
            return;
        }

        record_stream_free(s);
    } else {
        struct upload_stream *s;
        assert(command == PA_COMMAND_DELETE_UPLOAD_STREAM);
        if (!(s = pa_idxset_get_by_index(c->output_streams, channel)) || (s->type != UPLOAD_STREAM)) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
            return;
        }

        upload_stream_free(s);
    }
            
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_create_record_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    struct record_stream *s;
    size_t maxlength, fragment_size;
    uint32_t source_index;
    const char *name, *source_name;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_tagstruct *reply;
    pa_source *source;
    int corked;
    assert(c && t && c->protocol && c->protocol->core);
    
    if (pa_tagstruct_gets(t, &name) < 0 || !name ||
        pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
        pa_tagstruct_get_channel_map(t, &map) < 0 ||
        pa_tagstruct_getu32(t, &source_index) < 0 ||
        pa_tagstruct_gets(t, &source_name) < 0 ||
        pa_tagstruct_getu32(t, &maxlength) < 0 ||
        pa_tagstruct_get_boolean(t, &corked) < 0 ||
        pa_tagstruct_getu32(t, &fragment_size) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (source_index != (uint32_t) -1)
        source = pa_idxset_get_by_index(c->protocol->core->sources, source_index);
    else
        source = pa_namereg_get(c->protocol->core, source_name, PA_NAMEREG_SOURCE, 1);

    if (!source) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }
    
    if (!(s = record_stream_new(c, source, &ss, &map, name, maxlength, fragment_size))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
        return;
    }

    pa_source_output_cork(s->source_output, corked);
    
    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, s->index);
    assert(s->source_output);
    pa_tagstruct_putu32(reply, s->source_output->index);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_exit(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    assert(c && t);
    
    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }
    
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop);
    c->protocol->core->mainloop->quit(c->protocol->core->mainloop, 0);
    pa_pstream_send_simple_ack(c->pstream, tag); /* nonsense */
    return;
}

static void command_auth(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const void*cookie;
    assert(c && t);

    if (pa_tagstruct_get_arbitrary(t, &cookie, PA_NATIVE_COOKIE_LENGTH) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        if (memcmp(c->protocol->auth_cookie, cookie, PA_NATIVE_COOKIE_LENGTH) != 0) {
            pa_log(__FILE__": Denied access to client with invalid authorization key.\n");
            pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
            return;
        }
        
        c->authorized = 1;
        if (c->auth_timeout_event) {
            c->protocol->core->mainloop->time_free(c->auth_timeout_event);
            c->auth_timeout_event = NULL;
        }
    }
    
    pa_pstream_send_simple_ack(c->pstream, tag);
    return;
}

static void command_set_client_name(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const char *name;
    assert(c && t);

    if (pa_tagstruct_gets(t, &name) < 0 || !name ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    pa_client_set_name(c->client, name);
    pa_pstream_send_simple_ack(c->pstream, tag);
    return;
}

static void command_lookup(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const char *name;
    uint32_t idx = PA_IDXSET_INVALID;
    assert(c && t);

    if (pa_tagstruct_gets(t, &name) < 0 || !name ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (command == PA_COMMAND_LOOKUP_SINK) {
        pa_sink *sink;
        if ((sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1)))
            idx = sink->index;
    } else {
        pa_source *source;
        assert(command == PA_COMMAND_LOOKUP_SOURCE);
        if ((source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1)))
            idx = source->index;
    }

    if (idx == PA_IDXSET_INVALID)
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
    else {
        pa_tagstruct *reply;
        reply = pa_tagstruct_new(NULL, 0);
        assert(reply);
        pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
        pa_tagstruct_putu32(reply, tag);
        pa_tagstruct_putu32(reply, idx);
        pa_pstream_send_tagstruct(c->pstream, reply);
    }
}

static void command_drain_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    struct playback_stream *s;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->output_streams, idx)) || s->type != PLAYBACK_STREAM) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    s->drain_request = 0;

    pa_memblockq_prebuf_disable(s->memblockq);
    
    if (!pa_memblockq_is_readable(s->memblockq)) {
/*         pa_log("immediate drain: %u\n", pa_memblockq_get_length(s->memblockq));  */
        pa_pstream_send_simple_ack(c->pstream, tag);
    } else {
/*         pa_log("slow drain triggered\n");  */
        s->drain_request = 1;
        s->drain_tag = tag;

        pa_sink_notify(s->sink_input->sink);
    }
} 

static void command_stat(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_tagstruct *reply;
    assert(c && t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, c->protocol->core->memblock_stat->total);
    pa_tagstruct_putu32(reply, c->protocol->core->memblock_stat->total_size);
    pa_tagstruct_putu32(reply, c->protocol->core->memblock_stat->allocated);
    pa_tagstruct_putu32(reply, c->protocol->core->memblock_stat->allocated_size);
    pa_tagstruct_putu32(reply, pa_scache_total_size(c->protocol->core));
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_playback_latency(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_tagstruct *reply;
    struct playback_stream *s;
    struct timeval tv, now;
    uint64_t counter;
    uint32_t idx;
    assert(c && t);
    
    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_timeval(t, &tv) < 0 ||
        pa_tagstruct_getu64(t, &counter) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->output_streams, idx)) || s->type != PLAYBACK_STREAM) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_put_usec(reply, pa_sink_input_get_latency(s->sink_input));
    pa_tagstruct_put_usec(reply, pa_sink_get_latency(s->sink_input->sink));
    pa_tagstruct_put_usec(reply, 0);
    pa_tagstruct_put_boolean(reply, pa_memblockq_is_readable(s->memblockq));
    pa_tagstruct_putu32(reply, pa_memblockq_get_length(s->memblockq));
    pa_tagstruct_put_timeval(reply, &tv);
    pa_gettimeofday(&now);
    pa_tagstruct_put_timeval(reply, &now);
    pa_tagstruct_putu64(reply, counter);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_record_latency(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_tagstruct *reply;
    struct record_stream *s;
    struct timeval tv, now;
    uint64_t counter;
    uint32_t idx;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_timeval(t, &tv) < 0 ||
        pa_tagstruct_getu64(t, &counter) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->record_streams, idx))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_put_usec(reply, pa_source_output_get_latency(s->source_output));
    pa_tagstruct_put_usec(reply, s->source_output->source->monitor_of ? pa_sink_get_latency(s->source_output->source->monitor_of) : 0);
    pa_tagstruct_put_usec(reply, pa_source_get_latency(s->source_output->source));
    pa_tagstruct_put_boolean(reply, 0);
    pa_tagstruct_putu32(reply, pa_memblockq_get_length(s->memblockq));
    pa_tagstruct_put_timeval(reply, &tv);
    pa_gettimeofday(&now);
    pa_tagstruct_put_timeval(reply, &now);
    pa_tagstruct_putu64(reply, counter);
    pa_pstream_send_tagstruct(c->pstream, reply);
}


static void command_create_upload_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    struct upload_stream *s;
    size_t length;
    const char *name;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_tagstruct *reply;
    assert(c && t && c->protocol && c->protocol->core);
    
    if (pa_tagstruct_gets(t, &name) < 0 || !name ||
        pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
        pa_tagstruct_get_channel_map(t, &map) < 0 ||
        pa_tagstruct_getu32(t, &length) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if ((length % pa_frame_size(&ss)) != 0 || length <= 0 || !*name) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
        return;
    }
    
    if (!(s = upload_stream_new(c, &ss, &map, name, length))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_INVALID);
        return;
    }
    
    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, s->index);
    pa_tagstruct_putu32(reply, length);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_finish_upload_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t channel;
    struct upload_stream *s;
    uint32_t idx;
    assert(c && t);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->output_streams, channel)) || (s->type != UPLOAD_STREAM)) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
        return;
    }

    pa_scache_add_item(c->protocol->core, s->name, &s->sample_spec, &s->channel_map, &s->memchunk, &idx);
    pa_pstream_send_simple_ack(c->pstream, tag);
    upload_stream_free(s);
}

static void command_play_sample(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t sink_index;
    pa_cvolume volume;
    pa_sink *sink;
    const char *name, *sink_name;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &sink_index) < 0 ||
        pa_tagstruct_gets(t, &sink_name) < 0 ||
        pa_tagstruct_get_cvolume(t, &volume) < 0 ||
        pa_tagstruct_gets(t, &name) < 0 || !name || 
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (sink_index != (uint32_t) -1)
        sink = pa_idxset_get_by_index(c->protocol->core->sinks, sink_index);
    else
        sink = pa_namereg_get(c->protocol->core, sink_name, PA_NAMEREG_SINK, 1);

    if (!sink) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    if (pa_scache_play_item(c->protocol->core, name, sink, &volume) < 0) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_remove_sample(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const char *name;
    assert(c && t);

    if (pa_tagstruct_gets(t, &name) < 0 || !name || 
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (pa_scache_remove_item(c->protocol->core, name) < 0) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void sink_fill_tagstruct(pa_tagstruct *t, pa_sink *sink) {
    assert(t && sink);
    pa_tagstruct_put(
        t,
        PA_TAG_U32, sink->index,
        PA_TAG_STRING, sink->name,
        PA_TAG_STRING, sink->description,
        PA_TAG_SAMPLE_SPEC, &sink->sample_spec,
        PA_TAG_CHANNEL_MAP, &sink->channel_map,
        PA_TAG_U32, sink->owner ? sink->owner->index : PA_INVALID_INDEX,
        PA_TAG_CVOLUME, pa_sink_get_volume(sink, PA_MIXER_HARDWARE),
        PA_TAG_U32, sink->monitor_source->index,
        PA_TAG_STRING, sink->monitor_source->name,
        PA_TAG_USEC, pa_sink_get_latency(sink),
        PA_TAG_STRING, sink->driver,
        PA_TAG_INVALID);
}

static void source_fill_tagstruct(pa_tagstruct *t, pa_source *source) {
    assert(t && source);
    pa_tagstruct_put(
        t,
        PA_TAG_U32, source->index,
        PA_TAG_STRING, source->name,
        PA_TAG_STRING, source->description,
        PA_TAG_SAMPLE_SPEC, &source->sample_spec,
        PA_TAG_CHANNEL_MAP, &source->channel_map,
        PA_TAG_U32, source->owner ? source->owner->index : PA_INVALID_INDEX,
        PA_TAG_CVOLUME, pa_source_get_volume(source, PA_MIXER_HARDWARE),
        PA_TAG_U32, source->monitor_of ? source->monitor_of->index : PA_INVALID_INDEX,
        PA_TAG_STRING, source->monitor_of ? source->monitor_of->name : NULL,
        PA_TAG_USEC, pa_source_get_latency(source),
        PA_TAG_STRING, source->driver,
        PA_TAG_INVALID);
}

static void client_fill_tagstruct(pa_tagstruct *t, pa_client *client) {
    assert(t && client);
    pa_tagstruct_putu32(t, client->index);
    pa_tagstruct_puts(t, client->name);
    pa_tagstruct_putu32(t, client->owner ? client->owner->index : (uint32_t) -1);
    pa_tagstruct_puts(t, client->driver);
}

static void module_fill_tagstruct(pa_tagstruct *t, pa_module *module) {
    assert(t && module);
    pa_tagstruct_putu32(t, module->index);
    pa_tagstruct_puts(t, module->name);
    pa_tagstruct_puts(t, module->argument);
    pa_tagstruct_putu32(t, module->n_used);
    pa_tagstruct_put_boolean(t, module->auto_unload);
}

static void sink_input_fill_tagstruct(pa_tagstruct *t, pa_sink_input *s) {
    assert(t && s);
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_putu32(t, s->owner ? s->owner->index : (uint32_t) -1);
    pa_tagstruct_putu32(t, s->client ? s->client->index : (uint32_t) -1);
    pa_tagstruct_putu32(t, s->sink->index);
    pa_tagstruct_put_sample_spec(t, &s->sample_spec);
    pa_tagstruct_put_channel_map(t, &s->channel_map);
    pa_tagstruct_put_cvolume(t, &s->volume);
    pa_tagstruct_put_usec(t, pa_sink_input_get_latency(s));
    pa_tagstruct_put_usec(t, pa_sink_get_latency(s->sink));
    pa_tagstruct_puts(t, pa_resample_method_to_string(pa_sink_input_get_resample_method(s)));
    pa_tagstruct_puts(t, s->driver);
}

static void source_output_fill_tagstruct(pa_tagstruct *t, pa_source_output *s) {
    assert(t && s);
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_putu32(t, s->owner ? s->owner->index : (uint32_t) -1);
    pa_tagstruct_putu32(t, s->client ? s->client->index : (uint32_t) -1);
    pa_tagstruct_putu32(t, s->source->index);
    pa_tagstruct_put_sample_spec(t, &s->sample_spec);
    pa_tagstruct_put_channel_map(t, &s->channel_map);
    pa_tagstruct_put_usec(t, pa_source_output_get_latency(s));
    pa_tagstruct_put_usec(t, pa_source_get_latency(s->source));
    pa_tagstruct_puts(t, pa_resample_method_to_string(pa_source_output_get_resample_method(s)));
    pa_tagstruct_puts(t, s->driver);
}

static void scache_fill_tagstruct(pa_tagstruct *t, pa_scache_entry *e) {
    assert(t && e);
    pa_tagstruct_putu32(t, e->index);
    pa_tagstruct_puts(t, e->name);
    pa_tagstruct_put_cvolume(t, &e->volume);
    pa_tagstruct_put_usec(t, pa_bytes_to_usec(e->memchunk.length, &e->sample_spec));
    pa_tagstruct_put_sample_spec(t, &e->sample_spec);
    pa_tagstruct_put_channel_map(t, &e->channel_map);
    pa_tagstruct_putu32(t, e->memchunk.length);
    pa_tagstruct_put_boolean(t, e->lazy);
    pa_tagstruct_puts(t, e->filename);
}

static void command_get_info(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
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
    assert(c && t);

    
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
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (command == PA_COMMAND_GET_SINK_INFO) {
        if (idx != (uint32_t) -1)
            sink = pa_idxset_get_by_index(c->protocol->core->sinks, idx);
        else
            sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1);
    } else if (command == PA_COMMAND_GET_SOURCE_INFO) {
        if (idx != (uint32_t) -1)
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
        assert(command == PA_COMMAND_GET_SAMPLE_INFO);
        if (idx != (uint32_t) -1)
            sce = pa_idxset_get_by_index(c->protocol->core->scache, idx);
        else
            sce = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SAMPLE, 0);
    }
            
    if (!sink && !source && !client && !module && !si && !so && !sce) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag); 
    if (sink)
        sink_fill_tagstruct(reply, sink);
    else if (source)
        source_fill_tagstruct(reply, source);
    else if (client)
        client_fill_tagstruct(reply, client);
    else if (module)
        module_fill_tagstruct(reply, module);
    else if (si)
        sink_input_fill_tagstruct(reply, si);
    else if (so)
        source_output_fill_tagstruct(reply, so);
    else
        scache_fill_tagstruct(reply, sce);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_info_list(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_idxset *i;
    uint32_t idx;
    void *p;
    pa_tagstruct *reply;
    assert(c && t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);

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
        assert(command == PA_COMMAND_GET_SAMPLE_INFO_LIST);
        i = c->protocol->core->scache;
    }

    if (i) {
        for (p = pa_idxset_first(i, &idx); p; p = pa_idxset_next(i, &idx)) {
            if (command == PA_COMMAND_GET_SINK_INFO_LIST)
                sink_fill_tagstruct(reply, p);
            else if (command == PA_COMMAND_GET_SOURCE_INFO_LIST)
                source_fill_tagstruct(reply, p);
            else if (command == PA_COMMAND_GET_CLIENT_INFO_LIST)
                client_fill_tagstruct(reply, p);
            else if (command == PA_COMMAND_GET_MODULE_INFO_LIST)
                module_fill_tagstruct(reply, p);
            else if (command == PA_COMMAND_GET_SINK_INPUT_INFO_LIST)
                sink_input_fill_tagstruct(reply, p);
            else if (command == PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST) 
                source_output_fill_tagstruct(reply, p);
            else {
                assert(command == PA_COMMAND_GET_SAMPLE_INFO_LIST);
                scache_fill_tagstruct(reply, p);
            }
        }
    }
    
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_server_info(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_tagstruct *reply;
    char txt[256];
    const char *n;
    assert(c && t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_puts(reply, PACKAGE_NAME);
    pa_tagstruct_puts(reply, PACKAGE_VERSION);
    pa_tagstruct_puts(reply, pa_get_user_name(txt, sizeof(txt)));
    pa_tagstruct_puts(reply, pa_get_fqdn(txt, sizeof(txt)));
    pa_tagstruct_put_sample_spec(reply, &c->protocol->core->default_sample_spec);

    n = pa_namereg_get_default_sink_name(c->protocol->core);
    pa_tagstruct_puts(reply, n);
    n = pa_namereg_get_default_source_name(c->protocol->core);
    pa_tagstruct_puts(reply, n);

    pa_tagstruct_putu32(reply, c->protocol->core->cookie);
    
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void subscription_cb(pa_core *core, pa_subscription_event_type_t e, uint32_t idx, void *userdata) {
    pa_tagstruct *t;
    struct connection *c = userdata;
    assert(c && core);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_SUBSCRIBE_EVENT);
    pa_tagstruct_putu32(t, (uint32_t) -1);
    pa_tagstruct_putu32(t, e);
    pa_tagstruct_putu32(t, idx);
    pa_pstream_send_tagstruct(c->pstream, t);
}

static void command_subscribe(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_subscription_mask_t m;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &m) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (c->subscription)
        pa_subscription_free(c->subscription);

    if (m != 0) {
        c->subscription = pa_subscription_new(c->protocol->core, m, subscription_cb, c);
        assert(c->subscription);
    } else
        c->subscription = NULL;

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_volume(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    pa_cvolume volume;
    pa_sink *sink = NULL;
    pa_source *source = NULL;
    pa_sink_input *si = NULL;
    const char *name = NULL;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        (command == PA_COMMAND_SET_SINK_VOLUME && pa_tagstruct_gets(t, &name) < 0) ||
        (command == PA_COMMAND_SET_SOURCE_VOLUME && pa_tagstruct_gets(t, &name) < 0) ||
        pa_tagstruct_get_cvolume(t, &volume) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (command == PA_COMMAND_SET_SINK_VOLUME) {
        if (idx != (uint32_t) -1)
            sink = pa_idxset_get_by_index(c->protocol->core->sinks, idx);
        else
            sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK, 1);
    } else if (command == PA_COMMAND_SET_SOURCE_VOLUME) {
        if (idx != (uint32_t) -1)
            source = pa_idxset_get_by_index(c->protocol->core->sources, idx);
        else
            source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE, 1);
    }  else {
        assert(command == PA_COMMAND_SET_SINK_INPUT_VOLUME);
        si = pa_idxset_get_by_index(c->protocol->core->sink_inputs, idx);
    }

    if (!si && !sink && !source) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    if (sink)
        pa_sink_set_volume(sink, PA_MIXER_HARDWARE, &volume);
    else if (source)
        pa_source_set_volume(source, PA_MIXER_HARDWARE, &volume);
    else if (si)
        pa_sink_input_set_volume(si, &volume);

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_cork_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    int b;
    struct playback_stream *s, *sync;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_boolean(t, &b) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->output_streams, idx)) || s->type != PLAYBACK_STREAM) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    fprintf(stderr, "Corking %i\n", b);

    pa_sink_input_cork(s->sink_input, b);
    pa_memblockq_prebuf_force(s->memblockq);

    /* Do the same for all other members in the sync group */
    for (sync = s->prev; sync; sync = sync->prev) {
        pa_sink_input_cork(sync->sink_input, b);
        pa_memblockq_prebuf_force(sync->memblockq);
    }

    for (sync = s->next; sync; sync = sync->next) {
        pa_sink_input_cork(sync->sink_input, b);
        pa_memblockq_prebuf_force(sync->memblockq);
    }
    
    pa_pstream_send_simple_ack(c->pstream, tag);
} 

static void command_flush_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    struct playback_stream *s, *sync;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->output_streams, idx)) || s->type != PLAYBACK_STREAM) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_memblockq_flush(s->memblockq);
    s->underrun = 0;
    
    /* Do the same for all other members in the sync group */
    for (sync = s->prev; sync; sync = sync->prev) {
        pa_memblockq_flush(sync->memblockq);
        sync->underrun = 0;
    }

    for (sync = s->next; sync; sync = sync->next) {
        pa_memblockq_flush(sync->memblockq);
        sync->underrun = 0;
    }
    
    pa_pstream_send_simple_ack(c->pstream, tag);
    pa_sink_notify(s->sink_input->sink);
    request_bytes(s);
    
    for (sync = s->prev; sync; sync = sync->prev)
        request_bytes(sync);

    for (sync = s->next; sync; sync = sync->next)
        request_bytes(sync);
}

static void command_trigger_or_prebuf_playback_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    struct playback_stream *s;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->output_streams, idx)) || s->type != PLAYBACK_STREAM) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    switch (command) {
        case PA_COMMAND_PREBUF_PLAYBACK_STREAM:
            pa_memblockq_prebuf_force(s->memblockq);
            break;
            
        case PA_COMMAND_TRIGGER_PLAYBACK_STREAM:
            pa_memblockq_prebuf_disable(s->memblockq);
            break;
            
        default:
            abort();
    }

    pa_sink_notify(s->sink_input->sink);
    pa_pstream_send_simple_ack(c->pstream, tag);
    request_bytes(s);
}

static void command_cork_record_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    struct record_stream *s;
    int b;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_get_boolean(t, &b) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->record_streams, idx))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_source_output_cork(s->source_output, b);
    pa_memblockq_prebuf_force(s->memblockq);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_flush_record_stream(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    struct record_stream *s;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->record_streams, idx))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_memblockq_flush(s->memblockq);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_default_sink_or_source(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    const char *s;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_gets(t, &s) < 0 || !s ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    pa_namereg_set_default(c->protocol->core, s, command == PA_COMMAND_SET_DEFAULT_SOURCE ? PA_NAMEREG_SOURCE : PA_NAMEREG_SINK);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_set_stream_name(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    const char *name;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        pa_tagstruct_gets(t, &name) < 0 || !name || 
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (command == PA_COMMAND_SET_PLAYBACK_STREAM_NAME) {
        struct playback_stream *s;
        
        if (!(s = pa_idxset_get_by_index(c->output_streams, idx)) || s->type != PLAYBACK_STREAM) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
            return;
        }

        pa_sink_input_set_name(s->sink_input, name);
        
    } else {
        struct record_stream *s;
        
        if (!(s = pa_idxset_get_by_index(c->record_streams, idx))) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
            return;
        }

        pa_source_output_set_name(s->source_output, name);
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_kill(PA_GCC_UNUSED pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (command == PA_COMMAND_KILL_CLIENT) {
        pa_client *client;
        
        if (!(client = pa_idxset_get_by_index(c->protocol->core->clients, idx))) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
            return;
        }

        pa_client_kill(client);
    } else if (command == PA_COMMAND_KILL_SINK_INPUT) {
        pa_sink_input *s;
        
        if (!(s = pa_idxset_get_by_index(c->protocol->core->sink_inputs, idx))) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
            return;
        }

        pa_sink_input_kill(s);
    } else {
        pa_source_output *s;

        assert(command == PA_COMMAND_KILL_SOURCE_OUTPUT);
        
        if (!(s = pa_idxset_get_by_index(c->protocol->core->source_outputs, idx))) {
            pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
            return;
        }

        pa_source_output_kill(s);
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_load_module(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_module *m;
    const char *name, *argument;
    pa_tagstruct *reply;
    assert(c && t);

    if (pa_tagstruct_gets(t, &name) < 0 || !name ||
        pa_tagstruct_gets(t, &argument) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(m = pa_module_load(c->protocol->core, name, argument))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_MODINITFAILED);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, m->index);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_unload_module(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t idx;
    pa_module *m;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &idx) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (!(m = pa_idxset_get_by_index(c->protocol->core->modules, idx))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_module_unload_request(m);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_add_autoload(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const char *name, *module, *argument;
    uint32_t type;
    uint32_t idx;
    pa_tagstruct *reply;
    assert(c && t);

    if (pa_tagstruct_gets(t, &name) < 0 || !name ||
        pa_tagstruct_getu32(t, &type) < 0 || type > 1 ||
        pa_tagstruct_gets(t, &module) < 0 || !module ||
        pa_tagstruct_gets(t, &argument) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (pa_autoload_add(c->protocol->core, name, type == 0 ? PA_NAMEREG_SINK : PA_NAMEREG_SOURCE, module, argument, &idx) < 0) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_EXIST);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, idx);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_remove_autoload(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const char *name = NULL;
    uint32_t type, idx = PA_IDXSET_INVALID;
    int r;
    assert(c && t);

    if ((pa_tagstruct_getu32(t, &idx) < 0 &&
        (pa_tagstruct_gets(t, &name) < 0 ||
         pa_tagstruct_getu32(t, &type) < 0)) ||
        (!name && idx == PA_IDXSET_INVALID) ||
        (name && type > 1) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    if (name) 
        r = pa_autoload_remove_by_name(c->protocol->core, name, type == 0 ? PA_NAMEREG_SINK : PA_NAMEREG_SOURCE);
    else
        r = pa_autoload_remove_by_index(c->protocol->core, idx);

    if (r < 0) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void autoload_fill_tagstruct(pa_tagstruct *t, const pa_autoload_entry *e) {
    assert(t && e);

    pa_tagstruct_putu32(t, e->index);
    pa_tagstruct_puts(t, e->name);
    pa_tagstruct_putu32(t, e->type == PA_NAMEREG_SINK ? 0 : 1);
    pa_tagstruct_puts(t, e->module);
    pa_tagstruct_puts(t, e->argument);
}

static void command_get_autoload_info(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const pa_autoload_entry *a = NULL;
    uint32_t type, idx;
    const char *name;
    pa_tagstruct *reply;
    assert(c && t);

    if ((pa_tagstruct_getu32(t, &idx) < 0 &&
        (pa_tagstruct_gets(t, &name) < 0 ||
         pa_tagstruct_getu32(t, &type) < 0)) ||
        (!name && idx == PA_IDXSET_INVALID) ||
        (name && type > 1) ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }


    if (name)
        a = pa_autoload_get_by_name(c->protocol->core, name, type == 0 ? PA_NAMEREG_SINK : PA_NAMEREG_SOURCE);
    else
        a = pa_autoload_get_by_index(c->protocol->core, idx);

    if (!a) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_NOENTITY);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    autoload_fill_tagstruct(reply, a);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_get_autoload_info_list(PA_GCC_UNUSED pa_pdispatch *pd, PA_GCC_UNUSED uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    pa_tagstruct *reply;
    assert(c && t);

    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
    
    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERR_ACCESS);
        return;
    }

    reply = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);

    if (c->protocol->core->autoload_hashmap) {
        pa_autoload_entry *a;
        void *state = NULL;

        while ((a = pa_hashmap_iterate(c->protocol->core->autoload_hashmap, &state, NULL)))
            autoload_fill_tagstruct(reply, a);
    }
    
    pa_pstream_send_tagstruct(c->pstream, reply);
}

/*** pstream callbacks ***/

static void pstream_packet_callback(pa_pstream *p, pa_packet *packet, void *userdata) {
    struct connection *c = userdata;
    assert(p && packet && packet->data && c);

    if (pa_pdispatch_run(c->pdispatch, packet, c) < 0) {
        pa_log(__FILE__": invalid packet.\n");
        connection_free(c);
    }
}

static void pstream_memblock_callback(pa_pstream *p, uint32_t channel, int64_t offset, pa_seek_mode_t seek, const pa_memchunk *chunk, void *userdata) {
    struct connection *c = userdata;
    struct output_stream *stream;
    assert(p && chunk && userdata);
    
    if (!(stream = pa_idxset_get_by_index(c->output_streams, channel))) {
        pa_log(__FILE__": client sent block for invalid stream.\n");
        connection_free(c);
        return;
    }

    if (stream->type == PLAYBACK_STREAM) {
        struct playback_stream *ps = (struct playback_stream*) stream;
        if (chunk->length >= ps->requested_bytes)
            ps->requested_bytes = 0;
        else
            ps->requested_bytes -= chunk->length;

        pa_memblockq_seek(ps->memblockq, offset, seek);

        if (pa_memblockq_push_align(ps->memblockq, chunk) < 0) {
            pa_tagstruct *t;
            
            pa_log_warn(__FILE__": failed to push data into queue\n");

            /* Pushing this block into the queue failed, so we simulate
             * it by skipping ahead */
            
            pa_memblockq_seek(ps->memblockq, chunk->length, PA_SEEK_RELATIVE);

            /* Notify the user */
            t = pa_tagstruct_new(NULL, 0);
            pa_tagstruct_putu32(t, PA_COMMAND_OVERFLOW);
            pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
            pa_tagstruct_putu32(t, ps->index);
            pa_pstream_send_tagstruct(p, t);
        }

        ps->underrun = 0;
            
        pa_sink_notify(ps->sink_input->sink);

    } else {
        struct upload_stream *u = (struct upload_stream*) stream;
        size_t l;
        assert(u->type == UPLOAD_STREAM);

        if (!u->memchunk.memblock) {
            if (u->length == chunk->length) {
                u->memchunk = *chunk;
                pa_memblock_ref(u->memchunk.memblock);
                u->length = 0;
            } else {
                u->memchunk.memblock = pa_memblock_new(u->length, c->protocol->core->memblock_stat);
                u->memchunk.index = u->memchunk.length = 0;
            }
        }
        
        assert(u->memchunk.memblock);
        
        l = u->length; 
        if (l > chunk->length)
            l = chunk->length;

        if (l > 0) {
            memcpy((uint8_t*) u->memchunk.memblock->data + u->memchunk.index + u->memchunk.length,
                   (uint8_t*) chunk->memblock->data+chunk->index, l);
            u->memchunk.length += l;
            u->length -= l;
        }
    }
}

static void pstream_die_callback(pa_pstream *p, void *userdata) {
    struct connection *c = userdata;
    assert(p && c);
    connection_free(c);

/*    pa_log(__FILE__": connection died.\n");*/
}


static void pstream_drain_callback(pa_pstream *p, void *userdata) {
    struct connection *c = userdata;
    assert(p && c);

    send_memblock(c);
}

/*** client callbacks ***/

static void client_kill_cb(pa_client *c) {
    assert(c && c->userdata);
    connection_free(c->userdata);
}

/*** socket server callbacks ***/

static void auth_timeout(pa_mainloop_api*m, pa_time_event *e, const struct timeval *tv, void *userdata) {
    struct connection *c = userdata;
    assert(m && tv && c && c->auth_timeout_event == e);

    if (!c->authorized)
        connection_free(c);
}

static void on_connection(PA_GCC_UNUSED pa_socket_server*s, pa_iochannel *io, void *userdata) {
    pa_protocol_native *p = userdata;
    struct connection *c;
    assert(io && p);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log_warn(__FILE__": Warning! Too many connections (%u), dropping incoming connection.\n", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_xmalloc(sizeof(struct connection));

    c->authorized =!! p->public;

    if (!c->authorized) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec += AUTH_TIMEOUT;
        c->auth_timeout_event = p->core->mainloop->time_new(p->core->mainloop, &tv, auth_timeout, c);
    } else
        c->auth_timeout_event = NULL;
    
    c->protocol = p;
    assert(p->core);
    c->client = pa_client_new(p->core, __FILE__, "Client");
    assert(c->client);
    c->client->kill = client_kill_cb;
    c->client->userdata = c;
    c->client->owner = p->module;
    
    c->pstream = pa_pstream_new(p->core->mainloop, io, p->core->memblock_stat);
    assert(c->pstream);

    pa_pstream_set_recieve_packet_callback(c->pstream, pstream_packet_callback, c);
    pa_pstream_set_recieve_memblock_callback(c->pstream, pstream_memblock_callback, c);
    pa_pstream_set_die_callback(c->pstream, pstream_die_callback, c);
    pa_pstream_set_drain_callback(c->pstream, pstream_drain_callback, c);

    c->pdispatch = pa_pdispatch_new(p->core->mainloop, command_table, PA_COMMAND_MAX);
    assert(c->pdispatch);

    c->record_streams = pa_idxset_new(NULL, NULL);
    c->output_streams = pa_idxset_new(NULL, NULL);
    assert(c->record_streams && c->output_streams);

    c->rrobin_index = PA_IDXSET_INVALID;
    c->subscription = NULL;

    pa_idxset_put(p->connections, c, NULL);
}

/*** module entry points ***/

static int load_key(pa_protocol_native*p, const char*fn) {
    assert(p);

    p->auth_cookie_in_property = 0;
    
    if (!fn && pa_authkey_prop_get(p->core, PA_NATIVE_COOKIE_PROPERTY_NAME, p->auth_cookie, sizeof(p->auth_cookie)) >= 0) {
        pa_log_info(__FILE__": using already loaded auth cookie.\n");
        pa_authkey_prop_ref(p->core, PA_NATIVE_COOKIE_PROPERTY_NAME);
        p->auth_cookie_in_property = 1;
        return 0;
    }
    
    if (!fn)
        fn = PA_NATIVE_COOKIE_FILE;

    if (pa_authkey_load_auto(fn, p->auth_cookie, sizeof(p->auth_cookie)) < 0)
        return -1;

    pa_log_info(__FILE__": loading cookie from disk.\n");

    if (pa_authkey_prop_put(p->core, PA_NATIVE_COOKIE_PROPERTY_NAME, p->auth_cookie, sizeof(p->auth_cookie)) >= 0)
        p->auth_cookie_in_property = 1;
        
    return 0;
}

static pa_protocol_native* protocol_new_internal(pa_core *c, pa_module *m, pa_modargs *ma) {
    pa_protocol_native *p;
    int public = 0;
    assert(c && ma);

    if (pa_modargs_get_value_boolean(ma, "public", &public) < 0) {
        pa_log(__FILE__": public= expects a boolean argument.\n");
        return NULL;
    }
    
    p = pa_xmalloc(sizeof(pa_protocol_native));
    p->core = c;
    p->module = m;
    p->public = public;
    p->server = NULL;

    if (load_key(p, pa_modargs_get_value(ma, "cookie", NULL)) < 0) {
        pa_xfree(p);
        return NULL;
    }

    p->connections = pa_idxset_new(NULL, NULL);
    assert(p->connections);

    return p;
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
    struct connection *c;
    assert(p);

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_free(c);
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

    pa_xfree(p);
}

pa_protocol_native* pa_protocol_native_new_iochannel(pa_core*core, pa_iochannel *io, pa_module *m, pa_modargs *ma) {
    pa_protocol_native *p;

    if (!(p = protocol_new_internal(core, m, ma)))
        return NULL;

    on_connection(NULL, io, p);
    
    return p;
}
