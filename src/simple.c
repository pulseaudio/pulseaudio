#include <assert.h>
#include <stdlib.h>

#include "simple.h"
#include "polyp.h"
#include "mainloop.h"
#include "polyp-error.h"

struct pa_simple {
    struct pa_mainloop *mainloop;
    struct pa_context *context;
    struct pa_stream *stream;

    int dead;
};

static int iterate(struct pa_simple *p, int block, int *perror) {
    assert(p && p->context && p->mainloop && perror);

    if (!block && !pa_context_is_pending(p->context))
        return 0;
    
    do {
        if (pa_context_is_dead(p->context) || (p->stream && pa_stream_is_dead(p->stream))) {
            *perror = pa_context_errno(p->context);
            return -1;
        }
        
        if (pa_mainloop_iterate(p->mainloop, 1, NULL) < 0) {
            *perror = PA_ERROR_INTERNAL;
            return -1;
        }
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

    return p;
    
fail:
    *perror = error;
    pa_simple_free(p);
    return NULL;
}

void pa_simple_free(struct pa_simple *s) {
    assert(s);

    if (s->stream)
        pa_stream_free(s->stream);
    
    if (s->context)
        pa_context_free(s->context);

    if (s->mainloop)
        pa_mainloop_free(s->mainloop);

    free(s);
}

int pa_simple_write(struct pa_simple *p, const void*data, size_t length, int *perror) {
    assert(p && data);

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

int pa_simple_read(struct pa_simple *s, void*data, size_t length, int *perror) {
    assert(0);
}

