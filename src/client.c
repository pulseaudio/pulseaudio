#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "strbuf.h"

struct pa_client *pa_client_new(struct pa_core *core, const char *protocol_name, char *name) {
    struct pa_client *c;
    int r;
    assert(core);

    c = malloc(sizeof(struct pa_client));
    assert(c);
    c->name = name ? strdup(name) : NULL;
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

char *pa_client_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_client *client;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u client(s).\n", pa_idxset_ncontents(c->clients));
    
    for (client = pa_idxset_first(c->clients, &index); client; client = pa_idxset_next(c->clients, &index))
        pa_strbuf_printf(s, "    index: %u, name: <%s>, protocol_name: <%s>\n", client->index, client->name, client->protocol_name);
    
    return pa_strbuf_tostring_free(s);
}


void pa_client_rename(struct pa_client *c, const char *name) {
    assert(c);
    free(c->name);
    c->name = name ? strdup(name) : NULL;
}
