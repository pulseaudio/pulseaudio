#ifndef foomodulehfoo
#define foomodulehfoo

#include <inttypes.h>
#include <ltdl.h>

#include "core.h"

struct module {
    char *name, *argument;
    uint32_t index;

    lt_dlhandle *dl;
    
    int (*init)(struct core *c, struct module*m);
    void (*done)(struct core *c, struct module*m);

    void *userdata;
};

struct module* module_load(struct core *c, const char *name, const char*argument);
void module_unload(struct core *c, struct module *m);
void module_unload_by_index(struct core *c, uint32_t index);

void module_unload_all(struct core *c);

#endif
