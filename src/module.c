#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include "module.h"

struct module* module_load(struct core *c, const char *name, const char *argument) {
    struct module *m = NULL;
    int r;
    
    assert(c && name);

    m = malloc(sizeof(struct module));
    assert(m);

    if (!(m->dl = lt_dlopenext(name)))
        goto fail;

    if (!(m->init = lt_dlsym(m->dl, "module_init")))
        goto fail;

    if (!(m->done = lt_dlsym(m->dl, "module_done")))
        goto fail;
    
    m->name = strdup(name);
    m->argument = argument ? strdup(argument) : NULL;
    m->userdata = NULL;
    m->core = c;

    assert(m->init);
    if (m->init(c, m) < 0)
        goto fail;

    if (!c->modules)
        c->modules = idxset_new(NULL, NULL);
    
    assert(c->modules);
    r = idxset_put(c->modules, m, &m->index);
    assert(r >= 0 && m->index != IDXSET_INVALID);
    return m;
    
fail:
    if (m) {
        free(m->argument);
        free(m->name);
        
        if (m->dl)
            lt_dlclose(m->dl);

        free(m);
    }

    return NULL;
}

static void module_free(struct module *m) {
    assert(m && m->done && m->core);
    m->done(m->core, m);

    lt_dlclose(m->dl);
    free(m->name);
    free(m->argument);
    free(m);
}


void module_unload(struct core *c, struct module *m) {
    assert(c && m);

    assert(c->modules);
    if (!(m = idxset_remove_by_data(c->modules, m, NULL)))
        return;

    module_free(m);
}

void module_unload_by_index(struct core *c, uint32_t index) {
    struct module *m;
    assert(c && index != IDXSET_INVALID);

    assert(c->modules);
    if (!(m = idxset_remove_by_index(c->modules, index)))
        return;

    module_free(m);
}

void free_callback(void *p, void *userdata) {
    struct module *m = p;
    assert(m);
    module_free(m);
}

void module_unload_all(struct core *c) {
    assert(c);

    if (!c->modules)
        return;

    idxset_free(c->modules, free_callback, NULL);
    c->modules = NULL;
}

