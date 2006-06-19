/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>

#include "queue.h"

struct queue_entry {
    struct queue_entry *next;
    void *data;
};

struct pa_queue {
    struct queue_entry *front, *back;
    unsigned length;
};

pa_queue* pa_queue_new(void) {
    pa_queue *q = pa_xnew(pa_queue, 1);
    q->front = q->back = NULL;
    q->length = 0;
    return q;
}

void pa_queue_free(pa_queue* q, void (*destroy)(void *p, void *userdata), void *userdata) {
    struct queue_entry *e;
    assert(q);

    e = q->front;
    while (e) {
        struct queue_entry *n = e->next;

        if (destroy)
            destroy(e->data, userdata);

        pa_xfree(e);
        e = n;
    }

    pa_xfree(q);
}

void pa_queue_push(pa_queue *q, void *p) {
    struct queue_entry *e;

    e = pa_xnew(struct queue_entry, 1);
    e->data = p;
    e->next = NULL;

    if (q->back)
        q->back->next = e;
    else {
        assert(!q->front);
        q->front = e;
    }

    q->back = e;
    q->length++;
}

void* pa_queue_pop(pa_queue *q) {
    void *p;
    struct queue_entry *e;
    assert(q);

    if (!(e = q->front))
        return NULL;

    q->front = e->next;
    if (q->back == e)
        q->back = NULL;

    p = e->data;
    pa_xfree(e);

    q->length--;
    
    return p;
}

int pa_queue_is_empty(pa_queue *q) {
    assert(q);
    return q->length == 0;
}
