#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>

#include "namereg.h"

struct namereg_entry {
    enum pa_namereg_type type;
    char *name;
    void *data;
};

void pa_namereg_free(struct pa_core *c) {
    assert(c);
    if (!c->namereg)
        return;
    assert(pa_hashset_ncontents(c->namereg) == 0);
    pa_hashset_free(c->namereg, NULL, NULL);
}

const char *pa_namereg_register(struct pa_core *c, const char *name, enum pa_namereg_type type, void *data, int fail) {
    struct namereg_entry *e;
    char *n = NULL;
    int r;
    
    assert(c && name && data);

    if (!c->namereg) {
        c->namereg = pa_hashset_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        assert(c->namereg);
    }

    if ((e = pa_hashset_get(c->namereg, name)) && fail)
        return NULL;

    if (!e)
        n = strdup(name);
    else {
        unsigned i;
        size_t l = strlen(name);
        n = malloc(l+3);
        assert(n);
        
        for (i = 1; i <= 99; i++) {
            snprintf(n, l+2, "%s%u", name, i);

            if (!(e = pa_hashset_get(c->namereg, n)))
                break;
        }

        if (e) {
            free(n);
            return NULL;
        }
    }
    
    assert(n);
    e = malloc(sizeof(struct namereg_entry));
    assert(e);
    e->type = type;
    e->name = n;
    e->data = data;

    r = pa_hashset_put(c->namereg, e->name, e);
    assert (r >= 0);

    return e->name;
    
}

void pa_namereg_unregister(struct pa_core *c, const char *name) {
    struct namereg_entry *e;
    int r;
    assert(c && name);

    e = pa_hashset_get(c->namereg, name);
    assert(e);

    r = pa_hashset_remove(c->namereg, name);
    assert(r >= 0);

    free(e->name);
    free(e);
}

void* pa_namereg_get(struct pa_core *c, const char *name, enum pa_namereg_type type) {
    struct namereg_entry *e;
    uint32_t index;
    char *x = NULL;
    void *d = NULL;
    assert(c && name);

    if ((e = pa_hashset_get(c->namereg, name)))
        if (e->type == e->type)
            return e->data;

    index = (uint32_t) strtol(name, &x, 0);

    if (!x || *x != 0)
        return NULL;

    if (type == PA_NAMEREG_SINK)
        d = pa_idxset_get_by_index(c->sinks, index);
    else if (type == PA_NAMEREG_SOURCE)
        d = pa_idxset_get_by_index(c->sources, index);

    return d;
}
