#ifndef fooautoloadhfoo
#define fooautoloadhfoo

#include "namereg.h"

struct pa_autoload_entry {
    char *name;
    enum pa_namereg_type type;
    char *module, *argument;
};

void pa_autoload_add(struct pa_core *c, const char*name, enum pa_namereg_type type, const char*module, const char *argument);
void pa_autoload_free(struct pa_core *c);
int pa_autoload_remove(struct pa_core *c, const char*name, enum pa_namereg_type type);
void pa_autoload_request(struct pa_core *c, const char *name, enum pa_namereg_type type);

#endif
