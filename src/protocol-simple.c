#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "sinkinput.h"
#include "sourceoutput.h"
#include "protocol-simple.h"
#include "client.h"
#include "sample-util.h"

struct connection {
    struct protocol_simple *protocol;
    struct iochannel *io;
    struct sink_input *sink_input;
    struct source_output *source_output;
    struct client *client;
    struct memblockq *input_memblockq, *output_memblockq;
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
    
    if (c->sink_input)
        sink_input_free(c->sink_input);
    if (c->source_output)
        source_output_free(c->source_output);
    if (c->client)
        client_free(c->client);
    if (c->io)
        iochannel_free(c->io);
    if (c->input_memblockq)
        memblockq_free(c->input_memblockq);
    if (c->output_memblockq)
        memblockq_free(c->output_memblockq);
    free(c);
}

static void destroy_connection(struct connection *c) {
    assert(c && c->protocol);
    idxset_remove_by_data(c->protocol->connections, c, NULL);
    free_connection(c, NULL);
}

static int do_read(struct connection *c) {
    struct memchunk chunk;
    ssize_t r;

    if (!iochannel_is_readable(c->io))
        return 0;
    
    if (!c->sink_input || !memblockq_is_writable(c->input_memblockq, BUFSIZE))
        return 0;
    
    chunk.memblock = memblock_new(BUFSIZE);
    assert(chunk.memblock);

    if ((r = iochannel_read(c->io, chunk.memblock->data, BUFSIZE)) <= 0) {
        fprintf(stderr, "read(): %s\n", r == 0 ? "EOF" : strerror(errno));
        memblock_unref(chunk.memblock);
        return -1;
    }

    chunk.memblock->length = chunk.length = r;
    chunk.index = 0;

    assert(c->input_memblockq);
    memblockq_push(c->input_memblockq, &chunk, 0);
    memblock_unref(chunk.memblock);
    assert(c->sink_input);
    sink_notify(c->sink_input->sink);
    
    return 0;
}

static int do_write(struct connection *c) {
    struct memchunk chunk;
    ssize_t r;

    if (!iochannel_is_writable(c->io))
        return 0;
    
    if (!c->source_output)
        return 0;    

    assert(c->output_memblockq);
    if (memblockq_peek(c->output_memblockq, &chunk) < 0)
        return 0;
        
    assert(chunk.memblock && chunk.length);
    
    if ((r = iochannel_write(c->io, chunk.memblock->data+chunk.index, chunk.length)) < 0) {
        fprintf(stderr, "write(): %s\n", strerror(errno));
        memblock_unref(chunk.memblock);
        return -1;
    }
    
    memblockq_drop(c->output_memblockq, r);
    memblock_unref(chunk.memblock);
    return 0;
}

/*** sink_input callbacks ***/

static int sink_input_peek_cb(struct sink_input *i, struct memchunk *chunk) {
    struct connection*c;
    assert(i && i->userdata && chunk);
    c = i->userdata;
    
    if (memblockq_peek(c->input_memblockq, chunk) < 0)
        return -1;

    return 0;
}

static void sink_input_drop_cb(struct sink_input *i, size_t length) {
    struct connection*c = i->userdata;
    assert(i && c && length);

    memblockq_drop(c->input_memblockq, length);
    
    if (do_read(c) < 0)
        destroy_connection(c);
}

static void sink_input_kill_cb(struct sink_input *i) {
    assert(i && i->userdata);
    destroy_connection((struct connection *) i->userdata);
}


static uint32_t sink_input_get_latency_cb(struct sink_input *i) {
    struct connection*c = i->userdata;
    assert(i && c);
    return pa_samples_usec(memblockq_get_length(c->input_memblockq), &c->sink_input->sample_spec);
}

/*** source_output callbacks ***/

static void source_output_push_cb(struct source_output *o, struct memchunk *chunk) {
    struct connection *c = o->userdata;
    assert(o && c && chunk);

    memblockq_push(c->output_memblockq, chunk, 0);

    if (do_write(c) < 0)
        destroy_connection(c);
}

static void source_output_kill_cb(struct source_output *o) {
    assert(o && o->userdata);
    destroy_connection((struct connection *) o->userdata);
}

/*** client callbacks ***/

static void client_kill_cb(struct client *c) {
    assert(c && c->userdata);
    destroy_connection((struct connection *) c->userdata);
}

/*** iochannel callbacks ***/

static void io_callback(struct iochannel*io, void *userdata) {
    struct connection *c = userdata;
    assert(io && c && c->io == io);

    if (do_read(c) < 0 || do_write(c) < 0)
        destroy_connection(c);
}

/*** socket_server callbacks */

static void on_connection(struct socket_server*s, struct iochannel *io, void *userdata) {
    struct protocol_simple *p = userdata;
    struct connection *c = NULL;
    char cname[256];
    assert(s && io && p);

    c = malloc(sizeof(struct connection));
    assert(c);
    c->io = io;
    c->sink_input = NULL;
    c->source_output = NULL;
    c->input_memblockq = c->output_memblockq = NULL;
    c->protocol = p;

    iochannel_peer_to_string(io, cname, sizeof(cname));
    c->client = client_new(p->core, "SIMPLE", cname);
    assert(c->client);
    c->client->kill = client_kill_cb;
    c->client->userdata = c;

    if (p->mode & PROTOCOL_SIMPLE_RECORD) {
        struct source *source;
        size_t l;

        if (!(source = source_get_default(p->core))) {
            fprintf(stderr, "Failed to get default source.\n");
            goto fail;
        }

        c->source_output = source_output_new(source, &DEFAULT_SAMPLE_SPEC, c->client->name);
        assert(c->source_output);
        c->source_output->push = source_output_push_cb;
        c->source_output->kill = source_output_kill_cb;
        c->source_output->userdata = c;

        l = 5*pa_bytes_per_second(&DEFAULT_SAMPLE_SPEC); /* 5s */
        c->output_memblockq = memblockq_new(l, pa_sample_size(&DEFAULT_SAMPLE_SPEC), l/2);
    }

    if (p->mode & PROTOCOL_SIMPLE_PLAYBACK) {
        struct sink *sink;
        size_t l;

        if (!(sink = sink_get_default(p->core))) {
            fprintf(stderr, "Failed to get default sink.\n");
            goto fail;
        }

        c->sink_input = sink_input_new(sink, &DEFAULT_SAMPLE_SPEC, c->client->name);
        assert(c->sink_input);
        c->sink_input->peek = sink_input_peek_cb;
        c->sink_input->drop = sink_input_drop_cb;
        c->sink_input->kill = sink_input_kill_cb;
        c->sink_input->get_latency = sink_input_get_latency_cb;
        c->sink_input->userdata = c;

        l = pa_bytes_per_second(&DEFAULT_SAMPLE_SPEC)/2; /* half a second */
        c->input_memblockq = memblockq_new(l, pa_sample_size(&DEFAULT_SAMPLE_SPEC), l/2);
    }


    iochannel_set_callback(c->io, io_callback, c);
    idxset_put(p->connections, c, NULL);
    return;
    
fail:
    if (c) {
        free_connection(c, NULL);
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
