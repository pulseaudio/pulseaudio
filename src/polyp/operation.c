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

#include <assert.h>

#include <polypcore/xmalloc.h>
#include "internal.h"
#include "operation.h"

pa_operation *pa_operation_new(pa_context *c, pa_stream *s) {
    pa_operation *o;
    assert(c);

    o = pa_xmalloc(sizeof(pa_operation));
    o->ref = 1;
    o->context = pa_context_ref(c);
    o->stream = s ? pa_stream_ref(s) : NULL;

    o->state = PA_OPERATION_RUNNING;
    o->userdata = NULL;
    o->callback = NULL;

    PA_LLIST_PREPEND(pa_operation, o->context->operations, o);
    return pa_operation_ref(o);
}

pa_operation *pa_operation_ref(pa_operation *o) {
    assert(o && o->ref >= 1);
    o->ref++;
    return o;
}

void pa_operation_unref(pa_operation *o) {
    assert(o && o->ref >= 1);

    if ((--(o->ref)) == 0) {
        assert(!o->context);
        assert(!o->stream);
        free(o);
    }
}

static void operation_set_state(pa_operation *o, pa_operation_state_t st) {
    assert(o && o->ref >= 1);

    if (st == o->state)
        return;

    if (!o->context)
        return;

    o->state = st;

    if ((o->state == PA_OPERATION_DONE) || (o->state == PA_OPERATION_CANCELED)) {
        PA_LLIST_REMOVE(pa_operation, o->context->operations, o);
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

void pa_operation_cancel(pa_operation *o) {
    assert(o && o->ref >= 1);
    operation_set_state(o, PA_OPERATION_CANCELED);
}

void pa_operation_done(pa_operation *o) {
    assert(o && o->ref >= 1);
    operation_set_state(o, PA_OPERATION_DONE);
}

pa_operation_state_t pa_operation_get_state(pa_operation *o) {
    assert(o && o->ref >= 1);
    return o->state;
}
