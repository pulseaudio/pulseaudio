#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "strbuf.h"

struct client *client_new(struct core *core, const char *protocol_name, char *name) {
    struct client *c;
    int r;
    assert(core);

    c = malloc(sizeof(struct client));
    assert(c);
    c->name = name ? strdup(name) : NULL;
    c->core = core;
    c->protocol_name = protocol_name;

    c->kill = NULL;
    c->userdata = NULL;

    r = idxset_put(core->clients, c, &c->index);
    assert(c->index != IDXSET_INVALID && r >= 0);

    fprintf(stderr, "client: created %u \"%s\"\n", c->index, c->name);
    
    return c;
}

void client_free(struct client *c) {
    assert(c && c->core);

    idxset_remove_by_data(c->core->clients, c, NULL);
    fprintf(stderr, "client: freed %u \"%s\"\n", c->index, c->name);
    free(c->name);
    free(c);
}

void client_kill(struct client *c) {
    assert(c);
    if (c->kill)
        c->kill(c);
}

char *client_list_to_string(struct core *c) {
    struct strbuf *s;
    struct client *client;
    uint32_t index = IDXSET_INVALID;
    assert(c);

    s = strbuf_new();
    assert(s);

    strbuf_printf(s, "%u client(s).\n", idxset_ncontents(c->clients));
    
    for (client = idxset_first(c->clients, &index); client; client = idxset_next(c->clients, &index))
        strbuf_printf(s, "    index: %u, name: <%s>, protocol_name: <%s>\n", client->index, client->name, client->protocol_name);
    
    return strbuf_tostring_free(s);
}

