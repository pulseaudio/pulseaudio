/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdlib.h>

#include "queue.h"
#include "xmalloc.h"

struct queue_entry {
    struct queue_entry *next;
    void *data;
};

struct pa_queue {
    struct queue_entry *front, *back;
    unsigned length;
};

struct pa_queue* pa_queue_new(void) {
    struct pa_queue *q = pa_xmalloc(sizeof(struct pa_queue));
    q->front = q->back = NULL;
    q->length = 0;
    return q;
}

void pa_queue_free(struct pa_queue* q, void (*destroy)(void *p, void *userdata), void *userdata) {
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

void pa_queue_push(struct pa_queue *q, void *p) {
    struct queue_entry *e;

    e = pa_xmalloc(sizeof(struct queue_entry));
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

void* pa_queue_pop(struct pa_queue *q) {
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

int pa_queue_is_empty(struct pa_queue *q) {
    assert(q);
    return q->length == 0;
}
