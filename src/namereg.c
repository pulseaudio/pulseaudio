#include <string.h>
#include <assert.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>

#include "namereg.h"

struct namereg_entry {
    enum namereg_type type;
    char *name;
    void *data;
};

void namereg_free(struct core *c) {
    assert(c);
    if (!c->namereg)
        return;
    assert(hashset_ncontents(c->namereg) == 0);
    hashset_free(c->namereg, NULL, NULL);
}

const char *namereg_register(struct core *c, const char *name, enum namereg_type type, void *data, int fail) {
    struct namereg_entry *e;
    char *n = NULL;
    int r;
    
    assert(c && name && data);

    if (!c->namereg) {
        c->namereg = hashset_new(idxset_string_hash_func, idxset_string_compare_func);
        assert(c->namereg);
    }

    if ((e = hashset_get(c->namereg, name)) && fail)
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

            if (!(e = hashset_get(c->namereg, n)))
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

    r = hashset_put(c->namereg, e->name, e);
    assert (r >= 0);

    return e->name;
    
}

void namereg_unregister(struct core *c, const char *name) {
    struct namereg_entry *e;
    int r;
    assert(c && name);

    e = hashset_get(c->namereg, name);
    assert(e);

    r = hashset_remove(c->namereg, name);
    assert(r >= 0);

    free(e->name);
    free(e);
}

void* namereg_get(struct core *c, const char *name, enum namereg_type type) {
    struct namereg_entry *e;
    assert(c && name);

    if (!(e = hashset_get(c->namereg, name)))
        if (e->type == e->type)
            return e->data;

    return NULL;
}
