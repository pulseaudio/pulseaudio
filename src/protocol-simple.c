#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "inputstream.h"
#include "outputstream.h"
#include "protocol-simple.h"
#include "client.h"

struct connection {
    struct protocol_simple *protocol;
    struct iochannel *io;
    struct input_stream *istream;
    struct output_stream *ostream;
    struct client *client;
};

struct protocol_simple {
    struct core *core;
    struct socket_server*server;
    struct idxset *connections;
    enum protocol_simple_mode mode;
};

#define BUFSIZE PIPE_BUF

static void free_connection(void *data, void *userdata) {
    struct connection *c = data;
    assert(data);
    
    if (c->istream)
        input_stream_free(c->istream);
    if (c->ostream)
        output_stream_free(c->ostream);

    client_free(c->client);

    iochannel_free(c->io);
    free(c);
}

static void destroy_connection(struct connection *c) {
    assert(c && c->protocol);
    idxset_remove_by_data(c->protocol->connections, c, NULL);
    free_connection(c, NULL);
}

static void istream_kill_cb(struct input_stream *i, void *userdata) {
    struct connection *c = userdata;
    assert(i && c);
    destroy_connection(c);
}

static void ostream_kill_cb(struct output_stream *o, void *userdata) {
    struct connection *c = userdata;
    assert(o && c);
    destroy_connection(c);
}

static void client_kill_cb(struct client *client, void*userdata) {
    struct connection *c= userdata;
    assert(client && c);
    destroy_connection(c);
}

static int do_read(struct connection *c) {
    struct memchunk chunk;
    ssize_t r;

    if (!iochannel_is_readable(c->io))
        return 0;
    
    if (!c->istream || !memblockq_is_writable(c->istream->memblockq, BUFSIZE))
        return 0;
    
    chunk.memblock = memblock_new(BUFSIZE);
    assert(chunk.memblock);

    memblock_stamp(chunk.memblock);
    
    if ((r = iochannel_read(c->io, chunk.memblock->data, BUFSIZE)) <= 0) {
        fprintf(stderr, "read(): %s\n", r == 0 ? "EOF" : strerror(errno));
        memblock_unref(chunk.memblock);
        return -1;
    }

    chunk.memblock->length = r;
    chunk.length = r;
    chunk.index = 0;
    
    memblockq_push(c->istream->memblockq, &chunk, 0);
    input_stream_notify_sink(c->istream);
    memblock_unref(chunk.memblock);
    return 0;
}

static int do_write(struct connection *c) {
    struct memchunk chunk;
    ssize_t r;

    if (!iochannel_is_writable(c->io))
        return 0;
    
    if (!c->ostream)
        return 0;    

    memblockq_peek(c->ostream->memblockq, &chunk);
    assert(chunk.memblock && chunk.length);
    
    if ((r = iochannel_write(c->io, chunk.memblock->data+chunk.index, chunk.length)) < 0) {
        fprintf(stderr, "write(): %s\n", strerror(errno));
        memblock_unref(chunk.memblock);
        return -1;
    }
    
    memblockq_drop(c->ostream->memblockq, r);
    memblock_unref(chunk.memblock);
    return 0;
}

static void io_callback(struct iochannel*io, void *userdata) {
    struct connection *c = userdata;
    assert(io && c && c->io == io);

    if (do_read(c) < 0 || do_write(c) < 0)
        destroy_connection(c);
}

static void istream_notify_cb(struct input_stream *i, void *userdata) {
    struct connection*c = userdata;
    assert(i && c && c->istream == i);
    
    if (do_read(c) < 0)
        destroy_connection(c);
}

static void on_connection(struct socket_server*s, struct iochannel *io, void *userdata) {
    struct protocol_simple *p = userdata;
    struct connection *c = NULL;
    assert(s && io && p);

    c = malloc(sizeof(struct connection));
    assert(c);
    c->io = io;
    c->istream = NULL;
    c->ostream = NULL;
    c->protocol = p;

    c->client = client_new(p->core, "SIMPLE", "Client");
    assert(c->client);
    client_set_kill_callback(c->client, client_kill_cb, c);

    if (p->mode & PROTOCOL_SIMPLE_RECORD) {
        struct source *source;

        if (!(source = core_get_default_source(p->core))) {
            fprintf(stderr, "Failed to get default source.\n");
            goto fail;
        }

        c->ostream = output_stream_new(source, &DEFAULT_SAMPLE_SPEC, c->client->name);
        assert(c->ostream);
        output_stream_set_kill_callback(c->ostream, ostream_kill_cb, c);
    }

    if (p->mode & PROTOCOL_SIMPLE_PLAYBACK) {
        struct sink *sink;

        if (!(sink = core_get_default_sink(p->core))) {
            fprintf(stderr, "Failed to get default sink.\n");
            goto fail;
        }

        c->istream = input_stream_new(sink, &DEFAULT_SAMPLE_SPEC, c->client->name);
        assert(c->istream);
        input_stream_set_kill_callback(c->istream, istream_kill_cb, c);
        input_stream_set_notify_callback(c->istream, istream_notify_cb, c);
    }


    iochannel_set_callback(c->io, io_callback, c);
    idxset_put(p->connections, c, NULL);
    return;
    
fail:
    if (c) {
        if (c->client)
            client_free(c->client);
        if (c->istream)
            input_stream_free(c->istream);
        if (c->ostream)
            output_stream_free(c->ostream);

        iochannel_free(c->io);
        free(c);
    }
}

struct protocol_simple* protocol_simple_new(struct core *core, struct socket_server *server, enum protocol_simple_mode mode) {
    struct protocol_simple* p;
    assert(core && server && mode <= PROTOCOL_SIMPLE_DUPLEX && mode > 0);

    p = malloc(sizeof(struct protocol_simple));
    assert(p);
    p->core = core;
    p->server = server;
    p->connections = idxset_new(NULL, NULL);
    p->mode = mode;

    socket_server_set_callback(p->server, on_connection, p);
    
    return p;
}


void protocol_simple_free(struct protocol_simple *p) {
    assert(p);

    idxset_free(p->connections, free_connection, NULL);
    socket_server_free(p->server);
    free(p);
}
