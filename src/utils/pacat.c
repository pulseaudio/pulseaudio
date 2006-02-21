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

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include <polyp/polypaudio.h>
#include <polyp/mainloop.h>
#include <polyp/mainloop-signal.h>

#if PA_API_VERSION != 8
#error Invalid Polypaudio API version
#endif

static enum { RECORD, PLAYBACK } mode = PLAYBACK;

static pa_context *context = NULL;
static pa_stream *stream = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static void *buffer = NULL;
static size_t buffer_length = 0, buffer_index = 0;

static pa_io_event* stdio_event = NULL;

static char *stream_name = NULL, *client_name = NULL, *device = NULL;

static int verbose = 0;
static pa_volume_t volume = PA_VOLUME_NORM;

static pa_sample_spec sample_spec = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2
};

/* A shortcut for terminating the application */
static void quit(int ret) {
    assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}

/* Write some data to the stream */
static void do_stream_write(size_t length) {
    size_t l;
    assert(length);

    if (!buffer || !buffer_length)
        return;
    
    l = length;
    if (l > buffer_length)
        l = buffer_length;
    
    if (pa_stream_write(stream, (uint8_t*) buffer + buffer_index, l, NULL, 0, PA_SEEK_RELATIVE) < 0) {
        fprintf(stderr, "pa_stream_write() failed: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
        return;
    }
    
    buffer_length -= l;
    buffer_index += l;
    
    if (!buffer_length) {
        free(buffer);
        buffer = NULL;
        buffer_index = buffer_length = 0;
    }
}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    assert(s && length);

    if (stdio_event)
        mainloop_api->io_enable(stdio_event, PA_IO_EVENT_INPUT);

    if (!buffer)
        return;

    do_stream_write(length);
}

/* This is called whenever new data may is available */
static void stream_read_callback(pa_stream *s, size_t length, void *userdata) {
    const void *data;
    assert(s && length);

    if (stdio_event)
        mainloop_api->io_enable(stdio_event, PA_IO_EVENT_OUTPUT);

    if (pa_stream_peek(s, &data, &length) < 0) {
        fprintf(stderr, "pa_stream_peek() failed: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
        return;
    }
    
    assert(data && length);

    if (buffer) {
        fprintf(stderr, "Buffer overrun, dropping incoming data\n");
        if (pa_stream_drop(s) < 0) {
            fprintf(stderr, "pa_stream_drop() failed: %s\n", pa_strerror(pa_context_errno(context)));
            quit(1);
        }
        return;
    }

    buffer = malloc(buffer_length = length);
    assert(buffer);
    memcpy(buffer, data, length);
    buffer_index = 0;
    pa_stream_drop(s);
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata) {
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_TERMINATED:
            break;

        case PA_STREAM_READY:
            if (verbose)
                fprintf(stderr, "Stream successfully created\n");
            break;
            
        case PA_STREAM_FAILED:
        default:
            fprintf(stderr, "Stream error: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(1);
    }
}

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
        
        case PA_CONTEXT_READY: {
            int r;
            
            assert(c && !stream);

            if (verbose)
                fprintf(stderr, "Connection established.\n");

            if (!(stream = pa_stream_new(c, stream_name, &sample_spec, NULL))) {
                fprintf(stderr, "pa_stream_new() failed: %s\n", pa_strerror(pa_context_errno(c)));
                goto fail;
            }

            pa_stream_set_state_callback(stream, stream_state_callback, NULL);
            pa_stream_set_write_callback(stream, stream_write_callback, NULL);
            pa_stream_set_read_callback(stream, stream_read_callback, NULL);

            if (mode == PLAYBACK) {
                pa_cvolume cv;
                if ((r = pa_stream_connect_playback(stream, device, NULL, 0, pa_cvolume_set(&cv, sample_spec.channels, volume), NULL)) < 0) {
                    fprintf(stderr, "pa_stream_connect_playback() failed: %s\n", pa_strerror(pa_context_errno(c)));
                    goto fail;
                }
                    
            } else {
                if ((r = pa_stream_connect_record(stream, device, NULL, 0)) < 0) {
                    fprintf(stderr, "pa_stream_connect_record() failed: %s\n", pa_strerror(pa_context_errno(c)));
                    goto fail;
                }
            }
                
            break;
        }
            
        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, "Connection failure: %s\n", pa_strerror(pa_context_errno(c)));
            goto fail;
    }

    return;
    
fail:
    quit(1);
    
}

/* Connection draining complete */
static void context_drain_complete(pa_context*c, void *userdata) {
    pa_context_disconnect(c);
}

/* Stream draining complete */
static void stream_drain_complete(pa_stream*s, int success, void *userdata) {
    pa_operation *o;

    if (!success) {
        fprintf(stderr, "Failed to drain stream: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
    }
    
    if (verbose)    
        fprintf(stderr, "Playback stream drained.\n");

    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
    stream = NULL;
    
    if (!(o = pa_context_drain(context, context_drain_complete, NULL)))
        pa_context_disconnect(context);
    else {
        if (verbose)
            fprintf(stderr, "Draining connection to server.\n");
    }
}

/* New data on STDIN **/
static void stdin_callback(pa_mainloop_api*a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata) {
    size_t l, w = 0;
    ssize_t r;
    assert(a == mainloop_api && e && stdio_event == e);

    if (buffer) {
        mainloop_api->io_enable(stdio_event, PA_IO_EVENT_NULL);
        return;
    }

    if (!stream || pa_stream_get_state(stream) != PA_STREAM_READY || !(l = w = pa_stream_writable_size(stream)))
        l = 4096;
    
    buffer = malloc(l);
    assert(buffer);
    if ((r = read(fd, buffer, l)) <= 0) {
        if (r == 0) {
            pa_operation *o;
            
            if (verbose)
                fprintf(stderr, "Got EOF.\n");
            
            if (!(o = pa_stream_drain(stream, stream_drain_complete, NULL))) {
                fprintf(stderr, "pa_stream_drain(): %s\n", pa_strerror(pa_context_errno(context)));
                quit(1);
                return;
            }

            pa_operation_unref(o);
        } else {
            fprintf(stderr, "read() failed: %s\n", strerror(errno));
            quit(1);
        }

        mainloop_api->io_free(stdio_event);
        stdio_event = NULL;
        return;
    }

    buffer_length = r;
    buffer_index = 0;

    if (w)
        do_stream_write(w);
}

/* Some data may be written to STDOUT */
static void stdout_callback(pa_mainloop_api*a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata) {
    ssize_t r;
    assert(a == mainloop_api && e && stdio_event == e);

    if (!buffer) {
        mainloop_api->io_enable(stdio_event, PA_IO_EVENT_NULL);
        return;
    }

    assert(buffer_length);
    
    if ((r = write(fd, (uint8_t*) buffer+buffer_index, buffer_length)) <= 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        quit(1);

        mainloop_api->io_free(stdio_event);
        stdio_event = NULL;
        return;
    }

    buffer_length -= r;
    buffer_index += r;

    if (!buffer_length) {
        free(buffer);
        buffer = NULL;
        buffer_length = buffer_index = 0;
    }
}

/* UNIX signal to quit recieved */
static void exit_signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata) {
    if (verbose)
        fprintf(stderr, "Got signal, exiting.\n");
    quit(0);
    
}

/* Show the current latency */
static void stream_get_latency_callback(pa_stream *s, const pa_latency_info *i, void *userdata) {
    pa_usec_t total;
    int negative = 0;
    assert(s);

    if (!i) {
        fprintf(stderr, "Failed to get latency: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
        return;
    }

    total = pa_stream_get_latency(s, i, &negative);

    fprintf(stderr, "Latency: buffer: %0.0f usec; sink: %0.0f usec; source: %0.0f usec; transport: %0.0f usec; total: %0.0f usec; synchronized clocks: %s.\n",
            (float) i->buffer_usec, (float) i->sink_usec, (float) i->source_usec, (float) i->transport_usec, (float) total * (negative?-1:1),
            i->synchronized_clocks ? "yes" : "no");
}

/* Someone requested that the latency is shown */
static void sigusr1_signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, "Got SIGUSR1, requesting latency.\n");
    pa_operation_unref(pa_stream_get_latency_info(stream, stream_get_latency_callback, NULL));
}


static void help(const char *argv0) {

    printf("%s [options]\n\n"
           "  -h, --help                            Show this help\n"
           "      --version                         Show version\n\n"
           "  -r, --record                          Create a connection for recording\n"
           "  -p, --playback                        Create a connection for playback\n\n"
           "  -v, --verbose                         Enable verbose operations\n\n"
           "  -s, --server=SERVER                   The name of the server to connect to\n"
           "  -d, --device=DEVICE                   The name of the sink/source to connect to\n"
           "  -n, --client-name=NAME                How to call this client on the server\n"
           "      --stream-name=NAME                How to call this stream on the server\n"
           "      --volume=VOLUME                   Specify the initial (linear) volume in range 0...256\n"
           "      --rate=SAMPLERATE                 The sample rate in Hz (defaults to 44100)\n"
           "      --format=SAMPLEFORMAT             The sample type, one of s16le, s16be, u8, float32le,\n"
           "                                        float32be, ulaw, alaw (defaults to s16ne)\n"
           "      --channels=CHANNELS               The number of channels, 1 for mono, 2 for stereo\n"
           "                                        (defaults to 2)\n",
           argv0);
}

enum {
    ARG_VERSION = 256,
    ARG_STREAM_NAME,
    ARG_VOLUME,
    ARG_SAMPLERATE,
    ARG_SAMPLEFORMAT,
    ARG_CHANNELS
};

int main(int argc, char *argv[]) {
    pa_mainloop* m = NULL;
    int ret = 1, r, c;
    char *bn, *server = NULL;

    static const struct option long_options[] = {
        {"record",      0, NULL, 'r'},
        {"playback",    0, NULL, 'p'},
        {"device",      1, NULL, 'd'},
        {"server",      1, NULL, 's'},
        {"client-name", 1, NULL, 'n'},
        {"stream-name", 1, NULL, ARG_STREAM_NAME},
        {"version",     0, NULL, ARG_VERSION},
        {"help",        0, NULL, 'h'},
        {"verbose",     0, NULL, 'v'},
        {"volume",      1, NULL, ARG_VOLUME},
        {"rate",        1, NULL, ARG_SAMPLERATE},
        {"format",      1, NULL, ARG_SAMPLEFORMAT},
        {"channels",    1, NULL, ARG_CHANNELS},
        {NULL,          0, NULL, 0}
    };

    if (!(bn = strrchr(argv[0], '/')))
        bn = argv[0];
    else
        bn++;

    if (strstr(bn, "rec") || strstr(bn, "mon"))
        mode = RECORD;
    else if (strstr(bn, "cat") || strstr(bn, "play"))
        mode = PLAYBACK;

    while ((c = getopt_long(argc, argv, "rpd:s:n:hv", long_options, NULL)) != -1) {

        switch (c) {
            case 'h' :
                help(bn);
                ret = 0;
                goto quit;
                
            case ARG_VERSION:
                printf("pacat "PACKAGE_VERSION"\nCompiled with libpolyp %s\nLinked with libpolyp %s\n", pa_get_headers_version(), pa_get_library_version());
                ret = 0;
                goto quit;

            case 'r':
                mode = RECORD;
                break;

            case 'p':
                mode = PLAYBACK;
                break;

            case 'd':
                free(device);
                device = strdup(optarg);
                break;

            case 's':
                free(server);
                server = strdup(optarg);
                break;

            case 'n':
                free(client_name);
                client_name = strdup(optarg);
                break;

            case ARG_STREAM_NAME:
                free(stream_name);
                stream_name = strdup(optarg);
                break;

            case 'v':
                verbose = 1;
                break;

            case ARG_VOLUME: {
                int v = atoi(optarg);
                volume = v < 0 ? 0 : v;
                break;
            }

            case ARG_CHANNELS: 
                sample_spec.channels = atoi(optarg);
                break;

            case ARG_SAMPLEFORMAT:
                sample_spec.format = pa_parse_sample_format(optarg);
                break;

            case ARG_SAMPLERATE:
                sample_spec.rate = atoi(optarg);
                break;

            default:
                goto quit;
        }
    }

    if (!client_name)
        client_name = strdup(bn);

    if (!stream_name)
        stream_name = strdup(client_name);

    if (!pa_sample_spec_valid(&sample_spec)) {
        fprintf(stderr, "Invalid sample specification\n");
        goto quit;
    }
    
    if (verbose) {
        char t[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(t, sizeof(t), &sample_spec);
        fprintf(stderr, "Opening a %s stream with sample specification '%s'.\n", mode == RECORD ? "recording" : "playback", t);
    }

    /* Set up a new main loop */
    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    r = pa_signal_init(mainloop_api);
    assert(r == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
    pa_signal_new(SIGTERM, exit_signal_callback, NULL);
#ifdef SIGUSR1
    pa_signal_new(SIGUSR1, sigusr1_signal_callback, NULL);
#endif
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    
    if (!(stdio_event = mainloop_api->io_new(mainloop_api,
                                             mode == PLAYBACK ? STDIN_FILENO : STDOUT_FILENO,
                                             mode == PLAYBACK ? PA_IO_EVENT_INPUT : PA_IO_EVENT_OUTPUT,
                                             mode == PLAYBACK ? stdin_callback : stdout_callback, NULL))) {
        fprintf(stderr, "source_io() failed.\n");
        goto quit;
    }

    /* Create a new connection context */
    if (!(context = pa_context_new(mainloop_api, client_name))) {
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);

    /* Connect the context */
    pa_context_connect(context, server, 0, NULL);

    /* Run the main loop */
    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, "pa_mainloop_run() failed.\n");
        goto quit;
    }
    
quit:
    if (stream)
        pa_stream_unref(stream);

    if (context)
        pa_context_unref(context);

    if (stdio_event) {
        assert(mainloop_api);
        mainloop_api->io_free(stdio_event);
    }
    
    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }

    free(buffer);

    free(server);
    free(device);
    free(client_name);
    free(stream_name);
    
    return ret;
}
