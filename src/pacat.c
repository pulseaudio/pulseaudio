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

#include "polyplib.h"
#include "polyplib-error.h"
#include "mainloop.h"
#include "mainloop-signal.h"

static enum { RECORD, PLAYBACK } mode = PLAYBACK;

static struct pa_context *context = NULL;
static struct pa_stream *stream = NULL;
static struct pa_mainloop_api *mainloop_api = NULL;

static void *buffer = NULL;
static size_t buffer_length = 0, buffer_index = 0;

static void* stdio_source = NULL;

static void quit(int ret) {
    assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}

static void context_die_callback(struct pa_context *c, void *userdata) {
    assert(c);
    fprintf(stderr, "Connection to server shut down, exiting.\n");
    quit(1);
}

static void stream_die_callback(struct pa_stream *s, void *userdata) {
    assert(s);
    fprintf(stderr, "Stream deleted, exiting.\n");
    quit(1);
}

static void do_stream_write(size_t length) {
    size_t l;
    assert(length);

    if (!buffer || !buffer_length)
        return;
    
    l = length;
    if (l > buffer_length)
        l = buffer_length;
    
    pa_stream_write(stream, buffer+buffer_index, l);
    buffer_length -= l;
    buffer_index += l;
    
    if (!buffer_length) {
        free(buffer);
        buffer = NULL;
        buffer_index = buffer_length = 0;
    }
}

static void stream_write_callback(struct pa_stream *s, size_t length, void *userdata) {
    assert(s && length);

    if (stdio_source)
        mainloop_api->enable_io(mainloop_api, stdio_source, PA_MAINLOOP_API_IO_EVENT_INPUT);

    if (!buffer)
        return;

    do_stream_write(length);
}

static void stream_read_callback(struct pa_stream *s, const void*data, size_t length, void *userdata) {
    assert(s && data && length);

    if (stdio_source)
        mainloop_api->enable_io(mainloop_api, stdio_source, PA_MAINLOOP_API_IO_EVENT_OUTPUT);

    if (buffer) {
        fprintf(stderr, "Buffer overrrun, dropping incoming data\n");
        return;
    }

    buffer = malloc(buffer_length = length);
    assert(buffer);
    memcpy(buffer, data, length);
    buffer_index = 0;
}

static void stream_complete_callback(struct pa_stream*s, int success, void *userdata) {
    assert(s);

    if (!success) {
        fprintf(stderr, "Stream creation failed: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        quit(1);
        return;
    }

    fprintf(stderr, "Stream created.\n");
}

static void context_complete_callback(struct pa_context *c, int success, void *userdata) {
    static const struct pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 44100,
        .channels = 2
    };
        
    assert(c && !stream);

    if (!success) {
        fprintf(stderr, "Connection failed: %s\n", pa_strerror(pa_context_errno(c)));
        goto fail;
    }

    fprintf(stderr, "Connection established.\n");
    
    if (!(stream = pa_stream_new(c, mode == PLAYBACK ? PA_STREAM_PLAYBACK : PA_STREAM_RECORD, NULL, "pacat", &ss, NULL, stream_complete_callback, NULL))) {
        fprintf(stderr, "pa_stream_new() failed: %s\n", pa_strerror(pa_context_errno(c)));
        goto fail;
    }

    pa_stream_set_die_callback(stream, stream_die_callback, NULL);
    pa_stream_set_write_callback(stream, stream_write_callback, NULL);
    pa_stream_set_read_callback(stream, stream_read_callback, NULL);
    
    return;
    
fail:
    quit(1);
}

static void context_drain_complete(struct pa_context*c, void *userdata) {
    quit(0);
}

static void stream_drain_complete(struct pa_stream*s, void *userdata) {
    fprintf(stderr, "Playback stream drained.\n");

    pa_stream_free(stream);
    stream = NULL;
    
    if (pa_context_drain(context, context_drain_complete, NULL) < 0)
        quit(0);
    else
        fprintf(stderr, "Draining connection to server.\n");
}

static void stdin_callback(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    size_t l, w = 0;
    ssize_t r;
    assert(a == mainloop_api && id && stdio_source == id);

    if (buffer) {
        mainloop_api->enable_io(mainloop_api, stdio_source, PA_MAINLOOP_API_IO_EVENT_NULL);
        return;
    }

    if (!stream || !pa_stream_is_ready(stream) || !(l = w = pa_stream_writable_size(stream)))
        l = 4096;
    
    buffer = malloc(l);
    assert(buffer);
    if ((r = read(fd, buffer, l)) <= 0) {
        if (r == 0) {
            fprintf(stderr, "Got EOF.\n");
            pa_stream_drain(stream, stream_drain_complete, NULL);
        } else {
            fprintf(stderr, "read() failed: %s\n", strerror(errno));
            quit(1);
        }

        mainloop_api->cancel_io(mainloop_api, stdio_source);
        stdio_source = NULL;
        return;
    }

    buffer_length = r;
    buffer_index = 0;

    if (w)
        do_stream_write(w);
}

static void stdout_callback(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    ssize_t r;
    assert(a == mainloop_api && id && stdio_source == id);

    if (!buffer) {
        mainloop_api->enable_io(mainloop_api, stdio_source, PA_MAINLOOP_API_IO_EVENT_NULL);
        return;
    }

    assert(buffer_length);
    
    if ((r = write(fd, buffer+buffer_index, buffer_length)) <= 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        quit(1);

        mainloop_api->cancel_io(mainloop_api, stdio_source);
        stdio_source = NULL;
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

static void exit_signal_callback(void *id, int sig, void *userdata) {
    fprintf(stderr, "Got SIGINT, exiting.\n");
    quit(0);
    
}

static void stream_get_latency_callback(struct pa_stream *s, uint32_t latency, void *userdata) {
    assert(s);

    if (latency == (uint32_t) -1) {
        fprintf(stderr, "Failed to get latency: %s\n", strerror(errno));
        quit(1);
        return;
    }

    fprintf(stderr, "Current latency is %u usecs.\n", latency);
}

static void sigusr1_signal_callback(void *id, int sig, void *userdata) {
    if (mode == PLAYBACK) {
        fprintf(stderr, "Got SIGUSR1, requesting latency.\n");
        pa_stream_get_latency(stream, stream_get_latency_callback, NULL);
    }
}

int main(int argc, char *argv[]) {
    struct pa_mainloop* m = NULL;
    int ret = 1, r;
    char *bn;

    if (!(bn = strrchr(argv[0], '/')))
        bn = argv[0];

    if (strstr(bn, "rec") || strstr(bn, "mon"))
        mode = RECORD;
    else if (strstr(bn, "cat") || strstr(bn, "play"))
        mode = PLAYBACK;

    fprintf(stderr, "Opening a %s stream.\n", mode == RECORD ? "recording" : "playback");
    
    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    r = pa_signal_init(mainloop_api);
    assert(r == 0);
    pa_signal_register(SIGINT, exit_signal_callback, NULL);
    pa_signal_register(SIGUSR1, sigusr1_signal_callback, NULL);
    signal(SIGPIPE, SIG_IGN);
    
    if (!(stdio_source = mainloop_api->source_io(mainloop_api,
                                                 mode == PLAYBACK ? STDIN_FILENO : STDOUT_FILENO,
                                                 mode == PLAYBACK ? PA_MAINLOOP_API_IO_EVENT_INPUT : PA_MAINLOOP_API_IO_EVENT_OUTPUT,
                                                 mode == PLAYBACK ? stdin_callback : stdout_callback, NULL))) {
        fprintf(stderr, "source_io() failed.\n");
        goto quit;
    }
    
    if (!(context = pa_context_new(mainloop_api, argv[0]))) {
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    if (pa_context_connect(context, NULL, context_complete_callback, NULL) < 0) {
        fprintf(stderr, "pa_context_connext() failed.\n");
        goto quit;
    }
        
    pa_context_set_die_callback(context, context_die_callback, NULL);

    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, "pa_mainloop_run() failed.\n");
        goto quit;
    }
    
quit:
    if (stream)
        pa_stream_free(stream);
    if (context)
        pa_context_free(context);

    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }
    
    if (buffer)
        free(buffer);
    
    return ret;
}
