#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "polyplib-simple.h"
#include "polyplib.h"
#include "mainloop.h"
#include "native-common.h"
/*#include "polyp-error.h"*/

struct pa_simple {
    struct pa_mainloop *mainloop;
    struct pa_context *context;
    struct pa_stream *stream;
    enum pa_stream_direction direction;

    int dead, drained;

    void *read_data;
    size_t read_index, read_length;
};

static void read_callback(struct pa_stream *s, const void*data, size_t length, void *userdata);

static int check_error(struct pa_simple *p, int *perror) {
    assert(p);
    
    if (pa_context_is_dead(p->context) || (p->stream && pa_stream_is_dead(p->stream))) {
        if (perror)
            *perror = pa_context_errno(p->context);
        return -1;
    }

    return 0;
}

static int iterate(struct pa_simple *p, int block, int *perror) {
    assert(p && p->context && p->mainloop);

    if (check_error(p, perror) < 0)
        return -1;
    
    if (!block && !pa_context_is_pending(p->context))
        return 0;

    do {
        if (pa_mainloop_iterate(p->mainloop, 1, NULL) < 0) {
            if (perror)
                *perror = PA_ERROR_INTERNAL;
            return -1;
        }

        if (check_error(p, perror) < 0)
            return -1;
        
    } while (pa_context_is_pending(p->context));

    return 0;
}

struct pa_simple* pa_simple_new(
    const char *server,
    const char *name,
    enum pa_stream_direction dir,
    const char *dev,
    const char *stream_name,
    const struct pa_sample_spec *ss,
    const struct pa_buffer_attr *attr,
    int *perror) {
    
    struct pa_simple *p;
    int error = PA_ERROR_INTERNAL;
    assert(ss);

    p = malloc(sizeof(struct pa_simple));
    assert(p);
    p->context = NULL;
    p->stream = NULL;
    p->mainloop = pa_mainloop_new();
    assert(p->mainloop);
    p->dead = 0;
    p->direction = dir;
    p->read_data = NULL;
    p->read_index = p->read_length = 0;

    if (!(p->context = pa_context_new(pa_mainloop_get_api(p->mainloop), name)))
        goto fail;

    if (pa_context_connect(p->context, server, NULL, NULL) < 0) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    /* Wait until the context is ready */
    while (!pa_context_is_ready(p->context)) {
        if (iterate(p, 1, &error) < 0)
            goto fail;
    }

    if (!(p->stream = pa_stream_new(p->context, dir, dev, stream_name, ss, attr, NULL, NULL)))
        goto fail;

    /* Wait until the stream is ready */
    while (!pa_stream_is_ready(p->stream)) {
        if (iterate(p, 1, &error) < 0)
            goto fail;
    }

    pa_stream_set_read_callback(p->stream, read_callback, p);
    
    return p;
    
fail:
    if (perror)
        *perror = error;
    pa_simple_free(p);
    return NULL;
}

void pa_simple_free(struct pa_simple *s) {
    assert(s);

    free(s->read_data);

    if (s->stream)
        pa_stream_free(s->stream);
    
    if (s->context)
        pa_context_free(s->context);

    if (s->mainloop)
        pa_mainloop_free(s->mainloop);

    free(s);
}

int pa_simple_write(struct pa_simple *p, const void*data, size_t length, int *perror) {
    assert(p && data && p->direction == PA_STREAM_PLAYBACK);

    while (length > 0) {
        size_t l;
        
        while (!(l = pa_stream_writable_size(p->stream)))
            if (iterate(p, 1, perror) < 0)
                return -1;

        if (l > length)
            l = length;

        pa_stream_write(p->stream, data, l);
        data += l;
        length -= l;
    }

    /* Make sure that no data is pending for write */
    if (iterate(p, 0, perror) < 0)
        return -1;

    return 0;
}

static void read_callback(struct pa_stream *s, const void*data, size_t length, void *userdata) {
    struct pa_simple *p = userdata;
    assert(s && data && length && p);

    if (p->read_data) {
        fprintf(stderr, __FILE__": Buffer overflow, dropping incoming memory blocks.\n");
        free(p->read_data);
    }

    p->read_data = malloc(p->read_length = length);
    assert(p->read_data);
    memcpy(p->read_data, data, length);
    p->read_index = 0;
}

int pa_simple_read(struct pa_simple *p, void*data, size_t length, int *perror) {
    assert(p && data && p->direction == PA_STREAM_RECORD);

    while (length > 0) {
        if (p->read_data) {
            size_t l = length;

            if (p->read_length <= l)
                l = p->read_length;

            memcpy(data, p->read_data+p->read_index, l);

            data += l;
            length -= l;
            
            p->read_index += l;
            p->read_length -= l;

            if (!p->read_length) {
                free(p->read_data);
                p->read_data = NULL;
                p->read_index = 0;
            }
            
            if (!length)
                return 0;

            assert(!p->read_data);
        }

        if (iterate(p, 1, perror) < 0)
            return -1;
    }

    return 0;
}

static void drain_complete(struct pa_stream *s, void *userdata) {
    struct pa_simple *p = userdata;
    assert(s && p);
    p->drained = 1;
}

int pa_simple_drain(struct pa_simple *p, int *perror) {
    assert(p && p->direction == PA_STREAM_PLAYBACK);
    p->drained = 0;
    pa_stream_drain(p->stream, drain_complete, p);

    while (!p->drained) {
        if (iterate(p, 1, perror) < 0) {
            pa_stream_drain(p->stream, NULL, NULL);
            return -1;
        }
    }

    return 0;
}
