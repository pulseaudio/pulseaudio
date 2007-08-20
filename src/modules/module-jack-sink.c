/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006, 2007 Lennart Poettering and Tanu Kaskinen

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

#include <pthread.h>
#include <assert.h>
#include <errno.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <jack/types.h>

#include <pulse/mainloop-api.h>
#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/xmalloc.h>

#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/pipe.h>
#include <pulsecore/modargs.h>
#include <pulsecore/strbuf.h>

#include "module-jack-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering & Tanu Kaskinen")
PA_MODULE_DESCRIPTION("Jack Sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name of sink> "
        "server_name=<jack server name> "
        "client_name=<jack client name> "
        "channels=<number of channels> "
        "connect=<connect ports automatically?> "
        "buffersize=<intermediate buffering in frames> "
        "channel_map=<channel map>")

#define DEFAULT_SINK_NAME "jack_out"
#define DEFAULT_CLIENT_NAME "PulseAudio(output)"
#define DEFAULT_RINGBUFFER_SIZE 4096


struct userdata {
    pa_sink *sink;

    unsigned channels;
    unsigned frame_size;

    jack_port_t* j_ports[PA_CHANNELS_MAX];
    jack_client_t *j_client;

    jack_nframes_t j_buffersize;

    /* For avoiding j_buffersize changes at a wrong moment. */
    pthread_mutex_t buffersize_mutex;

    /* The intermediate store where the pulse side writes to and the jack side
       reads from. */
    jack_ringbuffer_t* ringbuffer;
    
    /* For signaling when there's room in the ringbuffer. */
    pthread_mutex_t cond_mutex;
    pthread_cond_t ringbuffer_cond;

    pthread_t filler_thread; /* Keeps the ringbuffer filled. */

    int ringbuffer_is_full;
    int filler_thread_is_running;
    int quit_requested;

    int pipe_fd_type;
    int pipe_fds[2];
    pa_io_event *io_event;
};


struct options {
    char* sink_name;
    int sink_name_given;

    char* server_name; /* May be NULL */
    int server_name_given;

    char* client_name;
    int client_name_given;

    unsigned channels;
    int channels_given;

    int connect;
    int connect_given;

    unsigned buffersize;
    int buffersize_given;

    pa_channel_map map;
    int map_given;
};


static const char* const valid_modargs[] = {
    "sink_name",
    "server_name",
    "client_name",
    "channels",
    "connect",
    "buffersize",
    "channel_map",
    NULL
};


/* Initialization functions. */
static int parse_options(struct options* o, const char* argument);
static void set_default_channels(pa_module* self, struct options* o);
static int create_sink(pa_module* self, struct options *o);
static void connect_ports(pa_module* self);
static int start_filling_ringbuffer(pa_module* self);

/* Various callbacks. */
static void jack_error_func(const char* t);
static pa_usec_t sink_get_latency_cb(pa_sink* s);
static int jack_process(jack_nframes_t nframes, void* arg);
static int jack_blocksize_cb(jack_nframes_t nframes, void* arg);
static void jack_shutdown(void* arg);
static void io_event_cb(pa_mainloop_api* m, pa_io_event* e, int fd,
                        pa_io_event_flags_t flags, void* userdata);

/* The ringbuffer filler thread runs in this function. */
static void* fill_ringbuffer(void* arg);

/* request_render asks asynchronously the mainloop to call io_event_cb. */
static void request_render(struct userdata* u);


int pa__init(pa_core* c, pa_module* self) {
    struct userdata* u = NULL;
    struct options o;
    unsigned i;
    
    assert(c);
    assert(self);

    o.sink_name = NULL;
    o.server_name = NULL;
    o.client_name = NULL;
    
    self->userdata = pa_xnew0(struct userdata, 1);
    u = self->userdata;
    
    u->pipe_fds[0] = u->pipe_fds[1] = -1;
    u->pipe_fd_type = 0;
    u->ringbuffer_is_full = 0;
    u->filler_thread_is_running = 0;
    u->quit_requested = 0;
    pthread_mutex_init(&u->buffersize_mutex, NULL);
    pthread_mutex_init(&u->cond_mutex, NULL);
    pthread_cond_init(&u->ringbuffer_cond, NULL);
    
    if (parse_options(&o, self->argument) != 0)
        goto fail;
    
    jack_set_error_function(jack_error_func);
    
    if (!(u->j_client = jack_client_open(
                          o.client_name,
                          o.server_name ? JackServerName : JackNullOption,
                          NULL, o.server_name))) {
        pa_log_error("jack_client_open() failed.");
        goto fail;
    }
    pa_log_info("Successfully connected as '%s'",
                jack_get_client_name(u->j_client));
    
    if (!o.channels_given)
        set_default_channels(self, &o);
    
    u->channels = o.channels;
    
    if (!o.map_given)
        pa_channel_map_init_auto(&o.map, u->channels, PA_CHANNEL_MAP_ALSA);
    
    for (i = 0; i < u->channels; i++) {
        char* port_name = pa_sprintf_malloc(
                              "out_%i:%s", i+1,
                              pa_channel_position_to_string(o.map.map[i]));
        
        if (!(u->j_ports[i] = jack_port_register(
                                  u->j_client, port_name,
                                  JACK_DEFAULT_AUDIO_TYPE,
                                  JackPortIsOutput|JackPortIsTerminal, 0))) {
            pa_log("jack_port_register() failed.");
            goto fail;
        }
        
        pa_xfree(port_name);
    }
    
    if (pipe(u->pipe_fds) < 0) {
        pa_log("pipe() failed: %s", pa_cstrerror(errno));
        goto fail;
    }
    pa_make_nonblock_fd(u->pipe_fds[1]);
    
    if (create_sink(self, &o) != 0)
        goto fail;

    u->frame_size = pa_frame_size(&u->sink->sample_spec);
    u->j_buffersize = jack_get_buffer_size(u->j_client);
    
    /* If the ringbuffer size were equal to the jack buffer size, a full block
       would never fit in the ringbuffer, because the ringbuffer can never be
       totally full: one slot is always wasted. */
    if (o.buffersize <= u->j_buffersize) {
        o.buffersize = u->j_buffersize + 1;
    }
    /* The actual ringbuffer size will be rounded up to the nearest power of
       two. */
    if (!(u->ringbuffer = jack_ringbuffer_create(
                              o.buffersize * u->frame_size))) {
        pa_log("jack_ringbuffer_create() failed.");
        goto fail;
    }
    assert((u->ringbuffer->size % sizeof(float)) == 0);
    pa_log_info("buffersize is %u frames (%u samples, %u bytes).",
                u->ringbuffer->size / u->frame_size,
                u->ringbuffer->size / sizeof(float),
                u->ringbuffer->size);
    
    jack_set_process_callback(u->j_client, jack_process, u);
    jack_set_buffer_size_callback(u->j_client, jack_blocksize_cb, u);
    jack_on_shutdown(u->j_client, jack_shutdown, u);
    
    if (jack_activate(u->j_client)) {
        pa_log("jack_activate() failed.");
        goto fail;
    }

    if (o.connect)
        connect_ports(self);

    u->io_event = c->mainloop->io_new(c->mainloop, u->pipe_fds[0],
                                      PA_IO_EVENT_INPUT, io_event_cb, self);
    
    if (start_filling_ringbuffer(self) != 0)
        goto fail;

    pa_xfree(o.sink_name);
    pa_xfree(o.server_name);
    pa_xfree(o.client_name);
    
    return 0;

fail:
    pa_xfree(o.sink_name);
    pa_xfree(o.server_name);
    pa_xfree(o.client_name);
    pa__done(c, self);

    return -1;
}


static int parse_options(struct options* o, const char* argument) {
    pa_modargs *ma = NULL;
    const char* arg_val;
    pa_strbuf* strbuf;
    
    assert(o);

    if (!(ma = pa_modargs_new(argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments.");
        goto fail;
    }

    strbuf = pa_strbuf_new();
    if ((arg_val = pa_modargs_get_value(ma, "sink_name", NULL))) {
        pa_strbuf_puts(strbuf, arg_val);
        o->sink_name = pa_strbuf_tostring(strbuf);
        o->sink_name_given = 1;
    } else {
        pa_strbuf_puts(strbuf, DEFAULT_SINK_NAME);
        o->sink_name = pa_strbuf_tostring(strbuf);
        o->sink_name_given = 0;
    }
    pa_strbuf_free(strbuf);

    strbuf = pa_strbuf_new();
    if ((arg_val = pa_modargs_get_value(ma, "server_name", NULL))) {
        pa_strbuf_puts(strbuf, arg_val);
        o->server_name = pa_strbuf_tostring(strbuf);
        o->server_name_given = 1;
    } else {
        o->server_name = NULL;
        o->server_name_given = 0;
    }
    pa_strbuf_free(strbuf);

    strbuf = pa_strbuf_new();
    if ((arg_val = pa_modargs_get_value(ma, "client_name", NULL))) {
        pa_strbuf_puts(strbuf, arg_val);
        o->client_name = pa_strbuf_tostring(strbuf);
        o->client_name_given = 1;
    } else {
        pa_strbuf_puts(strbuf, DEFAULT_CLIENT_NAME);
        o->client_name = pa_strbuf_tostring(strbuf);
        o->client_name_given = 1;
    }
    pa_strbuf_free(strbuf);

    if (pa_modargs_get_value(ma, "channels", NULL)) {
        o->channels_given = 1;
        if (pa_modargs_get_value_u32(ma, "channels", &o->channels) < 0 ||
            o->channels == 0 ||
            o->channels >= PA_CHANNELS_MAX) {
            pa_log_error("Failed to parse the \"channels\" argument.");
            goto fail;
        }
    } else {
        o->channels = 0; /* The actual default value is the number of physical
                            input ports in jack (unknown at the moment), or if
                            that's zero, then the default_sample_spec.channels
                            of the core. */
        o->channels_given = 0;
    }

    if (pa_modargs_get_value(ma, "connect", NULL)) {
        o->connect_given = 1;
        if (pa_modargs_get_value_boolean(ma, "connect", &o->connect) < 0) {
            pa_log_error("Failed to parse the \"connect\" argument.");
            goto fail;
        }
    } else {
        o->connect = 1;
        o->connect_given = 0;
    }

    if (pa_modargs_get_value(ma, "buffersize", NULL)) {
        o->buffersize_given = 1;
        if (pa_modargs_get_value_u32(ma, "buffersize", &o->buffersize) < 0) {
            pa_log_error("Failed to parse the \"buffersize\" argument.");
            goto fail;
        }
    } else {
        o->buffersize = DEFAULT_RINGBUFFER_SIZE;
        o->buffersize_given = 0;
    }

    if (pa_modargs_get_value(ma, "channel_map", NULL)) {
        o->map_given = 1;
        if (pa_modargs_get_channel_map(ma, &o->map) < 0) {
            pa_log_error("Failed to parse the \"channel_map\" argument.");
            goto fail;
        }

        /* channel_map specifies the channel count too. */
        if (o->channels_given && (o->channels != o->map.channels)) {
            pa_log_error(
                "\"channels\" and \"channel_map\" arguments conficted. If you "
                "use the \"channel_map\" argument, you can omit the "
                "\"channels\" argument.");
            goto fail;
        } else {
            o->channels = o->map.channels;
            o->channels_given = 1;
        }
    } else {
        /* The actual default value is the default alsa mappings, but that
           can't be set until the channel count is known. Here we initialize
           the map to some valid value, although the value won't be used. */
        pa_channel_map_init_stereo(&o->map);
        o->map_given = 0;
    }

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
      pa_modargs_free(ma);

    return -1;
}


static void set_default_channels(pa_module* self, struct options* o) {
    struct userdata* u;
    const char **ports, **p;
    
    assert(self);
    assert(o);
    assert(self->userdata);

    u = self->userdata;
    
    assert(u->j_client);
    assert(self->core);
    
    o->channels = 0;
    
    ports = jack_get_ports(u->j_client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                           JackPortIsPhysical|JackPortIsInput);
    
    for (p = ports; *p; p++)
        o->channels++;
    
    free(ports);
    
    if (o->channels >= PA_CHANNELS_MAX)
        o->channels = PA_CHANNELS_MAX - 1;
    
    if (o->channels == 0)
        o->channels = self->core->default_sample_spec.channels;
}


static int create_sink(pa_module* self, struct options* o) {
    struct userdata* u;
    pa_sample_spec ss;
    char *t;
    
    assert(self);
    assert(o);
    assert(self->userdata);

    u = self->userdata;
    
    assert(u->j_client);
    
    ss.channels = u->channels;
    ss.rate = jack_get_sample_rate(u->j_client);
    ss.format = PA_SAMPLE_FLOAT32NE;
    assert(pa_sample_spec_valid(&ss));

    if (!(u->sink = pa_sink_new(self->core, __FILE__, o->sink_name, 0, &ss,
                                &o->map))) {
        pa_log("failed to create sink.");
        return -1;
    }
    
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, self);
    
    pa_sink_set_description(
        u->sink,
        t = pa_sprintf_malloc("Jack sink (%s)",
                              jack_get_client_name(u->j_client)));
    pa_xfree(t);
    
    u->sink->get_latency = sink_get_latency_cb;
    
    return 0;
}


static void connect_ports(pa_module* self) {
    struct userdata* u;
    unsigned i;
    const char **ports, **p;
    
    assert(self);
    assert(self->userdata);

    u = self->userdata;
    
    assert(u->j_client);
    
    ports = jack_get_ports(u->j_client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                           JackPortIsPhysical|JackPortIsInput);
    
    for (i = 0, p = ports; i < u->channels; i++, p++) {
        assert(u->j_ports[i]);
        
        if (!*p) {
            pa_log("Not enough physical output ports, leaving unconnected.");
            break;
        }
        
        pa_log_info("connecting %s to %s",
                    jack_port_name(u->j_ports[i]), *p);
        
        if (jack_connect(u->j_client, jack_port_name(u->j_ports[i]), *p)) {
            pa_log("Failed to connect %s to %s, leaving unconnected.",
                   jack_port_name(u->j_ports[i]), *p);
            break;
        }
    }
    
    free(ports);
}


static int start_filling_ringbuffer(pa_module* self) {
    struct userdata* u;
    pthread_attr_t thread_attributes;

    assert(self);
    assert(self->userdata);

    u = self->userdata;
    
    pthread_attr_init(&thread_attributes);
    
    if (pthread_attr_setinheritsched(&thread_attributes,
                                     PTHREAD_INHERIT_SCHED) != 0) {
        pa_log("pthread_attr_setinheritsched() failed.");
        goto fail;
    }
    else if (pthread_create(&u->filler_thread, &thread_attributes,
                            fill_ringbuffer, u) != 0) {
        pa_log("pthread_create() failed.");
        goto fail;
    }
    
    u->filler_thread_is_running = 1;
    
    pthread_attr_destroy(&thread_attributes);

    return 0;
    
fail:
    pthread_attr_destroy(&thread_attributes);
    return -1;
}


static void jack_error_func(const char* t) {
    pa_log_warn("JACK error >%s<", t);
}


static pa_usec_t sink_get_latency_cb(pa_sink* s) {
    /* The latency is approximately the sum of the first port's latency,
       buffersize of jack and the ringbuffer size. Maybe instead of using just
       the first port, the max of all ports' latencies should be used? */
    struct userdata* u;
    jack_nframes_t l;
    
    assert(s);
    assert(s->userdata);

    u = s->userdata;
    
    l = jack_port_get_total_latency(u->j_client, u->j_ports[0]) +
        u->j_buffersize + u->ringbuffer->size / u->frame_size;
    
    return pa_bytes_to_usec(l * u->frame_size, &s->sample_spec);
}


static int jack_process(jack_nframes_t nframes, void* arg) {
    struct userdata* u = arg;
    float* j_buffers[PA_CHANNELS_MAX];
    unsigned nsamples = u->channels * nframes;
    unsigned sample_idx_part1, sample_idx_part2;
    jack_nframes_t frame_idx;
    jack_ringbuffer_data_t data[2]; /* In case the readable area in the
                                       ringbuffer is non-continuous, the data
                                       will be split in two parts. */
    unsigned chan;
    unsigned samples_left_over;
    
    for (chan = 0; chan < u->channels; chan++) {
        j_buffers[chan] = jack_port_get_buffer(u->j_ports[chan], nframes);
    }
    
    jack_ringbuffer_get_read_vector(u->ringbuffer, data);
    
    /* We assume that the possible discontinuity doesn't happen in the middle
     * of a sample. Should be a safe assumption. */
    assert(((data[0].len % sizeof(float)) == 0) ||
           (data[1].len == 0));
    
    /* Copy from the first part of data until enough samples are copied or the
       first part ends. */
    sample_idx_part1 = 0;
    chan = 0;
    frame_idx = 0;
    while (sample_idx_part1 < nsamples &&
           ((sample_idx_part1 + 1) * sizeof(float)) <= data[0].len) {
        float *s = ((float*) data[0].buf) + sample_idx_part1;
        float *d = j_buffers[chan] + frame_idx;
        *d = *s;

        sample_idx_part1++;
        chan = (chan + 1) % u->channels;
        frame_idx = sample_idx_part1 / u->channels;
    }
    
    samples_left_over = nsamples - sample_idx_part1;
    
    /* Copy from the second part of data until enough samples are copied or the
       second part ends. */
    sample_idx_part2 = 0;
    while (sample_idx_part2 < samples_left_over &&
           ((sample_idx_part2 + 1) * sizeof(float)) <= data[1].len) {
        float *s = ((float*) data[1].buf) + sample_idx_part2;
        float *d = j_buffers[chan] + frame_idx;
        *d = *s;

        sample_idx_part2++;
        chan = (chan + 1) % u->channels;
        frame_idx = (sample_idx_part1 + sample_idx_part2) / u->channels;
    }
    
    samples_left_over -= sample_idx_part2;
    
    /* If there's still samples left, fill the buffers with zeros. */
    while (samples_left_over > 0) {
        float *d = j_buffers[chan] + frame_idx;
        *d = 0.0;

        samples_left_over--;
        chan = (chan + 1) % u->channels;
        frame_idx = (nsamples - samples_left_over) / u->channels;
    }
    
    jack_ringbuffer_read_advance(
        u->ringbuffer, (sample_idx_part1 + sample_idx_part2) * sizeof(float));
    
    /* Tell the rendering part that there is room in the ringbuffer. */
    u->ringbuffer_is_full = 0;
    pthread_cond_signal(&u->ringbuffer_cond);
    
    return 0;
}


static int jack_blocksize_cb(jack_nframes_t nframes, void* arg) {
    /* This gets called in the processing thread, so do we have to be realtime
       safe? No, we can do whatever we want. User gets silence while we do it.
       
       In addition to just updating the j_buffersize field in userdata, we have
       to create a new ringbuffer, if the new buffer size is bigger or equal to
       the old ringbuffer size. */
    struct userdata* u = arg;
    
    assert(u);
    
    /* We don't want to change the blocksize and the ringbuffer while rendering
       is going on. */
    pthread_mutex_lock(&u->buffersize_mutex);
    
    u->j_buffersize = nframes;
    
    if ((u->ringbuffer->size / u->frame_size) <= nframes) {
        /* We have to create a new ringbuffer. What are we going to do with the
           old data in the old buffer? We throw it away, because we're lazy
           coders. The listening experience is likely to get ruined anyway
           during the blocksize change. */
        jack_ringbuffer_free(u->ringbuffer);
        
        /* The actual ringbuffer size will be rounded up to the nearest power
           of two. */
        if (!(u->ringbuffer =
                  jack_ringbuffer_create((nframes + 1) * u->frame_size))) {
            pa_log_error(
                "jack_ringbuffer_create() failed while changing jack's buffer "
                "size, module exiting.");
            jack_client_close(u->j_client);
            u->quit_requested = 1;
        }
        assert((u->ringbuffer->size % sizeof(float)) == 0);
        pa_log_info("buffersize is %u frames (%u samples, %u bytes).",
                    u->ringbuffer->size / u->frame_size,
                    u->ringbuffer->size / sizeof(float),
                    u->ringbuffer->size);
    }
    
    pthread_mutex_unlock(&u->buffersize_mutex);
    
    return 0;
}


static void jack_shutdown(void* arg) {
    struct userdata* u = arg;
    assert(u);

    u->quit_requested = 1;
    request_render(u);
}


static void io_event_cb(pa_mainloop_api* m, pa_io_event* e, int fd,
                        pa_io_event_flags_t flags, void* userdata) {
    pa_module* self = userdata;
    struct userdata* u;
    char x;
    jack_ringbuffer_data_t buffer[2]; /* The write area in the ringbuffer may
                                         be split in two parts. */
    pa_memchunk chunk; /* This is the data source. */
    unsigned part1_length, part2_length;
    unsigned sample_idx_part1, sample_idx_part2;
    unsigned chan;
    unsigned frame_size;
    int rem;
    
    assert(m);
    assert(e);
    assert(flags == PA_IO_EVENT_INPUT);
    assert(self);
    assert(self->userdata);

    u = self->userdata;
    
    assert(u->pipe_fds[0] == fd);

    pa_read(fd, &x, 1, &u->pipe_fd_type);

    if (u->quit_requested) {
        pa_module_unload_request(self);
        return;
    }

    frame_size = u->frame_size;
    
    /* No blocksize changes during rendering, please. */
    pthread_mutex_lock(&u->buffersize_mutex);
    
    jack_ringbuffer_get_write_vector(u->ringbuffer, buffer);
    assert(((buffer[0].len % sizeof(float)) == 0) || (buffer[1].len == 0));
    
    part1_length = buffer[0].len / sizeof(float);
    part2_length = buffer[1].len / sizeof(float);

    /* If the amount of free space is not a multiple of the frame size, we have
       to adjust the lengths in order to not get confused with which sample is
       which channel. */
    if ((rem = (part1_length + part2_length) % u->channels) != 0) {
        if (part2_length >= rem) {
            part2_length -= rem;
        } else {
            part1_length -= rem - part2_length;
            part2_length = 0;
        }
    }
    
    /* pa_sink_render_full doesn't accept zero length, so we have do the
       copying only if there's data to copy, which actually makes a kind of
       sense. */
    if (part1_length > 0 || part2_length > 0) {
        pa_sink_render_full(u->sink,
                            (part1_length + part2_length) * sizeof(float),
                            &chunk);
        
        /* Write to the first part of the buffer. */
        for (sample_idx_part1 = 0;
             sample_idx_part1 < part1_length;
             sample_idx_part1++) {
            float *s =
                ((float*) ((uint8_t*) chunk.memblock->data + chunk.index)) +
                sample_idx_part1;
            float *d = ((float*) buffer[0].buf) + sample_idx_part1;
            *d = *s;
        }
        
        /* Write to the second part of the buffer. */
        for (sample_idx_part2 = 0;
             sample_idx_part2 < part2_length;
             sample_idx_part2++) {
            float *s =
                ((float*) ((uint8_t*) chunk.memblock->data + chunk.index)) +
                sample_idx_part1 + sample_idx_part2;
            float *d = ((float*) buffer[1].buf) + sample_idx_part2;
            *d = *s;
        }
        
        pa_memblock_unref(chunk.memblock);
        
        jack_ringbuffer_write_advance(
            u->ringbuffer, (part1_length + part2_length) * sizeof(float));
    }
    
    /* Blocksize can be changed again. */
    pthread_mutex_unlock(&u->buffersize_mutex);
}


static void* fill_ringbuffer(void* arg) {
    struct userdata* u = arg;
    
    assert(u);
    
    while (!u->quit_requested) {
        if (u->ringbuffer_is_full) {
            pthread_mutex_lock(&u->cond_mutex);
            pthread_cond_wait(&u->ringbuffer_cond,
                              &u->cond_mutex);
            pthread_mutex_unlock(&u->cond_mutex);
        }
        /* No, it's not full yet, but this must be set to one as soon as
           possible, because if the jack thread manages to process another
           block before we set this to one, we may end up waiting without
           a reason. */
        u->ringbuffer_is_full = 1;

        request_render(u);
    }
    
    return NULL;
}


static void request_render(struct userdata* u) {
    char c = 'x';
    
    assert(u);
    
    assert(u->pipe_fds[1] >= 0);
    pa_write(u->pipe_fds[1], &c, 1, &u->pipe_fd_type);
}

void pa__done(pa_core* c, pa_module* self) {
    struct userdata* u;
    
    assert(c);
    assert(self);

    if (!self->userdata)
        return;

    u = self->userdata;
    
    if (u->filler_thread_is_running) {
        u->quit_requested = 1;
        pthread_cond_signal(&u->ringbuffer_cond);
        pthread_join(u->filler_thread, NULL);
    }
    
    if (u->j_client)
        jack_client_close(u->j_client);

    if (u->io_event)
        c->mainloop->io_free(u->io_event);

    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
    }
    
    if (u->ringbuffer)
        jack_ringbuffer_free(u->ringbuffer);

    if (u->pipe_fds[0] >= 0)
        pa_close(u->pipe_fds[0]);
    if (u->pipe_fds[1] >= 0)
        pa_close(u->pipe_fds[1]);
    
    pthread_mutex_destroy(&u->buffersize_mutex);
    pthread_cond_destroy(&u->ringbuffer_cond);
    pthread_mutex_destroy(&u->cond_mutex);
    pa_xfree(self->userdata);
    self->userdata = NULL;
}
