#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"

struct client *client_new(struct core *core, const char *protocol_name, char *name) {
    struct client *c;
    int r;
    assert(core);

    c = malloc(sizeof(struct client));
    assert(c);
    c->protocol_name = protocol_name;
    c->name = name ? strdup(name) : NULL;
    c->kill = NULL;
    c->userdata = NULL;
    c->core = core;

    r = idxset_put(core->clients, c, &c->index);
    assert(c->index != IDXSET_INVALID && r >= 0);
    
    return c;
}

void client_free(struct client *c) {
    assert(c && c->core);

    idxset_remove_by_data(c->core->clients, c, NULL);
    free(c->name);
    free(c);
}
