#ifndef fooclienthfoo
#define fooclienthfoo

#include "core.h"

struct client {
    char *name;
    uint32_t index;
    
    const char *protocol_name;

    void *kill_userdata;
    void (*kill)(struct client *c, void *userdata);

    struct core *core;
};

struct client *client_new(struct core *c, const char *protocol_name, char *name);

/* This function should be called only by the code that created the client */
void client_free(struct client *c);

/* The registrant of the client should call this function to set a
 * callback function which is called when destruction of the client is
 * requested */
void client_set_kill_callback(struct client *c, void (*kill)(struct client *c, void *userdata), void *userdata);

/* Code that didn't create the client should call this function to
 * request destruction of the client */
void client_kill(struct client *c);

#endif
