#ifndef foosourcehfoo
#define foosourcehfoo

struct source;

#include <inttypes.h>
#include "core.h"
#include "sample.h"
#include "idxset.h"
#include "memblock.h"

struct source {
    char *name;
    uint32_t index;
    
    struct core *core;
    struct sample_spec sample_spec;
    struct idxset *output_streams;

    void (*link_change_callback)(struct source*source, void *userdata);
    void *userdata;
};

struct source* source_new(struct core *core, const char *name, const struct sample_spec *spec);
void source_free(struct source *s);

/* Pass a new memory block to all output streams */
void source_post(struct source*s, struct memchunk *b);

#endif
