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

struct connection;
struct pa_protocol_native;

struct record_stream {
    struct connection *connection;
    uint32_t index;
    struct pa_source_output *source_output;
    struct pa_memblockq *memblockq;
};

struct playback_stream {
    struct connection *connection;
    uint32_t index;
    size_t qlength;
    struct pa_sink_input *sink_input;
    struct pa_memblockq *memblockq;
    size_t requested_bytes;
};

struct connection {
    int authorized;
    struct pa_protocol_native *protocol;
    struct pa_client *client;
    struct pa_pstream *pstream;
    struct pa_pdispatch *pdispatch;
    struct pa_idxset *record_streams, *playback_streams;
};

struct pa_protocol_native {
    int public;
    struct pa_core *core;
    struct pa_socket_server *server;
    struct pa_idxset *connections;
};

static int sink_input_peek_cb(struct pa_sink_input *i, struct pa_memchunk *chunk);
static void sink_input_drop_cb(struct pa_sink_input *i, size_t length);
static void sink_input_kill_cb(struct pa_sink_input *i);
static uint32_t sink_input_get_latency_cb(struct pa_sink_input *i);

static void request_bytes(struct playback_stream*s);

static int command_exit(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static int command_create_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
static int command_delete_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);

static const struct pa_pdispatch_command command_table[PA_COMMAND_MAX] = {
    [PA_COMMAND_ERROR] = { NULL },
    [PA_COMMAND_REPLY] = { NULL },
    [PA_COMMAND_CREATE_PLAYBACK_STREAM] = { command_create_playback_stream },
    [PA_COMMAND_DELETE_PLAYBACK_STREAM] = { command_delete_playback_stream },
    [PA_COMMAND_CREATE_RECORD_STREAM] = { NULL },
    [PA_COMMAND_DELETE_RECORD_STREAM] = { NULL },
    [PA_COMMAND_EXIT] = { command_exit },
};

/* structure management */

static void record_stream_free(struct record_stream* r) {
    assert(r && r->connection);

    pa_idxset_remove_by_data(r->connection->record_streams, r, NULL);
    pa_source_output_free(r->source_output);
    pa_memblockq_free(r->memblockq);
    free(r);
}

static struct playback_stream* playback_stream_new(struct connection *c, struct pa_sink *sink, struct pa_sample_spec *ss, const char *name, size_t qlen, size_t maxlength, size_t prebuf) {
    struct playback_stream *s;
    assert(c && sink && ss && name && qlen && maxlength && prebuf);

    s = malloc(sizeof(struct playback_stream));
    assert (s);
    s->connection = c;
    s->qlength = qlen;
    
    s->sink_input = pa_sink_input_new(sink, name, ss);
    assert(s->sink_input);
    s->sink_input->peek = sink_input_peek_cb;
    s->sink_input->drop = sink_input_drop_cb;
    s->sink_input->kill = sink_input_kill_cb;
    s->sink_input->get_latency = sink_input_get_latency_cb;
    s->sink_input->userdata = s;
    
    s->memblockq = pa_memblockq_new(maxlength, pa_sample_size(ss), prebuf);
    assert(s->memblockq);

    s->requested_bytes = 0;
    
    pa_idxset_put(c->playback_streams, s, &s->index);
    return s;
}

static void playback_stream_free(struct playback_stream* p) {
    assert(p && p->connection);

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

    if (!(l = pa_memblockq_missing_to(s->memblockq, s->qlength)))
        return;

    if (l <= s->requested_bytes)
        return;

    l -= s->requested_bytes;
    s->requested_bytes += l;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_REQUEST);
    pa_tagstruct_putu32(t, (uint32_t) -1); /* tag */
    pa_tagstruct_putu32(t, s->index);
    pa_tagstruct_putu32(t, l);
    pa_pstream_send_tagstruct(s->connection->pstream, t);

/*    fprintf(stderr, "Requesting %u bytes\n", l);*/
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
}

static void sink_input_kill_cb(struct pa_sink_input *i) {
    struct playback_stream *s;
    assert(i && i->userdata);
    s = i->userdata;

    playback_stream_free(s);
}

static uint32_t sink_input_get_latency_cb(struct pa_sink_input *i) {
    struct playback_stream *s;
    assert(i && i->userdata);
    s = i->userdata;

    return pa_samples_usec(pa_memblockq_get_length(s->memblockq), &s->sink_input->sample_spec);
}

/*** pdispatch callbacks ***/

static int command_create_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *s;
    size_t maxlength, prebuf, qlength;
    uint32_t sink_index;
    const char *name;
    struct pa_sample_spec ss;
    struct pa_tagstruct *reply;
    struct pa_sink *sink;
    assert(c && t && c->protocol && c->protocol->core);
    
    if (pa_tagstruct_gets(t, &name) < 0 ||
        pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
        pa_tagstruct_getu32(t, &sink_index) < 0 ||
        pa_tagstruct_getu32(t, &qlength) < 0 ||
        pa_tagstruct_getu32(t, &maxlength) < 0 ||
        pa_tagstruct_getu32(t, &prebuf) < 0 ||
        !pa_tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return 0;
    }

    if (sink_index == (uint32_t) -1)
        sink = pa_sink_get_default(c->protocol->core);
    else
        sink = pa_idxset_get_by_index(c->protocol->core->sinks, sink_index);

    if (!sink) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_EXIST);
        return 0;
    }
    
    if (!(s = playback_stream_new(c, sink, &ss, name, qlength, maxlength, prebuf))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_INVALID);
        return 0;
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
    return 0;
}

static int command_delete_playback_stream(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t channel;
    struct playback_stream *s;
    assert(c && t);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return 0;
    }
    
    if (!(s = pa_idxset_get_by_index(c->playback_streams, channel))) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_EXIST);
        return 0;
    }

    pa_pstream_send_simple_ack(c->pstream, tag);
    return 0;
}

static int command_exit(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    assert(c && t);
    
    if (!pa_tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        pa_pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return 0;
    }
    
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop);
    c->protocol->core->mainloop->quit(c->protocol->core->mainloop, 0);
    pa_pstream_send_simple_ack(c->pstream, tag); /* nonsense */
    return 0;
}

/*** pstream callbacks ***/

static int packet_callback(struct pa_pstream *p, struct pa_packet *packet, void *userdata) {
    struct connection *c = userdata;
    assert(p && packet && packet->data && c);

    if (pa_pdispatch_run(c->pdispatch, packet, c) < 0) {
        fprintf(stderr, "protocol-native: invalid packet.\n");
        return -1;
    }
    
    return 0;
}

static int memblock_callback(struct pa_pstream *p, uint32_t channel, int32_t delta, struct pa_memchunk *chunk, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *stream;
    assert(p && chunk && userdata);

    if (!(stream = pa_idxset_get_by_index(c->playback_streams, channel))) {
        fprintf(stderr, "protocol-native: client sent block for invalid stream.\n");
        return -1;
    }

    if (chunk->length >= stream->requested_bytes)
        stream->requested_bytes = 0;
    else
        stream->requested_bytes -= chunk->length;
    
    pa_memblockq_push_align(stream->memblockq, chunk, delta);
    assert(stream->sink_input);
    pa_sink_notify(stream->sink_input->sink);

    /*fprintf(stderr, "Recieved %u bytes.\n", chunk->length);*/

    return 0;
}

static void die_callback(struct pa_pstream *p, void *userdata) {
    struct connection *c = userdata;
    assert(p && c);
    connection_free(c);

    fprintf(stderr, "protocol-native: connection died.\n");
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
    c->pstream = pa_pstream_new(p->core->mainloop, io);
    assert(c->pstream);

    pa_pstream_set_recieve_packet_callback(c->pstream, packet_callback, c);
    pa_pstream_set_recieve_memblock_callback(c->pstream, memblock_callback, c);
    pa_pstream_set_die_callback(c->pstream, die_callback, c);

    c->pdispatch = pa_pdispatch_new(p->core->mainloop, command_table, PA_COMMAND_MAX);
    assert(c->pdispatch);

    c->record_streams = pa_idxset_new(NULL, NULL);
    c->playback_streams = pa_idxset_new(NULL, NULL);
    assert(c->record_streams && c->playback_streams);

    pa_idxset_put(p->connections, c, NULL);
}

/*** module entry points ***/

struct pa_protocol_native* pa_protocol_native_new(struct pa_core *core, struct pa_socket_server *server) {
    struct pa_protocol_native *p;
    assert(core && server);

    p = malloc(sizeof(struct pa_protocol_native));
    assert(p);

    p->public = 1;
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
