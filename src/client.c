#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"

struct pa_client *pa_client_new(struct pa_core *core, const char *protocol_name, char *name) {
    struct pa_client *c;
    int r;
    assert(core);

    c = malloc(sizeof(struct pa_client));
    assert(c);
    c->name = name ? strdup(name) : NULL;
    c->owner = NULL;
    c->core = core;
    c->protocol_name = protocol_name;

    c->kill = NULL;
    c->userdata = NULL;

    r = pa_idxset_put(core->clients, c, &c->index);
    assert(c->index != PA_IDXSET_INVALID && r >= 0);

    fprintf(stderr, "client: created %u \"%s\"\n", c->index, c->name);
    
    return c;
}

void pa_client_free(struct pa_client *c) {
    assert(c && c->core);

    pa_idxset_remove_by_data(c->core->clients, c, NULL);
    fprintf(stderr, "client: freed %u \"%s\"\n", c->index, c->name);
    free(c->name);
    free(c);
}

void pa_client_kill(struct pa_client *c) {
    assert(c);
    if (!c->kill) {
        fprintf(stderr, "kill() operation not implemented for client %u\n", c->index);
        return;
    }

    c->kill(c);
}

void pa_client_rename(struct pa_client *c, const char *name) {
    assert(c);
    free(c->name);
    c->name = name ? strdup(name) : NULL;
}
