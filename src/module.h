#ifndef foomodulehfoo
#define foomodulehfoo

#include <inttypes.h>
#include <ltdl.h>

#include "core.h"

struct pa_module {
    struct pa_core *core;
    char *name, *argument;
    uint32_t index;

    lt_dlhandle dl;
    
    int (*init)(struct pa_core *c, struct pa_module*m);
    void (*done)(struct pa_core *c, struct pa_module*m);

    void *userdata;
};

struct pa_module* pa_module_load(struct pa_core *c, const char *name, const char*argument);
void pa_module_unload(struct pa_core *c, struct pa_module *m);
void pa_module_unload_by_index(struct pa_core *c, uint32_t index);

void pa_module_unload_all(struct pa_core *c);

char *pa_module_list_to_string(struct pa_core *c);

void pa_module_unload_request(struct pa_core *c, struct pa_module *m);


/* These to following prototypes are for module entrypoints and not implemented by the core */
int pa_module_init(struct pa_core *c, struct pa_module*m);
void pa_module_done(struct pa_core *c, struct pa_module*m);

#endif
