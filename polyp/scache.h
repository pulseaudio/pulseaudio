#ifndef fooscachehfoo
#define fooscachehfoo

#include "core.h"
#include "memchunk.h"
#include "sink.h"

struct pa_scache_entry {
    uint32_t index;
    char *name;
    struct pa_sample_spec sample_spec;
    struct pa_memchunk memchunk;
};

void pa_scache_add_item(struct pa_core *c, const char *name, struct pa_sample_spec *ss, struct pa_memchunk *chunk, uint32_t *index);

int pa_scache_remove_item(struct pa_core *c, const char *name);
int pa_scache_play_item(struct pa_core *c, const char *name, struct pa_sink *sink, uint32_t volume);
void pa_scache_free(struct pa_core *c);

const char *pa_scache_get_name_by_id(struct pa_core *c, uint32_t id);
uint32_t pa_scache_get_id_by_name(struct pa_core *c, const char *name);

#endif
