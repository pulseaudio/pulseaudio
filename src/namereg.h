#ifndef foonamereghfoo
#define foonamereghfoo

#include "core.h"

enum pa_namereg_type {
    PA_NAMEREG_SINK,
    PA_NAMEREG_SOURCE
};

void pa_namereg_free(struct pa_core *c);

const char *pa_namereg_register(struct pa_core *c, const char *name, enum pa_namereg_type type, void *data, int fail);
void pa_namereg_unregister(struct pa_core *c, const char *name);
void* pa_namereg_get(struct pa_core *c, const char *name, enum pa_namereg_type type);

#endif
