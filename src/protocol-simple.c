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

static void io_callback(struct iochannel*io, void *userdata) {
    struct connection *c = userdata;
    assert(io && c);

    if (c->istream && iochannel_is_readable(io)) {
        struct memchunk chunk;
        ssize_t r;

        chunk.memblock = memblock_new(BUFSIZE);
        assert(chunk.memblock);

        if ((r = iochannel_read(io, chunk.memblock->data, BUFSIZE)) <= 0) {
            fprintf(stderr, "read(): %s\n", r == 0 ? "EOF" : strerror(errno));
            memblock_unref(chunk.memblock);
            goto fail;
        }
        
        chunk.memblock->length = r;
        chunk.length = r;
        chunk.index = 0;
        
        memblockq_push(c->istream->memblockq, &chunk, 0);
        input_stream_notify(c->istream);
        memblock_unref(chunk.memblock);
    }

    if (c->ostream && iochannel_is_writable(io)) {
        struct memchunk chunk;
        ssize_t r;

        memblockq_peek(c->ostream->memblockq, &chunk);
        assert(chunk.memblock && chunk.length);

        if ((r = iochannel_write(io, chunk.memblock->data+chunk.index, chunk.length)) < 0) {
            fprintf(stderr, "write(): %s\n", strerror(errno));
            memblock_unref(chunk.memblock);
            goto fail;
        }
        
        memblockq_drop(c->ostream->memblockq, r);
        memblock_unref(chunk.memblock);
    }

    return;
    
fail:
    idxset_remove_by_data(c->protocol->connections, c, NULL);
    free_connection(c, NULL);
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
    
    if (p->mode & PROTOCOL_SIMPLE_RECORD) {
        struct source *source;

        if (!(source = core_get_default_source(p->core))) {
            fprintf(stderr, "Failed to get default source.\n");
            goto fail;
        }

        c->ostream = output_stream_new(source, &DEFAULT_SAMPLE_SPEC, c->client->name);
        assert(c->ostream);
    }

    if (p->mode & PROTOCOL_SIMPLE_PLAYBACK) {
        struct sink *sink;

        if (!(sink = core_get_default_sink(p->core))) {
            fprintf(stderr, "Failed to get default sink.\n");
            goto fail;
        }

        c->istream = input_stream_new(sink, &DEFAULT_SAMPLE_SPEC, c->client->name);
        assert(c->istream);
    }

    c->client = client_new(p->core, "SIMPLE", "Client");
    assert(c->client);

    iochannel_set_callback(c->io, io_callback, c);
    idxset_put(p->connections, c, NULL);
    return;
    
fail:
    if (c) {
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
