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

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <polyp/polyplib.h>
#include <polyp/polyplib-error.h>
#include <polyp/mainloop.h>
#include <polyp/mainloop-signal.h>

static enum { RECORD, PLAYBACK } mode = PLAYBACK;

static struct pa_context *context = NULL;
static struct pa_stream *stream = NULL;
static struct pa_mainloop_api *mainloop_api = NULL;

static void *buffer = NULL;
static size_t buffer_length = 0, buffer_index = 0;

static struct pa_io_event* stdio_event = NULL;

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
    
    pa_stream_write(stream, (uint8_t*) buffer + buffer_index, l, NULL, 0);
    buffer_length -= l;
    buffer_index += l;
    
    if (!buffer_length) {
        free(buffer);
        buffer = NULL;
        buffer_index = buffer_length = 0;
    }
}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(struct pa_stream *s, size_t length, void *userdata) {
    assert(s && length);

    if (stdio_event)
        mainloop_api->io_enable(stdio_event, PA_IO_EVENT_INPUT);

    if (!buffer)
        return;

    do_stream_write(length);
}

/* This is called whenever new data may is available */
static void stream_read_callback(struct pa_stream *s, const void*data, size_t length, void *userdata) {
    assert(s && data && length);

    if (stdio_event)
        mainloop_api->io_enable(stdio_event, PA_IO_EVENT_OUTPUT);

    if (buffer) {
        fprintf(stderr, "Buffer overrrun, dropping incoming data\n");
        return;
    }

    buffer = malloc(buffer_length = length);
    assert(buffer);
    memcpy(buffer, data, length);
    buffer_index = 0;
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(struct pa_stream *s, void *userdata) {
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_TERMINATED:
            break;

        case PA_STREAM_READY:
            fprintf(stderr, "Stream successfully created\n");
            break;
            
        case PA_STREAM_FAILED:
        default:
            fprintf(stderr, "Stream errror: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(1);
    }
}

/* This is called whenever the context status changes */
static void context_state_callback(struct pa_context *c, void *userdata) {
    static const struct pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 44100,
        .channels = 2
    };

    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
        
        case PA_CONTEXT_READY:
            
            assert(c && !stream);
            fprintf(stderr, "Connection established.\n");

            stream = pa_stream_new(c, "pacat", &ss);
            assert(stream);

            pa_stream_set_state_callback(stream, stream_state_callback, NULL);
            pa_stream_set_write_callback(stream, stream_write_callback, NULL);
            pa_stream_set_read_callback(stream, stream_read_callback, NULL);

            if (mode == PLAYBACK)
                pa_stream_connect_playback(stream, NULL, NULL, PA_VOLUME_NORM);
            else
                pa_stream_connect_record(stream, NULL, NULL);
                
            break;
            
        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, "Connection failure: %s\n", pa_strerror(pa_context_errno(c)));
            quit(1);
    }
}

/* Connection draining complete */
static void context_drain_complete(struct pa_context*c, void *userdata) {
    pa_context_disconnect(c);
}

/* Stream draining complete */
static void stream_drain_complete(struct pa_stream*s, int success, void *userdata) {
    struct pa_operation *o;

    if (!success) {
        fprintf(stderr, "Failed to drain stream: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
    }
        
    fprintf(stderr, "Playback stream drained.\n");

    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
    stream = NULL;
    
    if (!(o = pa_context_drain(context, context_drain_complete, NULL)))
        pa_context_disconnect(context);
    else {
        pa_operation_unref(o);
        fprintf(stderr, "Draining connection to server.\n");
    }
}

/* New data on STDIN **/
static void stdin_callback(struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
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
            fprintf(stderr, "Got EOF.\n");
            pa_operation_unref(pa_stream_drain(stream, stream_drain_complete, NULL));
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
static void stdout_callback(struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
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
static void exit_signal_callback(struct pa_mainloop_api*m, struct pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, "Got SIGINT, exiting.\n");
    quit(0);
    
}

/* Show the current latency */
static void stream_get_latency_callback(struct pa_stream *s, const struct pa_latency_info *i, void *userdata) {
    double total;
    assert(s);

    if (!i) {
        fprintf(stderr, "Failed to get latency: %s\n", pa_strerror(pa_context_errno(context)));
        quit(1);
        return;
    }

    if (mode == PLAYBACK)
        total = (double) i->sink_usec + i->buffer_usec + i->transport_usec;
    else
        total = (double) i->source_usec + i->buffer_usec + i->transport_usec - i->sink_usec;

    fprintf(stderr, "Latency: buffer: %0.0f usec; sink: %0.0f usec; source: %0.0f usec; transport: %0.0f usec; total: %0.0f usec; synchronized clocks: %s.\n",
            (float) i->buffer_usec, (float) i->sink_usec, (float) i->source_usec, (float) i->transport_usec, total,
            i->synchronized_clocks ? "yes" : "no");
}

/* Someone requested that the latency is shown */
static void sigusr1_signal_callback(struct pa_mainloop_api*m, struct pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, "Got SIGUSR1, requesting latency.\n");
    pa_operation_unref(pa_stream_get_latency(stream, stream_get_latency_callback, NULL));
}

int main(int argc, char *argv[]) {
    struct pa_mainloop* m = NULL;
    int ret = 1, r;
    char *bn;

    if (!(bn = strrchr(argv[0], '/')))
        bn = argv[0];
    else
        bn++;

    if (strstr(bn, "rec") || strstr(bn, "mon"))
        mode = RECORD;
    else if (strstr(bn, "cat") || strstr(bn, "play"))
        mode = PLAYBACK;

    if (argc >= 2) {
        if (!strcmp(argv[1], "-r"))
            mode = RECORD;
        else if (!strcmp(argv[1], "-p"))
            mode = PLAYBACK;
        else {
            fprintf(stderr, "Invalid argument\n");
            goto quit;
        }
    }

    fprintf(stderr, "Opening a %s stream.\n", mode == RECORD ? "recording" : "playback");

    /* Set up a new main loop */
    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    r = pa_signal_init(mainloop_api);
    assert(r == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
    pa_signal_new(SIGUSR1, sigusr1_signal_callback, NULL);
    signal(SIGPIPE, SIG_IGN);
    
    if (!(stdio_event = mainloop_api->io_new(mainloop_api,
                                             mode == PLAYBACK ? STDIN_FILENO : STDOUT_FILENO,
                                             mode == PLAYBACK ? PA_IO_EVENT_INPUT : PA_IO_EVENT_OUTPUT,
                                             mode == PLAYBACK ? stdin_callback : stdout_callback, NULL))) {
        fprintf(stderr, "source_io() failed.\n");
        goto quit;
    }

    /* Create a new connection context */
    if (!(context = pa_context_new(mainloop_api, bn))) {
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);

    /* Connect the context */
    pa_context_connect(context, NULL, 1, NULL);

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

    if (buffer)
        free(buffer);
    
    return ret;
}
