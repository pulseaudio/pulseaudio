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
struct protocol_native;

struct record_stream {
    struct connection *connection;
    uint32_t index;
    struct source_output *source_output;
    struct memblockq *memblockq;
};

struct playback_stream {
    struct connection *connection;
    uint32_t index;
    size_t qlength;
    struct sink_input *sink_input;
    struct memblockq *memblockq;
    size_t requested_bytes;
};

struct connection {
    int authorized;
    struct protocol_native *protocol;
    struct client *client;
    struct pstream *pstream;
    struct pdispatch *pdispatch;
    struct idxset *record_streams, *playback_streams;
};

struct protocol_native {
    int public;
    struct core *core;
    struct socket_server *server;
    struct idxset *connections;
};

static int sink_input_peek_cb(struct sink_input *i, struct memchunk *chunk);
static void sink_input_drop_cb(struct sink_input *i, size_t length);
static void sink_input_kill_cb(struct sink_input *i);
static uint32_t sink_input_get_latency_cb(struct sink_input *i);

static void request_bytes(struct playback_stream*s);

static int command_exit(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata);
static int command_create_playback_stream(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata);
static int command_delete_playback_stream(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata);

static const struct pdispatch_command command_table[PA_COMMAND_MAX] = {
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

    idxset_remove_by_data(r->connection->record_streams, r, NULL);
    source_output_free(r->source_output);
    memblockq_free(r->memblockq);
    free(r);
}

static struct playback_stream* playback_stream_new(struct connection *c, struct sink *sink, struct pa_sample_spec *ss, const char *name, size_t qlen, size_t maxlength, size_t prebuf) {
    struct playback_stream *s;
    assert(c && sink && ss && name && qlen && maxlength && prebuf);

    s = malloc(sizeof(struct playback_stream));
    assert (s);
    s->connection = c;
    s->qlength = qlen;
    
    s->sink_input = sink_input_new(sink, ss, name);
    assert(s->sink_input);
    s->sink_input->peek = sink_input_peek_cb;
    s->sink_input->drop = sink_input_drop_cb;
    s->sink_input->kill = sink_input_kill_cb;
    s->sink_input->get_latency = sink_input_get_latency_cb;
    s->sink_input->userdata = s;
    
    s->memblockq = memblockq_new(maxlength, pa_sample_size(ss), prebuf);
    assert(s->memblockq);

    s->requested_bytes = 0;
    
    idxset_put(c->playback_streams, s, &s->index);
    return s;
}

static void playback_stream_free(struct playback_stream* p) {
    assert(p && p->connection);

    idxset_remove_by_data(p->connection->playback_streams, p, NULL);
    sink_input_free(p->sink_input);
    memblockq_free(p->memblockq);
    free(p);
}

static void connection_free(struct connection *c) {
    struct record_stream *r;
    struct playback_stream *p;
    assert(c && c->protocol);

    idxset_remove_by_data(c->protocol->connections, c, NULL);
    while ((r = idxset_first(c->record_streams, NULL)))
        record_stream_free(r);
    idxset_free(c->record_streams, NULL, NULL);

    while ((p = idxset_first(c->playback_streams, NULL)))
        playback_stream_free(p);
    idxset_free(c->playback_streams, NULL, NULL);

    pdispatch_free(c->pdispatch);
    pstream_free(c->pstream);
    client_free(c->client);
    free(c);
}

static void request_bytes(struct playback_stream *s) {
    struct tagstruct *t;
    size_t l;
    assert(s);

    if (!(l = memblockq_missing_to(s->memblockq, s->qlength)))
        return;

    if (l <= s->requested_bytes)
        return;

    l -= s->requested_bytes;
    s->requested_bytes += l;

    t = tagstruct_new(NULL, 0);
    assert(t);
    tagstruct_putu32(t, PA_COMMAND_REQUEST);
    tagstruct_putu32(t, (uint32_t) -1); /* tag */
    tagstruct_putu32(t, s->index);
    tagstruct_putu32(t, l);
    pstream_send_tagstruct(s->connection->pstream, t);

/*    fprintf(stderr, "Requesting %u bytes\n", l);*/
}

/*** sinkinput callbacks ***/

static int sink_input_peek_cb(struct sink_input *i, struct memchunk *chunk) {
    struct playback_stream *s;
    assert(i && i->userdata && chunk);
    s = i->userdata;

    if (memblockq_peek(s->memblockq, chunk) < 0)
        return -1;

    return 0;
}

static void sink_input_drop_cb(struct sink_input *i, size_t length) {
    struct playback_stream *s;
    assert(i && i->userdata && length);
    s = i->userdata;

    memblockq_drop(s->memblockq, length);
    request_bytes(s);
}

static void sink_input_kill_cb(struct sink_input *i) {
    struct playback_stream *s;
    assert(i && i->userdata);
    s = i->userdata;

    playback_stream_free(s);
}

static uint32_t sink_input_get_latency_cb(struct sink_input *i) {
    struct playback_stream *s;
    assert(i && i->userdata);
    s = i->userdata;

    return pa_samples_usec(memblockq_get_length(s->memblockq), &s->sink_input->sample_spec);
}

/*** pdispatch callbacks ***/

static int command_create_playback_stream(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *s;
    size_t maxlength, prebuf, qlength;
    uint32_t sink_index;
    const char *name;
    struct pa_sample_spec ss;
    struct tagstruct *reply;
    struct sink *sink;
    assert(c && t && c->protocol && c->protocol->core);
    
    if (tagstruct_gets(t, &name) < 0 ||
        tagstruct_get_sample_spec(t, &ss) < 0 ||
        tagstruct_getu32(t, &sink_index) < 0 ||
        tagstruct_getu32(t, &qlength) < 0 ||
        tagstruct_getu32(t, &maxlength) < 0 ||
        tagstruct_getu32(t, &prebuf) < 0 ||
        !tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return 0;
    }

    if (sink_index == (uint32_t) -1)
        sink = sink_get_default(c->protocol->core);
    else
        sink = idxset_get_by_index(c->protocol->core->sinks, sink_index);

    if (!sink) {
        pstream_send_error(c->pstream, tag, PA_ERROR_EXIST);
        return 0;
    }
    
    if (!(s = playback_stream_new(c, sink, &ss, name, qlength, maxlength, prebuf))) {
        pstream_send_error(c->pstream, tag, PA_ERROR_INVALID);
        return 0;
    }
    
    reply = tagstruct_new(NULL, 0);
    assert(reply);
    tagstruct_putu32(reply, PA_COMMAND_REPLY);
    tagstruct_putu32(reply, tag);
    tagstruct_putu32(reply, s->index);
    assert(s->sink_input);
    tagstruct_putu32(reply, s->sink_input->index);
    pstream_send_tagstruct(c->pstream, reply);
    request_bytes(s);
    return 0;
}

static int command_delete_playback_stream(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    uint32_t channel;
    struct playback_stream *s;
    assert(c && t);
    
    if (tagstruct_getu32(t, &channel) < 0 ||
        !tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return 0;
    }
    
    if (!(s = idxset_get_by_index(c->playback_streams, channel))) {
        pstream_send_error(c->pstream, tag, PA_ERROR_EXIST);
        return 0;
    }

    pstream_send_simple_ack(c->pstream, tag);
    return 0;
}

static int command_exit(struct pdispatch *pd, uint32_t command, uint32_t tag, struct tagstruct *t, void *userdata) {
    struct connection *c = userdata;
    assert(c && t);
    
    if (!tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        pstream_send_error(c->pstream, tag, PA_ERROR_ACCESS);
        return 0;
    }
    
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop);
    c->protocol->core->mainloop->quit(c->protocol->core->mainloop, 0);
    pstream_send_simple_ack(c->pstream, tag); /* nonsense */
    return 0;
}

/*** pstream callbacks ***/

static int packet_callback(struct pstream *p, struct packet *packet, void *userdata) {
    struct connection *c = userdata;
    assert(p && packet && packet->data && c);

    if (pdispatch_run(c->pdispatch, packet, c) < 0) {
        fprintf(stderr, "protocol-native: invalid packet.\n");
        return -1;
    }
    
    return 0;
}

static int memblock_callback(struct pstream *p, uint32_t channel, int32_t delta, struct memchunk *chunk, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *stream;
    assert(p && chunk && userdata);

    if (!(stream = idxset_get_by_index(c->playback_streams, channel))) {
        fprintf(stderr, "protocol-native: client sent block for invalid stream.\n");
        return -1;
    }

    if (chunk->length >= stream->requested_bytes)
        stream->requested_bytes = 0;
    else
        stream->requested_bytes -= chunk->length;
    
    memblockq_push(stream->memblockq, chunk, delta);
    assert(stream->sink_input);
    sink_notify(stream->sink_input->sink);

    /*fprintf(stderr, "Recieved %u bytes.\n", chunk->length);*/

    return 0;
}

static void die_callback(struct pstream *p, void *userdata) {
    struct connection *c = userdata;
    assert(p && c);
    connection_free(c);

    fprintf(stderr, "protocol-native: connection died.\n");
}

/*** socket server callbacks ***/

static void on_connection(struct socket_server*s, struct iochannel *io, void *userdata) {
    struct protocol_native *p = userdata;
    struct connection *c;
    assert(s && io && p);

    c = malloc(sizeof(struct connection));
    assert(c);
    c->authorized = p->public;
    c->protocol = p;
    assert(p->core);
    c->client = client_new(p->core, "NATIVE", "Client");
    assert(c->client);
    c->pstream = pstream_new(p->core->mainloop, io);
    assert(c->pstream);

    pstream_set_recieve_packet_callback(c->pstream, packet_callback, c);
    pstream_set_recieve_memblock_callback(c->pstream, memblock_callback, c);
    pstream_set_die_callback(c->pstream, die_callback, c);

    c->pdispatch = pdispatch_new(p->core->mainloop, command_table, PA_COMMAND_MAX);
    assert(c->pdispatch);

    c->record_streams = idxset_new(NULL, NULL);
    c->playback_streams = idxset_new(NULL, NULL);
    assert(c->record_streams && c->playback_streams);

    idxset_put(p->connections, c, NULL);
}

/*** module entry points ***/

struct protocol_native* protocol_native_new(struct core *core, struct socket_server *server) {
    struct protocol_native *p;
    assert(core && server);

    p = malloc(sizeof(struct protocol_native));
    assert(p);

    p->public = 1;
    p->server = server;
    p->core = core;
    p->connections = idxset_new(NULL, NULL);
    assert(p->connections);

    socket_server_set_callback(p->server, on_connection, p);
    
    return p;
}

void protocol_native_free(struct protocol_native *p) {
    struct connection *c;
    assert(p);

    while ((c = idxset_first(p->connections, NULL)))
        connection_free(c);
    idxset_free(p->connections, NULL, NULL);
    socket_server_free(p->server);
    free(p);
}
