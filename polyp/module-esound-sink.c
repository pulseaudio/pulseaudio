/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "iochannel.h"
#include "sink.h"
#include "module.h"
#include "util.h"
#include "modargs.h"
#include "xmalloc.h"
#include "log.h"
#include "module-esound-sink-symdef.h"
#include "socket-client.h"
#include "esound.h"
#include "authkey.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Esound ")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("sink_name=<name for the sink> server=<address> cookie=<filename>  format=<sample format> channels=<number of channels> rate=<sample rate>")

#define DEFAULT_SINK_NAME "esound_output"

struct userdata {
    struct pa_core *core;

    struct pa_sink *sink;
    struct pa_iochannel *io;
    struct pa_socket_client *client;

    struct pa_defer_event *defer_event;

    struct pa_memchunk memchunk;
    struct pa_module *module;

    void *write_data;
    size_t write_length, write_index;
    
    void *read_data;
    size_t read_length, read_index;

    enum { STATE_AUTH, STATE_LATENCY, STATE_RUNNING, STATE_DEAD } state;

    pa_usec_t latency;

    esd_format_t format;
    int32_t rate;
};

static const char* const valid_modargs[] = {
    "server",
    "cookie",
    "rate",
    "format",
    "channels",
    "sink_name",
    NULL
};

static void cancel(struct userdata *u) {
    assert(u);

    u->state = STATE_DEAD;

    if (u->io) {
        pa_iochannel_free(u->io);
        u->io = NULL;
    }

    if (u->defer_event) {
        u->core->mainloop->defer_free(u->defer_event);
        u->defer_event = NULL;
    }

    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }

    if (u->module) {
        pa_module_unload_request(u->module);
        u->module = NULL;
    }
}

static int do_write(struct userdata *u) {
    ssize_t r;
    assert(u);

    if (!pa_iochannel_is_writable(u->io))
        return 0;

    if (u->write_data) {
        assert(u->write_index < u->write_length);

        if ((r = pa_iochannel_write(u->io, (uint8_t*) u->write_data + u->write_index, u->write_length - u->write_index)) <= 0) {
            pa_log(__FILE__": write() failed: %s\n", strerror(errno));
            return -1;
        }

        u->write_index += r;
        assert(u->write_index <= u->write_length);
        
        if (u->write_index == u->write_length) {
            free(u->write_data);
            u->write_data = NULL;
            u->write_index = u->write_length = 0;
        }
    } else if (u->state == STATE_RUNNING) {
        pa_module_set_used(u->module, pa_idxset_ncontents(u->sink->inputs) + pa_idxset_ncontents(u->sink->monitor_source->outputs));
        
        if (!u->memchunk.length)
            if (pa_sink_render(u->sink, PIPE_BUF, &u->memchunk) < 0)
                return 0;

        assert(u->memchunk.memblock && u->memchunk.length);
        
        if ((r = pa_iochannel_write(u->io, (uint8_t*) u->memchunk.memblock->data + u->memchunk.index, u->memchunk.length)) < 0) {
            pa_log(__FILE__": write() failed: %s\n", strerror(errno));
            return -1;
        }

        u->memchunk.index += r;
        u->memchunk.length -= r;
        
        if (u->memchunk.length <= 0) {
            pa_memblock_unref(u->memchunk.memblock);
            u->memchunk.memblock = NULL;
        }
    }
    
    return 0;
}

static int handle_response(struct userdata *u) {
    assert(u);

    switch (u->state) {
        case STATE_AUTH:
            assert(u->read_length == sizeof(int32_t));

            /* Process auth data */
            if (!*(int32_t*) u->read_data) {
                pa_log(__FILE__": Authentication failed: %s\n", strerror(errno));
                return -1;
            }

            /* Request latency data */
            assert(!u->write_data);
            *(int32_t*) (u->write_data = pa_xmalloc(u->write_length = sizeof(int32_t))) = ESD_PROTO_LATENCY;

            u->write_index = 0;
            u->state = STATE_LATENCY;

            /* Space for next response */
            assert(u->read_length >= sizeof(int32_t));
            u->read_index = 0;
            u->read_length = sizeof(int32_t);
            
            break;

        case STATE_LATENCY: {
            int32_t *p;
            assert(u->read_length == sizeof(int32_t));

            /* Process latency info */
            u->latency = (pa_usec_t) ((double) (*(int32_t*) u->read_data) * 1000000 / 44100);
            if (u->latency > 10000000) {
                pa_log(__FILE__": WARNING! Invalid latency information received from server\n");
                u->latency = 0;
            }

            /* Create stream */
            assert(!u->write_data);
            p = u->write_data = pa_xmalloc0(u->write_length = sizeof(int32_t)*3+ESD_NAME_MAX);
            *(p++) = ESD_PROTO_STREAM_PLAY;
            *(p++) = u->format;
            *(p++) = u->rate;
            pa_strlcpy((char*) p, "Polypaudio Tunnel", ESD_NAME_MAX);

            u->write_index = 0;
            u->state = STATE_RUNNING;

            /* Don't read any further */
            pa_xfree(u->read_data);
            u->read_data = NULL;
            u->read_index = u->read_length = 0;
            
            break;
        }
            
        default:
            abort();
    }

    return 0;
}

static int do_read(struct userdata *u) {
    assert(u);
    
    if (!pa_iochannel_is_readable(u->io))
        return 0;
    
    if (u->state == STATE_AUTH || u->state == STATE_LATENCY) {
        ssize_t r;
        
        if (!u->read_data)
            return 0;
        
        assert(u->read_index < u->read_length);
        
        if ((r = pa_iochannel_read(u->io, (uint8_t*) u->read_data + u->read_index, u->read_length - u->read_index)) <= 0) {
            pa_log(__FILE__": read() failed: %s\n", r < 0 ? strerror(errno) : "EOF");
            cancel(u);
            return -1;
        }

        u->read_index += r;
        assert(u->read_index <= u->read_length);

        if (u->read_index == u->read_length)
            return handle_response(u);
    }

    return 0;
}

static void do_work(struct userdata *u) {
    assert(u);

    u->core->mainloop->defer_enable(u->defer_event, 0);
    
    if (do_read(u) < 0 || do_write(u) < 0)
        cancel(u);
}

static void notify_cb(struct pa_sink*s) {
    struct userdata *u = s->userdata;
    assert(s && u);

    if (pa_iochannel_is_writable(u->io))
        u->core->mainloop->defer_enable(u->defer_event, 1);
}

static pa_usec_t get_latency_cb(struct pa_sink *s) {
    struct userdata *u = s->userdata;
    assert(s && u);

    return
        u->latency +
        (u->memchunk.memblock ? pa_bytes_to_usec(u->memchunk.length, &s->sample_spec) : 0);
}

static void defer_callback(struct pa_mainloop_api *m, struct pa_defer_event*e, void *userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_work(u);
}

static void io_callback(struct pa_iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_work(u);
}

static void on_connection(struct pa_socket_client *c, struct pa_iochannel*io, void *userdata) {
    struct userdata *u = userdata;

    pa_socket_client_unref(u->client);
    u->client = NULL;
    
    if (!io) {
        pa_log(__FILE__": connection failed: %s\n", strerror(errno));
        cancel(u);
        return;
    }
    
    u->io = io;
    pa_iochannel_set_callback(u->io, io_callback, u);
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u = NULL;
    const char *p;
    struct pa_sample_spec ss;
    struct pa_modargs *ma = NULL;
    assert(c && m);
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": invalid sample format specification\n");
        goto fail;
    }

    if ((ss.format != PA_SAMPLE_U8 && ss.format != PA_SAMPLE_S16NE) ||
        (ss.channels > 2)) {
        pa_log(__FILE__": esound sample type support is limited to mono/stereo and U8 or S16NE sample data\n");
        goto fail;
    }
        
    u = pa_xmalloc0(sizeof(struct userdata));
    u->core = c;
    u->module = m;
    m->userdata = u;
    u->format =
        (ss.format == PA_SAMPLE_U8 ? ESD_BITS8 : ESD_BITS16) |
        (ss.channels == 2 ? ESD_STEREO : ESD_MONO);
    u->rate = ss.rate;
    u->sink = NULL;
    u->client = NULL;
    u->io = NULL;
    u->read_data = u->write_data = NULL;
    u->read_index = u->write_index = u->read_length = u->write_length = 0;
    u->state = STATE_AUTH;
    u->latency = 0;

    if (!(u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss))) {
        pa_log(__FILE__": failed to create sink.\n");
        goto fail;
    }

    if (!(u->client = pa_socket_client_new_string(u->core->mainloop, p = pa_modargs_get_value(ma, "server", ESD_UNIX_SOCKET_NAME), ESD_DEFAULT_PORT))) {
        pa_log(__FILE__": failed to connect to server.\n");
        goto fail;
    }
    pa_socket_client_set_callback(u->client, on_connection, u);

    /* Prepare the initial request */
    u->write_data = pa_xmalloc(u->write_length = ESD_KEY_LEN + sizeof(int32_t));
    if (pa_authkey_load_auto(pa_modargs_get_value(ma, "cookie", ".esd_auth"), u->write_data, ESD_KEY_LEN) < 0) {
        pa_log(__FILE__": failed to load cookie\n");
        goto fail;
    }
    *(int32_t*) ((uint8_t*) u->write_data + ESD_KEY_LEN) = ESD_ENDIAN_KEY;

    /* Reserve space for the response */
    u->read_data = pa_xmalloc(u->read_length = sizeof(int32_t));
    
    u->sink->notify = notify_cb;
    u->sink->get_latency = get_latency_cb;
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_sprintf_malloc("Esound sink '%s'", p);

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;

    u->defer_event = c->mainloop->defer_new(c->mainloop, defer_callback, u);
    c->mainloop->defer_enable(u->defer_event, 0);

    
    pa_modargs_free(ma);
    
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
        
    pa__done(c, m);

    return -1;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    u->module = NULL;
    cancel(u);
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->client)
        pa_socket_client_unref(u->client);
    
    pa_xfree(u->read_data);
    pa_xfree(u->write_data);

    pa_xfree(u);
}



