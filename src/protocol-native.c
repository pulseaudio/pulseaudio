#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#include "protocol-native.h"
#include "protocol-native-spec.h"
#include "packet.h"
#include "client.h"
#include "sourceoutput.h"
#include "sinkinput.h"
#include "pstream.h"
#include "tagstruct.h"
#include "pdispatch.h"
#include "pstream-util.h"
#include "authkey.h"
#include "namereg.h"

struct connection;
struct pa_protocol_native;

struct record_stream {
    struct connection *connection;
    uint32_t index;
    struct pa_source_output *source_output;
    struct pa_memblockq *memblockq;
    size_t fragment_size;
};

struct playback_stream {
    struct connection *connection;
    uint32_t index;
    struct pa_sink_input *sink_input;
    struct pa_memblockq *memblockq;
    size_t requested_bytes;
    int drain_request;
    uint32_t drain_tag;
};

struct connection {
    int authorized;
    struct pa_protocol_native *protocol;
    struct pa_client *client;
    struct pa_pstream *pstream;
    struct pa_pdispatch *pdispatch;
    struct pa_idxset *record_streams, *playback_streams;
    uint32_t rrobin_index;
};

struct pa_protocol_native {
    struct pa_module *module;
    int public;
    struct pa_core *core;
    struct pa_socket_server *server;
    struct pa_idxset *connections;
    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];
};

static int sink_input_peek_cb(struct pa_sink_input *i, struct pa_memchunk *chunk);
static void sink_input_drop_cb(struct pa_sink_input *i, size_t length);
static void sink_input_kill_cb(struct pa_sink_input *i);
static uint32_t sink_input_get_latency_cb(struct pa_sink_input *i);

static void request_bytes(struct playback_stream*s);

static void source_output_kill_cb(struct pa_source_output *o);
static void source_output_push_cb(struct pa_source_output *o, const struct pa_memchunk *chunk);

static void command_exit(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_create_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_delete_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_drain_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_create_record_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_delete_record_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_auth(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_set_name(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static void command_lookup(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);

static const struct pa_pdispatch_command command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_ERROR] = { NULL },
    [PA_COMMAND_TIMEOUT] = { NULL },
    [PA_COMMAND_REPLY] = { NULL },
    [PA_COMMAND_CREATE_PLAYBACK_STREAM] = { command_create_playback_stream },
    [PA_COMMAND_DELETE_PLAYBACK_STREAM] = { command_delete_playback_stream },
    [PA_COMMAND_DRAIN_PLAYBACK_STREAM] = { command_drain_playback_stream },
    [PA_COMMAND_CREATE_RECORD_STREAM] = { command_create_record_stream },
    [PA_COMMAND_DELETE_RECORD_STREAM] = { command_delete_record_stream },
    [PA_COMMAND_AUTH] = { command_auth },
    [PA_COMMAND_REQUEST] = { NULL },
    [PA_COMMAND_EXIT] = { command_exit },
    [PA_COMMAND_SET_NAME] = { command_set_name },
    [PA_COMMAND_LOOKUP_SINK] = { command_lookup },
    [PA_COMMAND_LOOKUP_SOURCE] = { command_lookup },
};

/* structure management */

static struct record_stream* record_stream_new(struct connection *c, struct pa_source *source, struct pa_sample_spec *ss, const char *name, size_t maxlength, size_t fragment_size) {
    struct record_stream *s;
    struct pa_source_output *source_output;
    size_t base;
    assert(c && source && ss && name && maxlength);

    if (!(source_output = pa_source_output_new(source, name, ss)))
        return NULL;

    s = malloc(sizeof(struct record_stream));
    assert(s);
    s->connection = c;
    s->source_output = source_output;
    s->source_output->push = source_output_push_cb;
    s->source_output->kill = source_output_kill_cb;
    s->source_output->userdata = s;
    s->source_output->owner = c->protocol->module;
    s->source_output->client = c->client;

    s->memblockq = pa_memblockq_new(maxlength, 0, base = pa_sample_size(ss), 0, 0);
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
    pa_source_output_free(r->source_output);
    pa_memblockq_free(r->memblockq);
    free(r);
}

static struct playback_stream* playback_stream_new(struct connection *c, struct pa_sink *sink, struct pa_sample_spec *ss, const char *name,
                                                   size_t maxlength,
                                                   size_t tlength,
                                                   size_t prebuf,
                                                   size_t minreq) {
    struct playback_stream *s;
    struct pa_sink_input *sink_input;
    assert(c && sink && ss && name && maxlength);

    if (!(sink_input = pa_sink_input_new(sink, name, ss)))
        return NULL;
    
    s = malloc(sizeof(struct playback_stream));
    assert (s);
    s->connection = c;
    s->sink_input = sink_input;
    
    s->sink_input->peek = sink_input_peek_cb;
    s->sink_input->drop = sink_input_drop_cb;
    s->sink_input->kill = sink_input_kill_cb;
    s->sink_input->get_latency = sink_input_get_latency_cb;
    s->sink_input->userdata = s;
    s->sink_input->owner = c->protocol->module;
    s->sink_input->client = c->client;
    
    s->memblockq = pa_memblockq_new(maxlength, tlength, pa_sample_size(ss), prebuf, minreq);
    assert(s->memblockq);

    s->requested_bytes = 0;
    s->drain_request = 0;
    
    pa_idxset_put(c->playback_streams, s, &s->index);
    return s;
}

static void playback_stream_free(struct playback_stream* p) {
    assert(p && p->connection);

    if (p->drain_request)
        pa_pstream_send_error(p->connection->pstream, p->drain_tag, PA_ERROR_NOENTITY);

    pa_idxset_remove_by_data(p->connection->playback_streams, p, NULL);
    pa_sink_input_free(p->sink_input);
    pa_memblockq_free(p->memblockq);
    free(p);
}

static void connection_free(struct connection *c) {
    struct record_stream *r;
    struct playback_stream *p;
    assert(c && c->protocol);

    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);
    while ((r = pa_idxset_first(c->record_streams, NULL)))
        record_stream_free(r);
    pa_idxset_free(c->record_streams, NULL, NULL);

    while ((p = pa_idxset_first(c->playback_streams, NULL)))
        playback_stream_free(p);
    pa_idxset_free(c->playback_streams, NULL, NULL);

    pa_pdispatch_free(c->pdispatch);
    pa_pstream_free(c->pstream);
    pa_client_free(c->client);
    free(c);
}

static void request_bytes(struct playback_stream *s) {
    struct pa_tagstruct *t;
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

    /*fprintf(stderr, "Requesting %u bytes\n", l);*/
}

static void send_memblock(struct connection *c) {
    uint32_t start;
    struct record_stream *r;

    start = PA_IDXSET_INVALID;
    for (;;) {
        struct pa_memchunk chunk;
        
        if (!(r = pa_idxset_rrobin(c->record_streams, &c->rrobin_index)))
            return;

        if (start == PA_IDXSET_INVALID)
            start = c->rrobin_index;
        else if (start == c->rrobin_index)
            return;

        if (pa_memblockq_peek(r->memblockq,  &chunk) >= 0) {
            if (chunk.length > r->fragment_size)
                chunk.length = r->fragment_size;

            pa_pstream_send_memblock(c->pstream, r->index, 0, &chunk);
            pa_memblockq_drop(r->memblockq, chunk.length);
            pa_memblock_unref(chunk.memblock);
            
            return;
        }
    }
}

static void send_playback_stream_killed(struct playback_stream *p) {
    struct pa_tagstruct *t;
    assert(p);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_PLAYBACK_STREAM_KILLED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, p->index);
    pa_pstream_send_tagstruct(p->connection->pstream, t);
}

static void send_record_stream_killed(struct record_stream *r) {
    struct pa_tagstruct *t;
    assert(r);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_RECORD_STREAM_KILLED);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, r->index);
    pa_pstream_send_tagstruct(r->connection->pstream, t);
}


/*** sinkinput callbacks ***/

static int sink_input_peek_cb(struct pa_sink_input *i, struct pa_memchunk *chunk) {
    struct playback_stream *s;
    assert(i && i->userdata && chunk);
    s = i->userdata;

    if (pa_memblockq_peek(s->memblockq, chunk) < 0)
        return -1;

    return 0;
}

static void sink_input_drop_cb(struct pa_sink_input *i, size_t length) {
    struct playback_stream *s;
    assert(i && i->userdata && length);
    s = i->userdata;

    pa_memblockq_drop(s->memblockq, length);
    request_bytes(s);

    if (s->drain_request && !pa_memblockq_is_readable(s->memblockq)) {
        pa_pstream_send_simple_ack(s->connection->pstream, s->drain_tag);
        s->drain_request = 0;
    }
}

static void sink_input_kill_cb(struct pa_sink_input *i) {
    assert(i && i->userdata);
    send_playback_stream_killed((struct playback_stream *) i->userdata);
    playback_stream_free((struct playback_stream *) i->userdata);
}

static uint32_t sink_input_get_latency_cb(struct pa_sink_input *i) {
    struct playback_stream *s;
    assert(i && i->userdata);
    s = i->userdata;

    return pa_samples_usec(pa_memblockq_get_length(s->memblockq), &s->sink_input->sample_spec);
}

/*** source_output callbacks ***/

static void source_output_push_cb(struct pa_source_output *o, const struct pa_memchunk *chunk) {
    struct record_stream *s;
    assert(o && o->userdata && chunk);
    s = o->userdata;
    
    pa_memblockq_push(s->memblockq, chunk, 0);
    if (!pa_pstream_is_pending(s->connection->pstream))
        send_memblock(s->connection);
}

static void source_output_kill_cb(struct pa_source_output *o) {
    assert(o && o->userdata);
    send_record_stream_killed((struct record_stream *) o->userdata);
    record_stream_free((struct record_stream *) o->userdata);
}

/*** pdispatch callbacks ***/

static void protocol_error(struct connection *c) {
    fprintf(stderr, __FILE__": protocol error, kicking client\n");
    connection_free(c);
}

static void command_create_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *s;
    size_t maxlength, tlength, prebuf, minreq;
    uint32_t sink_index;
    const char *name;
    struct pa_sample_spec ss;
    struct pa_tagstruct *reply;
    struct pa_sink *sink;
    assert(c && t && c->protocol && c->protocol->core);
    
    if (pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
        pa_tagstruct_getu32(t, &sink_index) < 0 ||
        pa_tagstruct_getu32(t, &maxlength) < 0 ||
        pa_tagstruct_getu32(t, &tlength) < 0 ||
        pa_tagstruct_getu32(t, &prebuf) < 0 ||
        pa_tagstruct_getu32(t, &minreq) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }

    if (sink_index == (uint32_t) -1)
        sink = pa_sink_get_default(c->protocol->core);
    else
        sink = pa_idxset_get_by_index(c->protocol->core->sinks, sink_index);

    if (!sink) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_NOENTITY);
        return;
    }
    
    if (!(s = playback_stream_new(c, sink, &ss, name, maxlength, tlength, prebuf, minreq))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_INVALID);
        return;
    }
    
    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, s->index);
    assert(s->sink_input);
    pa_tagstruct_putu32(reply, s->sink_input->index);
    pa_pstream_send_tagstruct(c->pstream, reply);
    request_bytes(s);
}

static void command_delete_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t channel;
    struct playback_stream *s;
    assert(c && t);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }
    
    if (!(s = pa_idxset_get_by_index(c->playback_streams, channel))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_EXIST);
        return;
    }

    playback_stream_free(s);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_create_record_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    struct record_stream *s;
    size_t maxlength, fragment_size;
    uint32_t source_index;
    const char *name;
    struct pa_sample_spec ss;
    struct pa_tagstruct *reply;
    struct pa_source *source;
    assert(c && t && c->protocol && c->protocol->core);
    
    if (pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
        pa_tagstruct_getu32(t, &source_index) < 0 ||
        pa_tagstruct_getu32(t, &maxlength) < 0 ||
        pa_tagstruct_getu32(t, &fragment_size) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }

    if (source_index == (uint32_t) -1)
        source = pa_source_get_default(c->protocol->core);
    else
        source = pa_idxset_get_by_index(c->protocol->core->sources, source_index);

    if (!source) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_NOENTITY);
        return;
    }
    
    if (!(s = record_stream_new(c, source, &ss, name, maxlength, fragment_size))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_INVALID);
        return;
    }
    
    reply = pa_tagstruct_new(NULL, 0);
    assert(reply);
    pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(reply, tag);
    pa_tagstruct_putu32(reply, s->index);
    assert(s->source_output);
    pa_tagstruct_putu32(reply, s->source_output->index);
    pa_pstream_send_tagstruct(c->pstream, reply);
}

static void command_delete_record_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t channel;
    struct record_stream *s;
    assert(c && t);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }
    
    if (!(s = pa_idxset_get_by_index(c->record_streams, channel))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_EXIST);
        return;
    }

    record_stream_free(s);
    pa_pstream_send_simple_ack(c->pstream, tag);
}

static void command_exit(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    assert(c && t);
    
    if (!pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }
    
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop);
    c->protocol->core->mainloop->quit(c->protocol->core->mainloop, 0);
    pa_pstream_send_simple_ack(c->pstream, tag); /* nonsense */
    return;
}

static void command_auth(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const void*cookie;
    assert(c && t);

    if (pa_tagstruct_get_arbitrary(t, &cookie, PA_NATIVE_COOKIE_LENGTH) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }
        
    if (memcmp(c->protocol->auth_cookie, cookie, PA_NATIVE_COOKIE_LENGTH) != 0) {
        fprintf(stderr, "protocol-native.c: Denied access to client with invalid authorization key.\n");
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }

    c->authorized = 1;
    pa_pstream_send_simple_ack(c->pstream, tag);
    return;
}

static void command_set_name(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const char *name;
    assert(c && t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    pa_client_rename(c->client, name);
    pa_pstream_send_simple_ack(c->pstream, tag);
    return;
}

static void command_lookup(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    const char *name;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c && t);

    if (pa_tagstruct_gets(t, &name) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }

    if (command == PA_COMMAND_LOOKUP_SINK) {
        struct pa_sink *sink;
        if ((sink = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SINK)))
            index = sink->index;
    } else {
        struct pa_source *source;
        assert(command == PA_COMMAND_LOOKUP_SOURCE);
        if ((source = pa_namereg_get(c->protocol->core, name, PA_NAMEREG_SOURCE)))
            index = source->index;
    }

    if (index == PA_IDXSET_INVALID)
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_NOENTITY);
    else {
        struct pa_tagstruct *reply;
        reply = pa_tagstruct_new(NULL, 0);
        assert(reply);
        pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
        pa_tagstruct_putu32(reply, tag);
        pa_tagstruct_putu32(reply, index);
        pa_pstream_send_tagstruct(c->pstream, reply);
    }
}

static void command_drain_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t index;
    struct playback_stream *s;
    assert(c && t);

    if (pa_tagstruct_getu32(t, &index) < 0 ||
        !pa_tagstruct_eof(t)) {
        protocol_error(c);
        return;
    }

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return;
    }

    if (!(s = pa_idxset_get_by_index(c->playback_streams, index))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_NOENTITY);
        return;
    }

    s->drain_request = 0;
    
    if (!pa_memblockq_is_readable(s->memblockq))
        pa_pstream_send_simple_ack(c->pstream, tag);
    else {
        s->drain_request = 1;
        s->drain_tag = tag;
    }
} 

/*** pstream callbacks ***/

static void pstream_packet_callback(struct pa_pstream *p, struct pa_packet *packet, void *userdata) {
    struct connection *c = userdata;
    assert(p && packet && packet->data && c);

    if (pa_pdispatch_run(c->pdispatch, packet, c) < 0) {
        fprintf(stderr, "protocol-native: invalid packet.\n");
        connection_free(c);
    }
}

static void pstream_memblock_callback(struct pa_pstream *p, uint32_t channel, int32_t delta, const struct pa_memchunk *chunk, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *stream;
    assert(p && chunk && userdata);

    if (!(stream = pa_idxset_get_by_index(c->playback_streams, channel))) {
        fprintf(stderr, "protocol-native: client sent block for invalid stream.\n");
        connection_free(c);
        return;
    }

    if (chunk->length >= stream->requested_bytes)
        stream->requested_bytes = 0;
    else
        stream->requested_bytes -= chunk->length;
    
    pa_memblockq_push_align(stream->memblockq, chunk, delta);
    assert(stream->sink_input);
    pa_sink_notify(stream->sink_input->sink);

    /*fprintf(stderr, "Recieved %u bytes.\n", chunk->length);*/
}

static void pstream_die_callback(struct pa_pstream *p, void *userdata) {
    struct connection *c = userdata;
    assert(p && c);
    connection_free(c);

    fprintf(stderr, "protocol-native: connection died.\n");
}


static void pstream_drain_callback(struct pa_pstream *p, void *userdata) {
    struct connection *c = userdata;
    assert(p && c);

    send_memblock(c);
}

/*** client callbacks ***/

static void client_kill_cb(struct pa_client *c) {
    assert(c && c->userdata);
    connection_free(c->userdata);
}

/*** socket server callbacks ***/

static void on_connection(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata) {
    struct pa_protocol_native *p = userdata;
    struct connection *c;
    assert(s && io && p);

    c = malloc(sizeof(struct connection));
    assert(c);
    c->authorized = p->public;
    c->protocol = p;
    assert(p->core);
    c->client = pa_client_new(p->core, "NATIVE", "Client");
    assert(c->client);
    c->client->kill = client_kill_cb;
    c->client->userdata = c;
    c->client->owner = p->module;
    
    c->pstream = pa_pstream_new(p->core->mainloop, io);
    assert(c->pstream);

    pa_pstream_set_recieve_packet_callback(c->pstream, pstream_packet_callback, c);
    pa_pstream_set_recieve_memblock_callback(c->pstream, pstream_memblock_callback, c);
    pa_pstream_set_die_callback(c->pstream, pstream_die_callback, c);
    pa_pstream_set_drain_callback(c->pstream, pstream_drain_callback, c);

    c->pdispatch = pa_pdispatch_new(p->core->mainloop, command_table, PA_COMMAND_MAX);
    assert(c->pdispatch);

    c->record_streams = pa_idxset_new(NULL, NULL);
    c->playback_streams = pa_idxset_new(NULL, NULL);
    assert(c->record_streams && c->playback_streams);

    c->rrobin_index = PA_IDXSET_INVALID;

    pa_idxset_put(p->connections, c, NULL);
}

/*** module entry points ***/

struct pa_protocol_native* pa_protocol_native_new(struct pa_core *core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma) {
    struct pa_protocol_native *p;
    uint32_t public;
    assert(core && server && ma);

    if (pa_modargs_get_value_u32(ma, "public", &public) < 0) {
        fprintf(stderr, __FILE__": public= expects numeric argument.\n");
        return NULL;
    }
    
    p = malloc(sizeof(struct pa_protocol_native));
    assert(p);

    if (pa_authkey_load_from_home(pa_modargs_get_value(ma, "cookie", PA_NATIVE_COOKIE_FILE), p->auth_cookie, sizeof(p->auth_cookie)) < 0) {
        free(p);
        return NULL;
    }

    p->module = m;
    p->public = public;
    p->server = server;
    p->core = core;
    p->connections = pa_idxset_new(NULL, NULL);
    assert(p->connections);

    pa_socket_server_set_callback(p->server, on_connection, p);
    
    return p;
}

void pa_protocol_native_free(struct pa_protocol_native *p) {
    struct connection *c;
    assert(p);

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_free(c);
    pa_idxset_free(p->connections, NULL, NULL);
    pa_socket_server_free(p->server);
    free(p);
}
