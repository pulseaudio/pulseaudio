#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "module.h"


static void free_deps(struct dependency_module** deps) {
    assert(deps);
    
    while (*deps) {
        struct dependency_module *next = (*deps)->next;
        lt_dlclose((*deps)->dl);
        free(deps);
        *deps = next;
    }
}

static int load_deps(const char *fname, struct dependency_module **deps) {
    char line[PATH_MAX];
    FILE *f;
    char depfile[PATH_MAX];
    assert(fname && deps);

    snprintf(depfile, sizeof(depfile), "%s.moddep", fname);
    
    if (!(f = fopen(depfile, "r")))
        return -1;

    while (fgets(line, sizeof(line)-1, f)) {
        lt_dlhandle dl;
        char *p;
        size_t l;
        struct dependency_module* d;

        p = line + strspn(line, " \t");
        
        l = strlen(p);
        if (p[l-1] == '\n')
            p[l-1] = 0;

        if (*p == '#' || *p == 0)
            continue;

        load_deps(p, deps);
        
        if (!(dl = lt_dlopenext(p))) {
            free_deps(deps);
            fclose(f);
            return -1;
        }

        d = malloc(sizeof(struct dependency_module));
        assert(d);
        d->dl = dl;
        d->next = *deps;
        *deps = d;
    }

    fclose(f);
    return 0;
}

struct module* module_load(struct core *c, const char *name, const char *argument) {
    struct module *m = NULL;
    int r;
    
    assert(c && name);

    m = malloc(sizeof(struct module));
    assert(m);

    m->dl = NULL;
    
    m->dependencies = NULL;
    if (load_deps(name, &m->dependencies) < 0)
        goto fail;

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
        if (m->dl)
            lt_dlclose(m->dl);

        free_deps(&m->dependencies);
        free(m);
    }

    return NULL;
}

static void module_free(struct module *m) {
    assert(m && m->done && m->core);
    m->done(m->core, m);

    lt_dlclose(m->dl);
    free_deps(&m->dependencies);
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

