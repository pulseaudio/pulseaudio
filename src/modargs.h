#ifndef foomodargshfoo
#define foomodargshfoo

#include <inttypes.h>
#include "sample.h"
#include "core.h"

struct pa_modargs;

struct pa_modargs *pa_modargs_new(const char *args, const char* const* keys);
void pa_modargs_free(struct pa_modargs*ma);

const char *pa_modargs_get_value(struct pa_modargs *ma, const char *key, const char *def);
int pa_modargs_get_value_u32(struct pa_modargs *ma, const char *key, uint32_t *value);

int pa_modargs_get_sample_spec(struct pa_modargs *ma, struct pa_sample_spec *ss);

int pa_modargs_get_source_index(struct pa_modargs *ma, struct pa_core *c, uint32_t *index);
int pa_modargs_get_sink_index(struct pa_modargs *ma, struct pa_core *c, uint32_t *index);

#endif
