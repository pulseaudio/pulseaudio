#ifndef fooclienthfoo
#define fooclienthfoo

#include "core.h"

struct client {
    char *name;
    uint32_t index;
    
    const char *protocol_name;

    void *userdata;
    void (*kill)(struct client *c);

    struct core *core;
};

struct client *client_new(struct core *c, const char *protocol_name, char *name);
void client_free(struct client *c);

#endif
