#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#include "protocol-native.h"
#include "packet.h"
#include "client.h"
#include "sourceoutput.h"
#include "sinkinput.h"
#include "pstream.h"
#include "tagstruct.h"

struct connection;
struct protocol_native;

enum {
    COMMAND_ERROR,
    COMMAND_REPLY,
    COMMAND_CREATE_PLAYBACK_STREAM,
    COMMAND_DELETE_PLAYBACK_STREAM,
    COMMAND_CREATE_RECORD_STREAM,
    COMMAND_DELETE_RECORD_STREAM,
    COMMAND_EXIT,
    COMMAND_MAX
};

enum {
    ERROR_ACCESS,
    ERROR_COMMAND,
    ERROR_ARGUMENT,
    ERROR_EXIST
};

struct record_stream {
    struct connection *connection;
    uint32_t index;
    struct source_output *source_output;
    struct memblockq *memblockq;
};

struct playback_stream {
    struct connection *connection;
    uint32_t index;
    struct sink_input *sink_input;
    struct memblockq *memblockq;
};

struct connection {
    int authorized;
    struct protocol_native *protocol;
    struct client *client;
    struct pstream *pstream;
    struct idxset *record_streams, *playback_streams;
};

struct protocol_native {
    int public;
    struct core *core;
    struct socket_server *server;
    struct idxset *connections;
};

static void record_stream_free(struct record_stream* r) {
    assert(r && r->connection);

    idxset_remove_by_data(r->connection->record_streams, r, NULL);
    source_output_free(r->source_output);
    memblockq_free(r->memblockq);
    free(r);
}

static struct playback_stream* playback_stream_new(struct connection *c, struct sink *sink, struct sample_spec *ss, const char *name, size_t maxlength, size_t prebuf) {
    struct playback_stream *s;

    s = malloc(sizeof(struct playback_stream));
    assert (s);
    s->connection = c;
    s->sink_input = sink_input_new(sink, ss, name);
    assert(s->sink_input);
    s->memblockq = memblockq_new(maxlength, sample_size(ss), prebuf);
    assert(s->memblockq);

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
    pstream_free(c->pstream);
    while ((r = idxset_first(c->record_streams, NULL)))
        record_stream_free(r);
    idxset_free(c->record_streams, NULL, NULL);

    while ((p = idxset_first(c->playback_streams, NULL)))
        playback_stream_free(p);
    idxset_free(c->playback_streams, NULL, NULL);

    client_free(c->client);
    free(c);
}

/*** pstream callbacks ***/

static void send_tagstruct(struct pstream *p, struct tagstruct *t) {
    size_t length;
    uint8_t *data;
    struct packet *packet;
    assert(p && t);

    data = tagstruct_free_data(t, &length);
    assert(data && length);
    packet = packet_new_dynamic(data, length);
    assert(packet);
    pstream_send_packet(p, packet);
    packet_unref(packet);
}

static void send_error(struct pstream *p, uint32_t tag, uint32_t error) {
    struct tagstruct *t = tagstruct_new(NULL, 0);
    assert(t);
    tagstruct_putu32(t, COMMAND_ERROR);
    tagstruct_putu32(t, tag);
    tagstruct_putu32(t, error);
    send_tagstruct(p, t);
}

static void send_simple_ack(struct pstream *p, uint32_t tag) {
    struct tagstruct *t = tagstruct_new(NULL, 0);
    assert(t);
    tagstruct_putu32(t, COMMAND_REPLY);
    tagstruct_putu32(t, tag);
    send_tagstruct(p, t);
}

struct command {
    int (*func)(struct connection *c, uint32_t tag, struct tagstruct *t);
};

static int command_create_playback_stream(struct connection *c, uint32_t tag, struct tagstruct *t) {
    struct playback_stream *s;
    size_t maxlength, prebuf;
    uint32_t sink_index;
    const char *name;
    struct sample_spec ss;
    struct tagstruct *reply;
    struct sink *sink;
    assert(c && t && c->protocol && c->protocol->core);
    
    if (tagstruct_gets(t, &name) < 0 ||
        tagstruct_get_sample_spec(t, &ss) < 0 ||
        tagstruct_getu32(t, &sink_index) < 0 || 
        tagstruct_getu32(t, &maxlength) < 0 ||
        tagstruct_getu32(t, &prebuf) < 0 ||
        !tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        send_error(c->pstream, tag, ERROR_ACCESS);
        return 0;
    }

    if (sink_index == (uint32_t) -1)
        sink = sink_get_default(c->protocol->core);
    else
        sink = idxset_get_by_index(c->protocol->core->sinks, sink_index);

    if (!sink) {
        send_error(c->pstream, tag, ERROR_EXIST);
        return 0;
    }
    
    if (!(s = playback_stream_new(c, sink, &ss, name, maxlength, prebuf))) {
        send_error(c->pstream, tag, ERROR_ARGUMENT);
        return 0;
    }
    
    reply = tagstruct_new(NULL, 0);
    assert(reply);
    tagstruct_putu32(reply, COMMAND_REPLY);
    tagstruct_putu32(reply, tag);
    tagstruct_putu32(reply, s->index);
    send_tagstruct(c->pstream, reply);
    return 0;
}

static int command_delete_playback_stream(struct connection *c, uint32_t tag, struct tagstruct *t) {
    uint32_t channel;
    struct playback_stream *s;
    assert(c && t);
    
    if (tagstruct_getu32(t, &channel) < 0 ||
        !tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        send_error(c->pstream, tag, ERROR_ACCESS);
        return 0;
    }
    
    if (!(s = idxset_get_by_index(c->playback_streams, channel))) {
        send_error(c->pstream, tag, ERROR_EXIST);
        return 0;
    }

    send_simple_ack(c->pstream, tag);
    return 0;
}

static int command_exit(struct connection *c, uint32_t tag, struct tagstruct *t) {
    assert(c && t);
    
    if (!tagstruct_eof(t))
        return -1;

    if (!c->authorized) {
        send_error(c->pstream, tag, ERROR_ACCESS);
        return 0;
    }
    
    assert(c->protocol && c->protocol->core);
    mainloop_quit(c->protocol->core->mainloop, -1);
    send_simple_ack(c->pstream, tag); /* nonsense */
    return 0;
}

static const struct command commands[] = {
    [COMMAND_ERROR] = { NULL },
    [COMMAND_REPLY] = { NULL },
    [COMMAND_CREATE_PLAYBACK_STREAM] = { command_create_playback_stream },
    [COMMAND_DELETE_PLAYBACK_STREAM] = { command_delete_playback_stream },
    [COMMAND_CREATE_RECORD_STREAM] = { NULL },
    [COMMAND_DELETE_RECORD_STREAM] = { NULL },
    [COMMAND_EXIT] = { command_exit },
};

static int packet_callback(struct pstream *p, struct packet *packet, void *userdata) {
    struct connection *c = userdata;
    uint32_t tag, command;
    struct tagstruct *ts = NULL;
    assert(p && packet && packet->data && c);

    if (packet->length <= 8)
        goto fail;

    ts = tagstruct_new(packet->data, packet->length);
    assert(ts);

    if (tagstruct_getu32(ts, &command) < 0 ||
        tagstruct_getu32(ts, &tag) < 0)
        goto fail;

    if (command >= COMMAND_MAX || !commands[command].func)
        send_error(p, tag, ERROR_COMMAND);
    else if (commands[command].func(c, tag, ts) < 0)
        goto fail;
    
    tagstruct_free(ts);    
        
    return 0;

fail:
    if (ts)
        tagstruct_free(ts);    

    fprintf(stderr, "protocol-native: invalid packet.\n");
    return -1;
    
}

static int memblock_callback(struct pstream *p, uint32_t channel, int32_t delta, struct memchunk *chunk, void *userdata) {
    struct connection *c = userdata;
    struct playback_stream *stream;
    assert(p && chunk && userdata);

    if (!(stream = idxset_get_by_index(c->playback_streams, channel))) {
        fprintf(stderr, "protocol-native: client sent block for invalid stream.\n");
        return -1;
    }

    memblockq_push(stream->memblockq, chunk, delta);
    assert(stream->sink_input);
    sink_notify(stream->sink_input->sink);

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
