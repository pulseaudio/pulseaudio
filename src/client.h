#ifndef fooclienthfoo
#define fooclienthfoo

#include "core.h"
#include "module.h"

struct pa_client {
    uint32_t index;

    struct pa_module *owner;
    char *name;
    struct pa_core *core;
    const char *protocol_name;

    void (*kill)(struct pa_client *c);
    void *userdata;
};

struct pa_client *pa_client_new(struct pa_core *c, const char *protocol_name, char *name);

/* This function should be called only by the code that created the client */
void pa_client_free(struct pa_client *c);

/* Code that didn't create the client should call this function to
 * request destruction of the client */
void pa_client_kill(struct pa_client *c);

void pa_client_rename(struct pa_client *c, const char *name);

#endif
