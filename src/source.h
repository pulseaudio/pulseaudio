#ifndef foosourcehfoo
#define foosourcehfoo

struct source;

#include <inttypes.h>
#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "memblock.h"

struct source {
    uint32_t index;
    
    char *name;
    struct core *core;
    struct pa_sample_spec sample_spec;
    struct idxset *outputs;

    void (*notify)(struct source*source);
    void *userdata;
};

struct source* source_new(struct core *core, const char *name, int fail, const struct pa_sample_spec *spec);
void source_free(struct source *s);

/* Pass a new memory block to all output streams */
void source_post(struct source*s, struct memchunk *b);

void source_notify(struct source *s);

char *source_list_to_string(struct core *c);

struct source* source_get_default(struct core *c);

#endif
