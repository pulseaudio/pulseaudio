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

    while (!pa_context_is_ready(p->context)) {
        if (pa_context_is_dead(p->context)) {
            error = pa_context_errno(p->context);
            goto fail;
        }
        
        if (pa_mainloop_iterate(p->mainloop, 1, NULL) < 0)
            goto fail;
    }

    if (!(p->stream = pa_stream_new(p->context, dir, dev, stream_name, ss, attr, NULL, NULL)))
        goto fail;

    while (!pa_stream_is_ready(p->stream)) {
        if (pa_stream_is_dead(p->stream)) {
            error = pa_context_errno(p->context);
            goto fail;
        }

        if (pa_mainloop_iterate(p->mainloop, 1, NULL) < 0)
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
        
        while (!(l = pa_stream_writable_size(p->stream))) {
            if (pa_context_is_dead(p->context)) {
                *perror = pa_context_errno(p->context);
                return -1;
            }
            
            if (pa_mainloop_iterate(p->mainloop, 1, NULL) < 0) {
                *perror = PA_ERROR_INTERNAL;
                return -1;
            }
        }

        if (l > length)
            l = length;

        pa_stream_write(p->stream, data, l);
        data += l;
        length -= l;
    }

    return 0;
}

int pa_simple_read(struct pa_simple *s, void*data, size_t length, int *perror) {
    assert(0);
}
