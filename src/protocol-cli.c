#include <assert.h>
#include <stdlib.h>

#include "protocol-cli.h"
#include "cli.h"

struct pa_protocol_cli {
    struct pa_core *core;
    struct pa_socket_server*server;
    struct pa_idxset *connections;
};

static void cli_eof_cb(struct pa_cli*c, void*userdata) {
    struct pa_protocol_cli *p = userdata;
    assert(p);
    pa_idxset_remove_by_data(p->connections, c, NULL);
    pa_cli_free(c);
}

static void on_connection(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata) {
    struct pa_protocol_cli *p = userdata;
    struct pa_cli *c;
    assert(s && io && p);

    c = pa_cli_new(p->core, io);
    assert(c);
    pa_cli_set_eof_callback(c, cli_eof_cb, p);

    pa_idxset_put(p->connections, c, NULL);
}

struct pa_protocol_cli* pa_protocol_cli_new(struct pa_core *core, struct pa_socket_server *server) {
    struct pa_protocol_cli* p;
    assert(core && server);

    p = malloc(sizeof(struct pa_protocol_cli));
    assert(p);
    p->core = core;
    p->server = server;
    p->connections = pa_idxset_new(NULL, NULL);

    pa_socket_server_set_callback(p->server, on_connection, p);
    
    return p;
}

static void free_connection(void *p, void *userdata) {
    assert(p);
    pa_cli_free(p);
}

void pa_protocol_cli_free(struct pa_protocol_cli *p) {
    assert(p);

    pa_idxset_free(p->connections, free_connection, NULL);
    pa_socket_server_free(p->server);
    free(p);
}
