#include "simple.h"
#include "polyp.h"
#include "mainloop.h"

struct pa_simple {
    struct mainloop *mainloop;
    struct pa_context *context;
    struct pa_stream *stream;

    size_t requested;
    int dead;
};

static void playback_callback(struct pa_stream *p, size_t length, void *userdata) {
    struct pa_stream *sp = userdata;
    assert(p && length && sp);

    sp->requested = length;
}

struct pa_simple* pa_simple_new(
    const char *server,
    const char *name,
    enum pa_stream_direction dir,
    const char *dev,
    const char *stream_name,
    const struct pa_sample_spec *ss,
    const struct pa_buffer_attr *attr) {
    
    struct pa_simple *p;
    assert(ss);

    p = malloc(sizeof(struct pa_simple));
    assert(p);
    p->context = NULL;
    p->stream = NULL;
    p->mainloop = pa_mainloop_new();
    assert(p->mainloop);
    p->requested = 0;
    p->dead = 0;

    if (!(p->context = pa_context_new(pa_mainloop_get_api(p->mainloop), name)))
        goto fail;

    if (pa_context_connect(c, server, NULL, NULL) < 0)
        goto fail;

    while (!pa_context_is_ready(c)) {
        if (pa_context_is_dead(c))
            goto fail;
        
        if (mainloop_iterate(p->mainloop) < 0)
            goto fail;
    }

    if (!(p->stream = pa_stream_new(p->context, dir, sink, stream_name, ss, attr, NULL, NULL)))
        goto fail;

    while (!pa_stream_is_ready(c)) {
        if (pa_stream_is_dead(c))
            goto fail;

        if (mainloop_iterate(p->mainloop) < 0)
            goto fail;
    }

    pa_stream_set_write_callback(p->stream, playback_callback, p);

    return p;
    
fail:
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
        mainloop_free(s->mainloop);

    free(s);
}

int pa_simple_write(struct pa_simple *s, const void*data, size_t length) {
    assert(s && data);

    while (length > 0) {
        size_t l;
        
        while (!s->requested) {
            if (pa_context_is_dead(c))
                return -1;
            
            if (mainloop_iterate(s->mainloop) < 0)
                return -1;
        }

        l = length;
        if (l > s->requested)
            l = s->requested;

        pa_stream_write(s->stream, data, l);
        data += l;
        length -= l;
        s->requested = -l;
    }

    return 0;
}

int pa_simple_read(struct pa_simple *s, const void*data, size_t length) {
    assert(0);
}
