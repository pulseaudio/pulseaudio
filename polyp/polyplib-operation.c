#include <assert.h>

#include "xmalloc.h"
#include "polyplib-internal.h"
#include "polyplib-operation.h"

struct pa_operation *pa_operation_new(struct pa_context *c, struct pa_stream *s) {
    struct pa_operation *o;
    assert(c);

    o = pa_xmalloc(sizeof(struct pa_operation));
    o->ref = 1;
    o->context = pa_context_ref(c);
    o->stream = s ? pa_stream_ref(s) : NULL;

    o->state = PA_OPERATION_RUNNING;
    o->userdata = NULL;
    o->callback = NULL;

    PA_LLIST_PREPEND(struct pa_operation, o->context->operations, o);
    return pa_operation_ref(o);
}

struct pa_operation *pa_operation_ref(struct pa_operation *o) {
    assert(o && o->ref >= 1);
    o->ref++;
    return o;
}

void pa_operation_unref(struct pa_operation *o) {
    assert(o && o->ref >= 1);

    if ((--(o->ref)) == 0) {
        assert(!o->context);
        assert(!o->stream);
        free(o);
    }
}

static void operation_set_state(struct pa_operation *o, enum pa_operation_state st) {
    assert(o && o->ref >= 1);

    if (st == o->state)
        return;

    if (!o->context)
        return;

    o->state = st;

    if ((o->state == PA_OPERATION_DONE) || (o->state == PA_OPERATION_CANCELED)) {
        PA_LLIST_REMOVE(struct pa_operation, o->context->operations, o);
        pa_context_unref(o->context);
        if (o->stream)
            pa_stream_unref(o->stream);
        o->context = NULL;
        o->stream = NULL;
        o->callback = NULL;
        o->userdata = NULL;

        pa_operation_unref(o);
    }
}

void pa_operation_cancel(struct pa_operation *o) {
    assert(o && o->ref >= 1);
    operation_set_state(o, PA_OPERATION_CANCELED);
}

void pa_operation_done(struct pa_operation *o) {
    assert(o && o->ref >= 1);
    operation_set_state(o, PA_OPERATION_DONE);
}

enum pa_operation_state pa_operation_get_state(struct pa_operation *o) {
    assert(o && o->ref >= 1);
    return o->state;
}
