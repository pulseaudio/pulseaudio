/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "sink-input.h"
#include "source-output.h"
#include "protocol-simple.h"
#include "client.h"
#include "sample-util.h"
#include "namereg.h"
#include "xmalloc.h"
#include "log.h"

struct connection {
    struct pa_protocol_simple *protocol;
    struct pa_iochannel *io;
    struct pa_sink_input *sink_input;
    struct pa_source_output *source_output;
    struct pa_client *client;
    struct pa_memblockq *input_memblockq, *output_memblockq;
    struct pa_defer_event *defer_event;

    struct {
        struct pa_memblock *current_memblock;
        size_t memblock_index, fragment_size;
    } playback;
};

struct pa_protocol_simple {
    struct pa_module *module;
    struct pa_core *core;
    struct pa_socket_server*server;
    struct pa_idxset *connections;
    enum {
        RECORD = 1,
        PLAYBACK = 2,
        DUPLEX = 3
    } mode;
    struct pa_sample_spec sample_spec;
    char *source_name, *sink_name;
};

#define PLAYBACK_BUFFER_SECONDS (.5)
#define PLAYBACK_BUFFER_FRAGMENTS (10)
#define RECORD_BUFFER_SECONDS (5)
#define RECORD_BUFFER_FRAGMENTS (100)

static void connection_free(struct connection *c) {
    assert(c);

    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);

    if (c->playback.current_memblock)
        pa_memblock_unref(c->playback.current_memblock);
    if (c->sink_input) {
        pa_sink_input_disconnect(c->sink_input);
        pa_sink_input_unref(c->sink_input);
    }
    if (c->source_output) {
        pa_source_output_disconnect(c->source_output);
        pa_source_output_unref(c->source_output);
    }
    if (c->client)
        pa_client_free(c->client);
    if (c->io)
        pa_iochannel_free(c->io);
    if (c->input_memblockq)
        pa_memblockq_free(c->input_memblockq);
    if (c->output_memblockq)
        pa_memblockq_free(c->output_memblockq);
    if (c->defer_event)
        c->protocol->core->mainloop->defer_free(c->defer_event);
    pa_xfree(c);
}

static int do_read(struct connection *c) {
    struct pa_memchunk chunk;
    ssize_t r;
    size_t l;

    if (!c->sink_input || !(l = pa_memblockq_missing(c->input_memblockq)))
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
        pa_log(__FILE__": read() failed: %s\n", r == 0 ? "EOF" : strerror(errno));
        return -1;
    }

    chunk.memblock = c->playback.current_memblock;
    chunk.index = c->playback.memblock_index;
    chunk.length = r;
    assert(chunk.memblock);

    c->playback.memblock_index += r;
    
    assert(c->input_memblockq);
    pa_memblockq_push_align(c->input_memblockq, &chunk, 0);
    assert(c->sink_input);
    pa_sink_notify(c->sink_input->sink);
    
    return 0;
}

static int do_write(struct connection *c) {
    struct pa_memchunk chunk;
    ssize_t r;

    if (!c->source_output)
        return 0;    

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
    
    return 0;
}


static void do_work(struct connection *c) {
    assert(c);

    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);
    c->protocol->core->mainloop->defer_enable(c->defer_event, 0);

    if (pa_iochannel_is_hungup(c->io))
        goto fail;
    
    if (pa_iochannel_is_writable(c->io))
        if (do_write(c) < 0)
            goto fail;
    
    if (pa_iochannel_is_readable(c->io))
        if (do_read(c) < 0)
            goto fail;

    return;

fail:
    connection_free(c);
}

/*** sink_input callbacks ***/

static int sink_input_peek_cb(struct pa_sink_input *i, struct pa_memchunk *chunk) {
    struct connection*c;
    assert(i && i->userdata && chunk);
    c = i->userdata;
    
    if (pa_memblockq_peek(c->input_memblockq, chunk) < 0)
        return -1;

    return 0;
}

static void sink_input_drop_cb(struct pa_sink_input *i, const struct pa_memchunk *chunk, size_t length) {
    struct connection*c = i->userdata;
    assert(i && c && length);

    pa_memblockq_drop(c->input_memblockq, chunk, length);

    /* do something */
    assert(c->protocol && c->protocol->core && c->protocol->core->mainloop && c->protocol->core->mainloop->defer_enable);
    c->protocol->core->mainloop->defer_enable(c->defer_event, 1);
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

/*** client callbacks ***/

static void client_kill_cb(struct pa_client *c) {
    assert(c && c->userdata);
    connection_free((struct connection *) c->userdata);
}

/*** pa_iochannel callbacks ***/

static void io_callback(struct pa_iochannel*io, void *userdata) {
    struct connection *c = userdata;
    assert(io && c && c->io == io);

    do_work(c);
}

/*** fixed callback ***/

static void defer_callback(struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata) {
    struct connection *c = userdata;
    assert(a && c && c->defer_event == e);

    do_work(c);
}

/*** socket_server callbacks ***/

static void on_connection(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata) {
    struct pa_protocol_simple *p = userdata;
    struct connection *c = NULL;
    char cname[256];
    assert(s && io && p);

    c = pa_xmalloc(sizeof(struct connection));
    c->io = io;
    c->sink_input = NULL;
    c->source_output = NULL;
    c->defer_event = NULL;
    c->input_memblockq = c->output_memblockq = NULL;
    c->protocol = p;
    c->playback.current_memblock = NULL;
    c->playback.memblock_index = 0;
    c->playback.fragment_size = 0;
    
    pa_iochannel_socket_peer_to_string(io, cname, sizeof(cname));
    c->client = pa_client_new(p->core, "SIMPLE", cname);
    assert(c->client);
    c->client->owner = p->module;
    c->client->kill = client_kill_cb;
    c->client->userdata = c;

    if (p->mode & PLAYBACK) {
        struct pa_sink *sink;
        size_t l;

        if (!(sink = pa_namereg_get(p->core, p->sink_name, PA_NAMEREG_SINK, 1))) {
            pa_log(__FILE__": Failed to get sink.\n");
            goto fail;
        }

        if (!(c->sink_input = pa_sink_input_new(sink, c->client->name, &p->sample_spec, 0, -1))) {
            pa_log(__FILE__": Failed to create sink input.\n");
            goto fail;
        }
        
        c->sink_input->owner = p->module;
        c->sink_input->client = c->client;
        
        c->sink_input->peek = sink_input_peek_cb;
        c->sink_input->drop = sink_input_drop_cb;
        c->sink_input->kill = sink_input_kill_cb;
        c->sink_input->get_latency = sink_input_get_latency_cb;
        c->sink_input->userdata = c;

        l = (size_t) (pa_bytes_per_second(&p->sample_spec)*PLAYBACK_BUFFER_SECONDS);
        c->input_memblockq = pa_memblockq_new(l, 0, pa_frame_size(&p->sample_spec), l/2, l/PLAYBACK_BUFFER_FRAGMENTS, p->core->memblock_stat);
        assert(c->input_memblockq);
        pa_iochannel_socket_set_rcvbuf(io, l/PLAYBACK_BUFFER_FRAGMENTS*5);
        c->playback.fragment_size = l/10;
    }

    if (p->mode & RECORD) {
        struct pa_source *source;
        size_t l;

        if (!(source = pa_namereg_get(p->core, p->source_name, PA_NAMEREG_SOURCE, 1))) {
            pa_log(__FILE__": Failed to get source.\n");
            goto fail;
        }

        c->source_output = pa_source_output_new(source, c->client->name, &p->sample_spec, -1);
        if (!c->source_output) {
            pa_log(__FILE__": Failed to create source output.\n");
            goto fail;
        }
        c->source_output->owner = p->module;
        c->source_output->client = c->client;
        
        c->source_output->push = source_output_push_cb;
        c->source_output->kill = source_output_kill_cb;
        c->source_output->get_latency = source_output_get_latency_cb;
        c->source_output->userdata = c;

        l = (size_t) (pa_bytes_per_second(&p->sample_spec)*RECORD_BUFFER_SECONDS);
        c->output_memblockq = pa_memblockq_new(l, 0, pa_frame_size(&p->sample_spec), 0, 0, p->core->memblock_stat);
        pa_iochannel_socket_set_sndbuf(io, l/RECORD_BUFFER_FRAGMENTS*2);
    }

    pa_iochannel_set_callback(c->io, io_callback, c);
    pa_idxset_put(p->connections, c, NULL);

    c->defer_event = p->core->mainloop->defer_new(p->core->mainloop, defer_callback, c);
    assert(c->defer_event);
    p->core->mainloop->defer_enable(c->defer_event, 0);
    
    return;
    
fail:
    if (c)
        connection_free(c);
}

struct pa_protocol_simple* pa_protocol_simple_new(struct pa_core *core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma) {
    struct pa_protocol_simple* p = NULL;
    int enable;
    assert(core && server && ma);

    p = pa_xmalloc0(sizeof(struct pa_protocol_simple));
    p->module = m;
    p->core = core;
    p->server = server;
    p->connections = pa_idxset_new(NULL, NULL);

    p->sample_spec = core->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &p->sample_spec) < 0) {
        pa_log(__FILE__": Failed to parse sample type specification.\n");
        goto fail;
    }

    p->source_name = pa_xstrdup(pa_modargs_get_value(ma, "source", NULL));
    p->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));
    
    enable = 0;
    if (pa_modargs_get_value_boolean(ma, "record", &enable) < 0) {
        pa_log(__FILE__": record= expects a numeric argument.\n");
        goto fail;
    }
    p->mode = enable ? RECORD : 0;

    enable = 1;
    if (pa_modargs_get_value_boolean(ma, "playback", &enable) < 0) {
        pa_log(__FILE__": playback= expects a numeric argument.\n");
        goto fail;
    }
    p->mode |= enable ? PLAYBACK : 0;

    if ((p->mode & (RECORD|PLAYBACK)) == 0) {
        pa_log(__FILE__": neither playback nor recording enabled for protocol.\n");
        goto fail;
    }
    
    pa_socket_server_set_callback(p->server, on_connection, p);
    
    return p;

fail:
    if (p)
        pa_protocol_simple_free(p);
    return NULL;
}


void pa_protocol_simple_free(struct pa_protocol_simple *p) {
    struct connection *c;
    assert(p);

    if (p->connections) {
        while((c = pa_idxset_first(p->connections, NULL)))
            connection_free(c);
        
        pa_idxset_free(p->connections, NULL, NULL);
    }

    if (p->server)
        pa_socket_server_unref(p->server);
    pa_xfree(p);
}

