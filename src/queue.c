#include <assert.h>
#include <stdlib.h>

#include "queue.h"

struct queue_entry {
    struct queue_entry *next;
    void *data;
};

struct queue {
    struct queue_entry *front, *back;
    unsigned length;
};

struct queue* queue_new(void) {
    struct queue *q = malloc(sizeof(struct queue));
    assert(q);
    q->front = q->back = NULL;
    q->length = 0;
    return q;
}

void queue_free(struct queue* q, void (*destroy)(void *p, void *userdata), void *userdata) {
    struct queue_entry *e;
    assert(q);

    e = q->front;
    while (e) {
        struct queue_entry *n = e->next;

        if (destroy)
            destroy(e->data, userdata);

        free(e);
        e = n;
    }

    free(q);
}

void queue_push(struct queue *q, void *p) {
    struct queue_entry *e;

    e = malloc(sizeof(struct queue_entry));

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

void* queue_pop(struct queue *q) {
    void *p;
    struct queue_entry *e;
    assert(q);

    if (!(e = q->front))
        return NULL;

    q->front = e->next;
    if (q->back == e)
        q->back = NULL;

    p = e->data;
    free(e);

    return p;
}
