#ifndef foonamereghfoo
#define foonamereghfoo

#include "core.h"

enum namereg_type {
    NAMEREG_SINK,
    NAMEREG_SOURCE
};

void namereg_free(struct core *c);

const char *namereg_register(struct core *c, const char *name, enum namereg_type type, void *data, int fail);
void namereg_unregister(struct core *c, const char *name);
void* namereg_get(struct core *c, const char *name, enum namereg_type type);

#endif
