#ifndef fooclienthfoo
#define fooclienthfoo

#include "core.h"

struct client {
    uint32_t index;

    char *name;
    struct core *core;
    const char *protocol_name;

    void (*kill)(struct client *c);

    void *userdata;
};

struct client *client_new(struct core *c, const char *protocol_name, char *name);

/* This function should be called only by the code that created the client */
void client_free(struct client *c);

/* Code that didn't create the client should call this function to
 * request destruction of the client */
void client_kill(struct client *c);

#endif
