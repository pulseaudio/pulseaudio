/* $Id$ */

/***
  This file is part of PulseAudio.
 
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
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#include <jack/jack.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulse/mainloop-api.h>

#include "module-jack-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Jack Sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name of sink> "
        "server_name=<jack server name> "
        "client_name=<jack client name> "
        "channels=<number of channels> "
        "connect=<connect ports?> "
        "channel_map=<channel map>")

#define DEFAULT_SINK_NAME "jack_out"

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_sink *sink;

    unsigned channels;

    jack_port_t* port[PA_CHANNELS_MAX];
    jack_client_t *client;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
    void * buffer[PA_CHANNELS_MAX];
    jack_nframes_t frames_requested;
    int quit_requested;

    int pipe_fd_type;
    int pipe_fds[2];
    pa_io_event *io_event;

    jack_nframes_t frames_in_buffer;
    jack_nframes_t timestamp;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "server_name",
    "client_name",
    "channels",
    "connect",
    "channel_map",
    NULL
};

static void stop_sink(struct userdata *u) {
    assert (u);
    
    jack_client_close(u->client);
    u->client = NULL;
    u->core->mainloop->io_free(u->io_event);
    u->io_event = NULL;
    pa_sink_disconnect(u->sink);
    pa_sink_unref(u->sink);
    u->sink = NULL;
    pa_module_unload_request(u->module);
}

static void io_event_cb(pa_mainloop_api *m, pa_io_event *e, int fd, pa_io_event_flags_t flags, void *userdata) {
    struct userdata *u = userdata;
    char x;
    
    assert(m);
    assert(e);
    assert(flags == PA_IO_EVENT_INPUT);
    assert(u);
    assert(u->pipe_fds[0] == fd);

    pa_read(fd, &x, 1, &u->pipe_fd_type);
    
    if (u->quit_requested) {
        stop_sink(u);
        u->quit_requested = 0;
        return;
    }
    
    pthread_mutex_lock(&u->mutex);

    if (u->frames_requested > 0) {
        unsigned fs;
        jack_nframes_t frame_idx;
        pa_memchunk chunk;
        void *p;
        
        fs = pa_frame_size(&u->sink->sample_spec);

        pa_sink_render_full(u->sink, u->frames_requested * fs, &chunk);
        p = pa_memblock_acquire(chunk.memblock);

        for (frame_idx = 0; frame_idx < u->frames_requested; frame_idx ++) {
            unsigned c;
                
            for (c = 0; c < u->channels; c++) {
                float *s = ((float*) ((uint8_t*) p + chunk.index)) + (frame_idx * u->channels) + c;
                float *d = ((float*) u->buffer[c]) + frame_idx;
                
                *d = *s;
            }
        }
        
        pa_memblock_release(chunk.memblock);
        pa_memblock_unref(chunk.memblock);

        u->frames_requested = 0;
        
        pthread_cond_signal(&u->cond);
    }

    pthread_mutex_unlock(&u->mutex);
}

static void request_render(struct userdata *u) {
    char c = 'x';
    assert(u);

    assert(u->pipe_fds[1] >= 0);
    pa_write(u->pipe_fds[1], &c, 1, &u->pipe_fd_type);
}

static void jack_shutdown(void *arg) {
    struct userdata *u = arg;
    assert(u);

    u->quit_requested = 1;
    request_render(u);
}

static int jack_process(jack_nframes_t nframes, void *arg) {
    struct userdata *u = arg;
    assert(u);

    if (jack_transport_query(u->client, NULL) == JackTransportRolling) {
        unsigned c;
        
        pthread_mutex_lock(&u->mutex);
        
        u->frames_requested = nframes;
        
        for (c = 0; c < u->channels; c++) {
            u->buffer[c] = jack_port_get_buffer(u->port[c], nframes);
            assert(u->buffer[c]);
        }
        
        request_render(u);
        
        pthread_cond_wait(&u->cond, &u->mutex);

        u->frames_in_buffer = nframes;
        u->timestamp = jack_get_current_transport_frame(u->client);
        
        pthread_mutex_unlock(&u->mutex);
    }
    
    return 0;
}

static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    struct userdata *u;
    jack_nframes_t n, l, d;
    
    assert(s);
    u = s->userdata;
    
    if (jack_transport_query(u->client, NULL) != JackTransportRolling)
        return 0;

    n = jack_get_current_transport_frame(u->client);

    if (n < u->timestamp)
        return 0;

    d = n - u->timestamp;
    l = jack_port_get_total_latency(u->client, u->port[0]) + u->frames_in_buffer;

    if (d >= l)
        return 0;
    
    return pa_bytes_to_usec((l - d) * pa_frame_size(&s->sample_spec), &s->sample_spec);
}

static void jack_error_func(const char*t) {
    pa_log_warn("JACK error >%s<", t);
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    jack_status_t status;
    const char *server_name, *client_name;
    uint32_t channels = 0;
    int do_connect = 1;
    unsigned i;
    const char **ports = NULL, **p;
    char *t;
    
    assert(c);
    assert(m);

    jack_set_error_function(jack_error_func);
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "connect", &do_connect) < 0) {
        pa_log("failed to parse connect= argument.");
        goto fail;
    }
        
    server_name = pa_modargs_get_value(ma, "server_name", NULL);
    client_name = pa_modargs_get_value(ma, "client_name", "PulseAudio");

    u = pa_xnew0(struct userdata, 1);
    m->userdata = u;
    u->core = c;
    u->module = m;
    u->pipe_fds[0] = u->pipe_fds[1] = -1;
    u->pipe_fd_type = 0;

    pthread_mutex_init(&u->mutex, NULL);
    pthread_cond_init(&u->cond, NULL);
    
    if (pipe(u->pipe_fds) < 0) {
        pa_log("pipe() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    pa_make_nonblock_fd(u->pipe_fds[1]);
    
    if (!(u->client = jack_client_open(client_name, server_name ? JackServerName : JackNullOption, &status, server_name))) {
        pa_log("jack_client_open() failed.");
        goto fail;
    }

    ports = jack_get_ports(u->client, NULL, NULL, JackPortIsPhysical|JackPortIsInput);
    
    channels = 0;
    for (p = ports; *p; p++)
        channels++;

    if (!channels)
        channels = c->default_sample_spec.channels;
    
    if (pa_modargs_get_value_u32(ma, "channels", &channels) < 0 || channels <= 0 || channels >= PA_CHANNELS_MAX) {
        pa_log("failed to parse channels= argument.");
        goto fail;
    }

    pa_channel_map_init_auto(&map, channels, PA_CHANNEL_MAP_ALSA);
    if (pa_modargs_get_channel_map(ma, &map) < 0 || map.channels != channels) {
        pa_log("failed to parse channel_map= argument.");
        goto fail;
    }
    
    pa_log_info("Successfully connected as '%s'", jack_get_client_name(u->client));

    ss.channels = u->channels = channels;
    ss.rate = jack_get_sample_rate(u->client);
    ss.format = PA_SAMPLE_FLOAT32NE;

    assert(pa_sample_spec_valid(&ss));

    for (i = 0; i < ss.channels; i++) {
        if (!(u->port[i] = jack_port_register(u->client, pa_channel_position_to_string(map.map[i]), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal, 0))) {
            pa_log("jack_port_register() failed.");
            goto fail;
        }
    }

    if (!(u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map))) {
        pa_log("failed to create sink.");
        goto fail;
    }

    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Jack sink (%s)", jack_get_client_name(u->client)));
    pa_xfree(t);
    u->sink->get_latency = sink_get_latency_cb;

    jack_set_process_callback(u->client, jack_process, u);
    jack_on_shutdown(u->client, jack_shutdown, u);

    if (jack_activate(u->client)) {
        pa_log("jack_activate() failed");
        goto fail;
    }

    if (do_connect) {
        for (i = 0, p = ports; i < ss.channels; i++, p++) {

            if (!*p) {
                pa_log("not enough physical output ports, leaving unconnected.");
                break;
            }

            pa_log_info("connecting %s to %s", jack_port_name(u->port[i]), *p);
            
            if (jack_connect(u->client, jack_port_name(u->port[i]), *p)) {
                pa_log("failed to connect %s to %s, leaving unconnected.", jack_port_name(u->port[i]), *p);
                break;
            }
        }

    }

    u->io_event = c->mainloop->io_new(c->mainloop, u->pipe_fds[0], PA_IO_EVENT_INPUT, io_event_cb, u);
    
    free(ports);
    pa_modargs_free(ma);
    
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    free(ports);
        
    pa__done(c, m);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    if (u->client)
        jack_client_close(u->client);

    if (u->io_event)
        c->mainloop->io_free(u->io_event);

    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->pipe_fds[0] >= 0)
        close(u->pipe_fds[0]);
    if (u->pipe_fds[1] >= 0)
        close(u->pipe_fds[1]);

    pthread_mutex_destroy(&u->mutex);
    pthread_cond_destroy(&u->cond);
    pa_xfree(u);
}
