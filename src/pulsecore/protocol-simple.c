/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/client.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/core-error.h>
#include <pulsecore/atomic.h>
#include <pulsecore/thread-mq.h>

#include "protocol-simple.h"

/* Don't allow more than this many concurrent connections */
#define MAX_CONNECTIONS 10

typedef struct connection {
    pa_msgobject parent;
    pa_protocol_simple *protocol;
    pa_iochannel *io;
    pa_sink_input *sink_input;
    pa_source_output *source_output;
    pa_client *client;
    pa_memblockq *input_memblockq, *output_memblockq;

    int dead;

    struct {
        pa_memblock *current_memblock;
        size_t memblock_index, fragment_size;
        pa_atomic_t missing;
    } playback;
} connection;

PA_DECLARE_CLASS(connection);
#define CONNECTION(o) (connection_cast(o))
static PA_DEFINE_CHECK_TYPE(connection, pa_msgobject);

struct pa_protocol_simple {
    pa_module *module;
    pa_core *core;
    pa_socket_server*server;
    pa_idxset *connections;

    enum {
        RECORD = 1,
        PLAYBACK = 2,
        DUPLEX = 3
    } mode;

    pa_sample_spec sample_spec;
    char *source_name, *sink_name;
};

enum {
    SINK_INPUT_MESSAGE_POST_DATA = PA_SINK_INPUT_MESSAGE_MAX, /* data from main loop to sink input */
    SINK_INPUT_MESSAGE_DISABLE_PREBUF /* disabled prebuf, get playback started. */
};

enum {
    CONNECTION_MESSAGE_REQUEST_DATA,      /* data requested from sink input from the main loop */
    CONNECTION_MESSAGE_POST_DATA,         /* data from source output to main loop */
    CONNECTION_MESSAGE_UNLINK_CONNECTION    /* Please drop a aconnection now */
};


#define PLAYBACK_BUFFER_SECONDS (.5)
#define PLAYBACK_BUFFER_FRAGMENTS (10)
#define RECORD_BUFFER_SECONDS (5)
#define RECORD_BUFFER_FRAGMENTS (100)

static void connection_unlink(connection *c) {
    pa_assert(c);

    if (!c->protocol)
        return;

    if (c->sink_input) {
        pa_sink_input_unlink(c->sink_input);
        pa_sink_input_unref(c->sink_input);
        c->sink_input = NULL;
    }

    if (c->source_output) {
        pa_source_output_unlink(c->source_output);
        pa_source_output_unref(c->source_output);
        c->source_output = NULL;
    }

    if (c->client) {
        pa_client_free(c->client);
        c->client = NULL;
    }

    if (c->io) {
        pa_iochannel_free(c->io);
        c->io = NULL;
    }

    pa_assert_se(pa_idxset_remove_by_data(c->protocol->connections, c, NULL) == c);
    c->protocol = NULL;
    connection_unref(c);
}

static void connection_free(pa_object *o) {
    connection *c = CONNECTION(o);
    pa_assert(c);

    connection_unref(c);

    if (c->playback.current_memblock)
        pa_memblock_unref(c->playback.current_memblock);

    if (c->input_memblockq)
        pa_memblockq_free(c->input_memblockq);
    if (c->output_memblockq)
        pa_memblockq_free(c->output_memblockq);

    pa_xfree(c);
}

static int do_read(connection *c) {
    pa_memchunk chunk;
    ssize_t r;
    size_t l;
    void *p;

    connection_assert_ref(c);

    if (!c->sink_input || (l = pa_atomic_load(&c->playback.missing)) <= 0)
        return 0;

    if (l > c->playback.fragment_size)
        l = c->playback.fragment_size;

    if (c->playback.current_memblock)
        if (pa_memblock_get_length(c->playback.current_memblock) - c->playback.memblock_index < l) {
            pa_memblock_unref(c->playback.current_memblock);
            c->playback.current_memblock = NULL;
            c->playback.memblock_index = 0;
        }

    if (!c->playback.current_memblock) {
        pa_assert_se(c->playback.current_memblock = pa_memblock_new(c->protocol->core->mempool, l));
        c->playback.memblock_index = 0;
    }

    p = pa_memblock_acquire(c->playback.current_memblock);
    r = pa_iochannel_read(c->io, (uint8_t*) p + c->playback.memblock_index, l);
    pa_memblock_release(c->playback.current_memblock);

    if (r <= 0) {

        if (r < 0 && (errno == EINTR || errno == EAGAIN))
            return 0;

        pa_log_debug("read(): %s", r == 0 ? "EOF" : pa_cstrerror(errno));
        return -1;
    }

    chunk.memblock = c->playback.current_memblock;
    chunk.index = c->playback.memblock_index;
    chunk.length = r;

    c->playback.memblock_index += r;

    pa_asyncmsgq_post(c->sink_input->sink->asyncmsgq, PA_MSGOBJECT(c->sink_input), SINK_INPUT_MESSAGE_POST_DATA, NULL, 0, &chunk, NULL);
    pa_atomic_sub(&c->playback.missing, r);

    return 0;
}

static int do_write(connection *c) {
    pa_memchunk chunk;
    ssize_t r;
    void *p;

    connection_assert_ref(c);

    if (!c->source_output)
        return 0;

    if (pa_memblockq_peek(c->output_memblockq, &chunk) < 0) {
/*         pa_log("peek failed"); */
        return 0;
    }

    pa_assert(chunk.memblock);
    pa_assert(chunk.length);

    p = pa_memblock_acquire(chunk.memblock);
    r = pa_iochannel_write(c->io, (uint8_t*) p+chunk.index, chunk.length);
    pa_memblock_release(chunk.memblock);

    pa_memblock_unref(chunk.memblock);

    if (r < 0) {

        if (errno == EINTR || errno == EAGAIN)
            return 0;

        pa_log("write(): %s", pa_cstrerror(errno));
        return -1;
    }

    pa_memblockq_drop(c->output_memblockq, r);

    return 0;
}

static void do_work(connection *c) {
    connection_assert_ref(c);

    if (c->dead)
        return;

    if (pa_iochannel_is_readable(c->io)) {
        if (do_read(c) < 0)
            goto fail;
    } else if (pa_iochannel_is_hungup(c->io))
        goto fail;

    if (pa_iochannel_is_writable(c->io)) {
        if (do_write(c) < 0)
            goto fail;
    }

    return;

fail:

    if (c->sink_input) {

        /* If there is a sink input, we first drain what we already have read before shutting down the connection */
        c->dead = 1;

        pa_iochannel_free(c->io);
        c->io = NULL;

        pa_asyncmsgq_post(c->sink_input->sink->asyncmsgq, PA_MSGOBJECT(c->sink_input), SINK_INPUT_MESSAGE_DISABLE_PREBUF, NULL, 0, NULL, NULL);
    } else
        connection_unlink(c);
}

static int connection_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    connection *c = CONNECTION(o);
    connection_assert_ref(c);

    switch (code) {
        case CONNECTION_MESSAGE_REQUEST_DATA:
            do_work(c);
            break;

        case CONNECTION_MESSAGE_POST_DATA:
/*             pa_log("got data %u", chunk->length); */
            pa_memblockq_push_align(c->output_memblockq, chunk);
            do_work(c);
            break;

        case CONNECTION_MESSAGE_UNLINK_CONNECTION:
            connection_unlink(c);
            break;
    }

    return 0;
}

/*** sink_input callbacks ***/

/* Called from thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink_input *i = PA_SINK_INPUT(o);
    connection*c;

    pa_sink_input_assert_ref(i);
    c = CONNECTION(i->userdata);
    connection_assert_ref(c);

    switch (code) {

        case SINK_INPUT_MESSAGE_POST_DATA: {
            pa_assert(chunk);

            /* New data from the main loop */
            pa_memblockq_push_align(c->input_memblockq, chunk);

/*             pa_log("got data, %u", pa_memblockq_get_length(c->input_memblockq)); */

            return 0;
        }

        case SINK_INPUT_MESSAGE_DISABLE_PREBUF: {
            pa_memblockq_prebuf_disable(c->input_memblockq);
            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            *r = pa_bytes_to_usec(pa_memblockq_get_length(c->input_memblockq), &c->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
        }

        default:
            return pa_sink_input_process_msg(o, code, userdata, offset, chunk);
    }
}

/* Called from thread context */
static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    connection *c;
    int r;

    pa_assert(i);
    c = CONNECTION(i->userdata);
    connection_assert_ref(c);
    pa_assert(chunk);

    r = pa_memblockq_peek(c->input_memblockq, chunk);

/*     pa_log("peeked %u %i", r >= 0 ? chunk->length: 0, r); */

    if (c->dead && r < 0)
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(c), CONNECTION_MESSAGE_UNLINK_CONNECTION, NULL, 0, NULL, NULL);

    return r;
}

/* Called from thread context */
static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    connection *c;
    size_t old, new;

    pa_assert(i);
    c = CONNECTION(i->userdata);
    connection_assert_ref(c);
    pa_assert(length);

    old = pa_memblockq_missing(c->input_memblockq);
    pa_memblockq_drop(c->input_memblockq, length);
    new = pa_memblockq_missing(c->input_memblockq);

    if (new > old) {
        if (pa_atomic_add(&c->playback.missing, new - old) <= 0)
            pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(c), CONNECTION_MESSAGE_REQUEST_DATA, NULL, 0, NULL, NULL);
    }
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    connection_unlink(CONNECTION(i->userdata));
}

/*** source_output callbacks ***/

/* Called from thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    connection *c;

    pa_assert(o);
    c = CONNECTION(o->userdata);
    pa_assert(c);
    pa_assert(chunk);

    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(c), CONNECTION_MESSAGE_POST_DATA, NULL, 0, chunk, NULL);
}

/* Called from main context */
static void source_output_kill_cb(pa_source_output *o) {
    pa_source_output_assert_ref(o);

    connection_unlink(CONNECTION(o->userdata));
}

/* Called from main context */
static pa_usec_t source_output_get_latency_cb(pa_source_output *o) {
    connection*c;

    pa_assert(o);
    c = CONNECTION(o->userdata);
    pa_assert(c);

    return pa_bytes_to_usec(pa_memblockq_get_length(c->output_memblockq), &c->source_output->sample_spec);
}

/*** client callbacks ***/

static void client_kill_cb(pa_client *client) {
    connection*c;

    pa_assert(client);
    c = CONNECTION(client->userdata);
    pa_assert(c);

    connection_unlink(c);
}

/*** pa_iochannel callbacks ***/

static void io_callback(pa_iochannel*io, void *userdata) {
    connection *c = CONNECTION(userdata);

    connection_assert_ref(c);
    pa_assert(io);

    do_work(c);
}

/*** socket_server callbacks ***/

static void on_connection(pa_socket_server*s, pa_iochannel *io, void *userdata) {
    pa_protocol_simple *p = userdata;
    connection *c = NULL;
    char cname[256];

    pa_assert(s);
    pa_assert(io);
    pa_assert(p);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_msgobject_new(connection);
    c->parent.parent.free = connection_free;
    c->parent.process_msg = connection_process_msg;
    c->io = io;
    c->sink_input = NULL;
    c->source_output = NULL;
    c->input_memblockq = c->output_memblockq = NULL;
    c->protocol = p;
    c->playback.current_memblock = NULL;
    c->playback.memblock_index = 0;
    c->playback.fragment_size = 0;
    c->dead = 0;
    pa_atomic_store(&c->playback.missing, 0);

    pa_iochannel_socket_peer_to_string(io, cname, sizeof(cname));
    pa_assert_se(c->client = pa_client_new(p->core, __FILE__, cname));
    c->client->owner = p->module;
    c->client->kill = client_kill_cb;
    c->client->userdata = c;

    if (p->mode & PLAYBACK) {
        pa_sink_input_new_data data;
        size_t l;

        pa_sink_input_new_data_init(&data);
        data.driver = __FILE__;
        data.name = c->client->name;
        pa_sink_input_new_data_set_sample_spec(&data, &p->sample_spec);
        data.module = p->module;
        data.client = c->client;

        if (!(c->sink_input = pa_sink_input_new(p->core, &data, 0))) {
            pa_log("Failed to create sink input.");
            goto fail;
        }

        c->sink_input->parent.process_msg = sink_input_process_msg;
        c->sink_input->peek = sink_input_peek_cb;
        c->sink_input->drop = sink_input_drop_cb;
        c->sink_input->kill = sink_input_kill_cb;
        c->sink_input->userdata = c;

        l = (size_t) (pa_bytes_per_second(&p->sample_spec)*PLAYBACK_BUFFER_SECONDS);
        c->input_memblockq = pa_memblockq_new(
                0,
                l,
                0,
                pa_frame_size(&p->sample_spec),
                (size_t) -1,
                l/PLAYBACK_BUFFER_FRAGMENTS,
                NULL);
        pa_iochannel_socket_set_rcvbuf(io, l/PLAYBACK_BUFFER_FRAGMENTS*5);
        c->playback.fragment_size = l/PLAYBACK_BUFFER_FRAGMENTS;

        pa_atomic_store(&c->playback.missing, pa_memblockq_missing(c->input_memblockq));

        pa_sink_input_put(c->sink_input);
    }

    if (p->mode & RECORD) {
        pa_source_output_new_data data;
        size_t l;

        pa_source_output_new_data_init(&data);
        data.driver = __FILE__;
        data.name = c->client->name;
        pa_source_output_new_data_set_sample_spec(&data, &p->sample_spec);
        data.module = p->module;
        data.client = c->client;

        if (!(c->source_output = pa_source_output_new(p->core, &data, 0))) {
            pa_log("Failed to create source output.");
            goto fail;
        }
        c->source_output->push = source_output_push_cb;
        c->source_output->kill = source_output_kill_cb;
        c->source_output->get_latency = source_output_get_latency_cb;
        c->source_output->userdata = c;

        l = (size_t) (pa_bytes_per_second(&p->sample_spec)*RECORD_BUFFER_SECONDS);
        c->output_memblockq = pa_memblockq_new(
                0,
                l,
                0,
                pa_frame_size(&p->sample_spec),
                1,
                0,
                NULL);
        pa_iochannel_socket_set_sndbuf(io, l/RECORD_BUFFER_FRAGMENTS*2);

        pa_source_output_put(c->source_output);
    }

    pa_iochannel_set_callback(c->io, io_callback, c);
    pa_idxset_put(p->connections, c, NULL);

    return;

fail:
    if (c)
        connection_unlink(c);
}

pa_protocol_simple* pa_protocol_simple_new(pa_core *core, pa_socket_server *server, pa_module *m, pa_modargs *ma) {
    pa_protocol_simple* p = NULL;
    pa_bool_t enable;

    pa_assert(core);
    pa_assert(server);
    pa_assert(ma);

    p = pa_xnew0(pa_protocol_simple, 1);
    p->module = m;
    p->core = core;
    p->server = server;
    p->connections = pa_idxset_new(NULL, NULL);

    p->sample_spec = core->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &p->sample_spec) < 0) {
        pa_log("Failed to parse sample type specification.");
        goto fail;
    }

    p->source_name = pa_xstrdup(pa_modargs_get_value(ma, "source", NULL));
    p->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));

    enable = FALSE;
    if (pa_modargs_get_value_boolean(ma, "record", &enable) < 0) {
        pa_log("record= expects a numeric argument.");
        goto fail;
    }
    p->mode = enable ? RECORD : 0;

    enable = 1;
    if (pa_modargs_get_value_boolean(ma, "playback", &enable) < 0) {
        pa_log("playback= expects a numeric argument.");
        goto fail;
    }
    p->mode |= enable ? PLAYBACK : 0;

    if ((p->mode & (RECORD|PLAYBACK)) == 0) {
        pa_log("neither playback nor recording enabled for protocol.");
        goto fail;
    }

    pa_socket_server_set_callback(p->server, on_connection, p);

    return p;

fail:
    if (p)
        pa_protocol_simple_free(p);

    return NULL;
}


void pa_protocol_simple_free(pa_protocol_simple *p) {
    connection *c;
    pa_assert(p);

    if (p->connections) {
        while((c = pa_idxset_first(p->connections, NULL)))
            connection_unlink(c);

        pa_idxset_free(p->connections, NULL, NULL);
    }

    if (p->server)
        pa_socket_server_unref(p->server);

    pa_xfree(p);
}
