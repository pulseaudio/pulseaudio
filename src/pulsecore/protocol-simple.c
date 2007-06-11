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

#include "protocol-simple.h"

/* Don't allow more than this many concurrent connections */
#define MAX_CONNECTIONS 10

struct connection {
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
        pa_atomic_int missing;
    } playback;
};

struct pa_protocol_simple {
    pa_module *module;
    pa_core *core;
    pa_socket_server*server;
    pa_idxset *connections;

    pa_asyncmsgq *asyncmsgq;

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
};

enum {
    MESSAGE_REQUEST_DATA,   /* data from source output to main loop */
    MESSAGE_POST_DATA       /* data from source output to main loop */
};


#define PLAYBACK_BUFFER_SECONDS (.5)
#define PLAYBACK_BUFFER_FRAGMENTS (10)
#define RECORD_BUFFER_SECONDS (5)
#define RECORD_BUFFER_FRAGMENTS (100)

static void connection_free(struct connection *c) {
    pa_assert(c);

    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);

    if (c->sink_input) {
        pa_sink_input_disconnect(c->sink_input);
        pa_sink_input_unref(c->sink_input);
    }

    if (c->source_output) {
        pa_source_output_disconnect(c->source_output);
        pa_source_output_unref(c->source_output);
    }

    if (c->playback.current_memblock)
        pa_memblock_unref(c->playback.current_memblock);

    if (c->client)
        pa_client_free(c->client);
    if (c->io)
        pa_iochannel_free(c->io);
    if (c->input_memblockq)
        pa_memblockq_free(c->input_memblockq);
    if (c->output_memblockq)
        pa_memblockq_free(c->output_memblockq);

    pa_xfree(c);
}

static int do_read(struct connection *c) {
    pa_memchunk chunk;
    ssize_t r;
    size_t l;
    void *p;

    pa_assert(c);

    if (!c->sink_input || !(l = pa_atomic_load(&c->playback.missing)))
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

        if (errno == EINTR || errno == EAGAIN)
            return 0;

        pa_log_debug("read(): %s", r == 0 ? "EOF" : pa_cstrerror(errno));
        return -1;
    }

    chunk.memblock = c->playback.current_memblock;
    chunk.index = c->playback.memblock_index;
    chunk.length = r;

    c->playback.memblock_index += r;

    pa_asyncmsgq_post(c->protocol->asyncmsgq, c, MESSAGE_POST_DATA, NULL, &chunk, NULL, NULL);

    return 0;
}

static int do_write(struct connection *c) {
    pa_memchunk chunk;
    ssize_t r;
    void *p;

    p_assert(c);

    if (!c->source_output)
        return 0;

    if (pa_memblockq_peek(c->output_memblockq, &chunk) < 0)
        return 0;

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

    pa_memblockq_drop(c->output_memblockq, &chunk, r);

    return 0;
}

static void do_work(struct connection *c) {
    pa_assert(c);

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

        pa_memblockq_prebuf_disable(c->input_memblockq);
    } else
        connection_free(c);
}

/*** sink_input callbacks ***/

/* Called from thread context */
static int sink_input_process_msg(pa_sink_input *i, int code, void *userdata, const pa_memchunk *chunk) {
    struct connection*c;

    pa_assert(i);
    c = i->userdata;
    pa_assert(c);

    switch (code) {

        case SINK_INPUT_MESSAGE_POST_DATA: {
            pa_assert(chunk);

            /* New data from the main loop */
            pa_memblockq_push_align(c->input_memblockq, chunk);
            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            *r = pa_bytes_to_usec(pa_memblockq_get_length(c->input_memblockq), &c->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
        }

        default:
            return pa_sink_input_process_msg(i, code, userdata);
    }
}

/* Called from thread context */
static int sink_input_peek_cb(pa_sink_input *i, pa_memchunk *chunk) {
    struct connection*c;

    pa_assert(i);
    c = i->userdata;
    pa_assert(c);
    pa_assert(chunk);

    r = pa_memblockq_peek(c->input_memblockq, chunk);

    if (c->dead && r < 0)
        connection_free(c);

    return r;
}

/* Called from thread context */
static void sink_input_drop_cb(pa_sink_input *i, const pa_memchunk *chunk, size_t length) {
    struct connection*c = i->userdata;
    size_t old, new;

    pa_assert(i);
    pa_assert(c);
    pa_assert(length);

    old = pa_memblockq_missing(c->input_memblockq);
    pa_memblockq_drop(c->input_memblockq, chunk, length);
    new = pa_memblockq_missing(c->input_memblockq);

    pa_atomic_store(&c->playback.missing, &new);

    if (new > old)
        pa_asyncmsgq_post(c->protocol->asyncmsgq, c, MESSAGE_REQUEST_DATA, NULL, NULL, NULL, NULL);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    pa_assert(i);
    pa_assert(i->userdata);

    connection_free((struct connection *) i->userdata);
}

/*** source_output callbacks ***/

static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct connection *c;

    pa_assert(o);
    c = o->userdata;
    pa_assert(c);
    pa_assert(chunk);

    pa_asyncmsgq_post(c->protocol->asyncmsgq, c, MESSAGE_REQUEST_DATA, NULL, chunk, NULL, NULL);
}

static void source_output_kill_cb(pa_source_output *o) {
    struct connection*c;

    pa_assert(o);
    c = o->userdata;
    pa_assert(c);

    connection_free(c);
}

static pa_usec_t source_output_get_latency_cb(pa_source_output *o) {
    struct connection*c;

    pa_assert(o);
    c = o->userdata;
    pa_assert(c);

    return pa_bytes_to_usec(pa_memblockq_get_length(c->output_memblockq), &c->source_output->sample_spec);
}

/*** client callbacks ***/

static void client_kill_cb(pa_client *client) {
    struct connection*c;

    pa_assert(client);
    c = client->userdata;
    pa_assert(c);

    connection_free(client);
}

/*** pa_iochannel callbacks ***/

static void io_callback(pa_iochannel*io, void *userdata) {
    struct connection *c = userdata;

    pa_assert(io);
    pa_assert(c);

    do_work(c);
}

/*** socket_server callbacks ***/

static void on_connection(pa_socket_server*s, pa_iochannel *io, void *userdata) {
    pa_protocol_simple *p = userdata;
    struct connection *c = NULL;
    char cname[256];

    pa_assert(s);
    pa_assert(io);
    pa_assert(p);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_xnew(struct connection, 1);
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

        c->sink_input->peek = sink_input_peek_cb;
        c->sink_input->drop = sink_input_drop_cb;
        c->sink_input->kill = sink_input_kill_cb;
        c->sink_input->get_latency = sink_input_get_latency_cb;
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
        pa_assert(c->input_memblockq);
        pa_iochannel_socket_set_rcvbuf(io, l/PLAYBACK_BUFFER_FRAGMENTS*5);
        c->playback.fragment_size = l/10;

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
        connection_free(c);
}

static void asyncmsgq_cb(pa_mainloop_api*api, pa_io_event* e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_protocol_simple *p = userdata;
    int do_some_work = 0;

    pa_assert(pa_asyncmsgq_get_fd(p->asyncmsgq) == fd);
    pa_assert(events == PA_IO_EVENT_INPUT);

    pa_asyncmsgq_after_poll(p->asyncmsgq);

    for (;;) {
        int code;
        void *object, *data;

        /* Check whether there is a message for us to process */
        while (pa_asyncmsgq_get(p->asyncmsgq, &object, &code, &data) == 0) {

            connection *c = object;

            pa_assert(c);

            switch (code) {

                case MESSAGE_REQUEST_DATA:
                    do_work(c);
                    break;

                case MESSAGE_POST_DATA:
                    pa_memblockq_push(c->output_memblockq, chunk);
                    do_work(c);
                    break;
            }

            pa_asyncmsgq_done(p->asyncmsgq);
        }

        if (pa_asyncmsgq_before_poll(p->asyncmsgq) == 0)
            break;
    }
}

pa_protocol_simple* pa_protocol_simple_new(pa_core *core, pa_socket_server *server, pa_module *m, pa_modargs *ma) {
    pa_protocol_simple* p = NULL;
    int enable;

    pa_assert(core);
    pa_assert(server);
    pa_assert(ma);

    p = pa_xnew0(pa_protocol_simple, 1);
    p->module = m;
    p->core = core;
    p->server = server;
    p->connections = pa_idxset_new(NULL, NULL);
    pa_assert_se(p->asyncmsgq = pa_asyncmsgq_new(0));

    p->sample_spec = core->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &p->sample_spec) < 0) {
        pa_log("Failed to parse sample type specification.");
        goto fail;
    }

    p->source_name = pa_xstrdup(pa_modargs_get_value(ma, "source", NULL));
    p->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));

    enable = 0;
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

    pa_assert_se(pa_asyncmsgq_before_poll(p->asyncmsgq) == 0);
    pa_assert_se(p->asyncmsgq_event = core->mainloop->io_event_new(core->mainloop, pa_asyncmsgq_get_fd(p->asyncmsgq), PA_IO_EVENT_INPUT, p));

    return p;

fail:
    if (p)
        pa_protocol_simple_free(p);

    return NULL;
}


void pa_protocol_simple_free(pa_protocol_simple *p) {
    struct connection *c;
    pa_assert(p);

    if (p->connections) {
        while((c = pa_idxset_first(p->connections, NULL)))
            connection_free(c);

        pa_idxset_free(p->connections, NULL, NULL);
    }

    if (p->server)
        pa_socket_server_unref(p->server);

    if (p->asyncmsgq) {
        c->mainloop->io_event_free(c->asyncmsgq_event);
        pa_asyncmsgq_after_poll(c->asyncmsgq);
        pa_asyncmsgq_free(p->asyncmsgq);
    }

    pa_xfree(p);
}

