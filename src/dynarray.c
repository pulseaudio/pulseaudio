#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "dynarray.h"

struct dynarray {
    void **data;
    unsigned n_allocated, n_entries;
};

struct dynarray* dynarray_new(void) {
    struct dynarray *a;
    a = malloc(sizeof(struct dynarray));
    assert(a);
    a->data = NULL;
    a->n_entries = 0;
    a->n_allocated = 0;
    return a;
}

void dynarray_free(struct dynarray* a, void (*func)(void *p, void *userdata), void *userdata) {
    unsigned i;
    assert(a);

    if (func)
        for (i = 0; i < a->n_entries; i++)
            if (a->data[i])
                func(a->data[i], userdata);

    free(a->data);
    free(a);
}

void dynarray_put(struct dynarray*a, unsigned i, void *p) {
    assert(a);

    if (i >= a->n_allocated) {
        unsigned n;

        if (!p)
            return;

        n = i+100;
        a->data = realloc(a->data, sizeof(void*)*n);
        memset(a->data+a->n_allocated, 0, sizeof(void*)*(n-a->n_allocated));
        a->n_allocated = n;
    }

    a->data[i] = p;

    if (i >= a->n_entries)
        a->n_entries = i+1;
}

unsigned dynarray_append(struct dynarray*a, void *p) {
    unsigned i = a->n_entries;
    dynarray_put(a, i, p);
    return i;
}

void *dynarray_get(struct dynarray*a, unsigned i) {
    assert(a);
    if (i >= a->n_allocated)
        return NULL;
    assert(a->data);
    return a->data[i];
}

unsigned dynarray_ncontents(struct dynarray*a) {
    assert(a);
    return a->n_entries;
}
