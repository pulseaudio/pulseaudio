#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "dynarray.h"

struct pa_dynarray {
    void **data;
    unsigned n_allocated, n_entries;
};

struct pa_dynarray* pa_dynarray_new(void) {
    struct pa_dynarray *a;
    a = malloc(sizeof(struct pa_dynarray));
    assert(a);
    a->data = NULL;
    a->n_entries = 0;
    a->n_allocated = 0;
    return a;
}

void pa_dynarray_free(struct pa_dynarray* a, void (*func)(void *p, void *userdata), void *userdata) {
    unsigned i;
    assert(a);

    if (func)
        for (i = 0; i < a->n_entries; i++)
            if (a->data[i])
                func(a->data[i], userdata);

    free(a->data);
    free(a);
}

void pa_dynarray_put(struct pa_dynarray*a, unsigned i, void *p) {
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

unsigned pa_dynarray_append(struct pa_dynarray*a, void *p) {
    unsigned i = a->n_entries;
    pa_dynarray_put(a, i, p);
    return i;
}

void *pa_dynarray_get(struct pa_dynarray*a, unsigned i) {
    assert(a);
    if (i >= a->n_allocated)
        return NULL;
    assert(a->data);
    return a->data[i];
}

unsigned pa_dynarray_ncontents(struct pa_dynarray*a) {
    assert(a);
    return a->n_entries;
}
