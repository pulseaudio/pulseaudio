#include <assert.h>
#include <stdlib.h>

#include "protocol-cli.h"
#include "cli.h"

struct protocol_cli {
    struct core *core;
    struct socket_server*server;
    struct idxset *connections;
};

static void cli_eof_cb(struct cli*c, void*userdata) {
    struct protocol_cli *p = userdata;
    assert(c && p);

    idxset_remove_by_data(p->connections, c, NULL);
    cli_free(c);
}

static void on_connection(struct socket_server*s, struct iochannel *io, void *userdata) {
    struct protocol_cli *p = userdata;
    struct cli *c;
    assert(s && io && p);
    
    c = cli_new(p->core, io);
    assert(c);
    cli_set_eof_callback(c, cli_eof_cb, p);

    idxset_put(p->connections, c, NULL);
}

struct protocol_cli* protocol_cli_new(struct core *core, struct socket_server *server) {
    struct protocol_cli* p;
    assert(core && server);

    p = malloc(sizeof(struct protocol_cli));
    assert(p);
    p->core = core;
    p->server = server;
    p->connections = idxset_new(NULL, NULL);

    socket_server_set_callback(p->server, on_connection, p);
    
    return p;
}

static void free_connection(void *p, void *userdata) {
    assert(p);
    cli_free(p);
}

void protocol_cli_free(struct protocol_cli *p) {
    assert(p);

    idxset_free(p->connections, free_connection, NULL);
    socket_server_free(p->server);
    free(p);
}
