#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "polyp.h"
#include "mainloop.h"

static struct pa_context *context = NULL;
static struct pa_stream *stream = NULL;
static struct pa_mainloop_api *mainloop_api = NULL;

static void *buffer = NULL;
static size_t buffer_length = 0, buffer_index = 0;

static void* stdin_source = NULL;

static void context_die_callback(struct pa_context *c, void *userdata) {
    assert(c);
    fprintf(stderr, "Connection to server shut down, exiting.\n");
    mainloop_api->quit(mainloop_api, 1);
}

static void stream_die_callback(struct pa_stream *s, void *userdata) {
    assert(s);
    fprintf(stderr, "Stream deleted, exiting.\n");
    mainloop_api->quit(mainloop_api, 1);
}

static void do_write(size_t length) {
    size_t l;
    assert(buffer && buffer_length);
    
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
    
    mainloop_api->enable_io(mainloop_api, stdin_source, PA_MAINLOOP_API_IO_EVENT_INPUT);

    if (!buffer)
        return;

    do_write(length);
}

static void stream_complete_callback(struct pa_context*c, struct pa_stream *s, void *userdata) {
    assert(c);

    if (!s) {
        fprintf(stderr, "Stream creation failed.\n");
        mainloop_api->quit(mainloop_api, 1);
        return;
    }

    stream = s;
    pa_stream_set_die_callback(stream, stream_die_callback, NULL);
    pa_stream_set_write_callback(stream, stream_write_callback, NULL);
}

static void context_complete_callback(struct pa_context *c, int success, void *userdata) {
    static const struct pa_sample_spec ss = {
        .format = SAMPLE_S16NE,
        .rate = 44100,
        .channels = 2
    };
        
    assert(c && !stream);

    if (!success) {
        fprintf(stderr, "Connection failed\n");
        goto fail;
    }
    
    if (pa_stream_new(c, PA_STREAM_PLAYBACK, NULL, "pacat", &ss, NULL, stream_complete_callback, NULL) < 0) {
        fprintf(stderr, "pa_stream_new() failed.\n");
        goto fail;
    }

    return;
    
fail:
    mainloop_api->quit(mainloop_api, 1);
}

static void stdin_callback(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    size_t l, w = 0;
    ssize_t r;
    assert(a == mainloop_api && id && fd == STDIN_FILENO && events == PA_MAINLOOP_API_IO_EVENT_INPUT);

    if (buffer) {
        mainloop_api->enable_io(mainloop_api, stdin_source, PA_MAINLOOP_API_IO_EVENT_NULL);
        return;
    }

    if (!stream || !(l = w = pa_stream_writable_size(stream)))
        l = 4096;
    buffer = malloc(l);
    assert(buffer);
    if ((r = read(fd, buffer, l)) <= 0) {
        if (r == 0)
            mainloop_api->quit(mainloop_api, 0);
        else {
            fprintf(stderr, "read() failed: %s\n", strerror(errno));
            mainloop_api->quit(mainloop_api, 1);
        }

        return;
    }

    buffer_length = r;
    buffer_index = 0;

    if (w)
        do_write(w);
}

int main(int argc, char *argv[]) {
    struct pa_mainloop* m;
    int ret = 1;

    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    if (!(stdin_source = mainloop_api->source_io(mainloop_api, STDIN_FILENO, PA_MAINLOOP_API_IO_EVENT_INPUT, stdin_callback, NULL))) {
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
    if (m)
        pa_mainloop_free(m);
    if (buffer)
        free(buffer);
    
    return ret;
}
