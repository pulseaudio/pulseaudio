#include <assert.h>
#include <arpa/inet.h>

#include "module.h"
#include "socket-server.h"
#include "protocol-simple.h"

int module_init(struct core *c, struct module*m) {
    struct socket_server *s;
    assert(c && m);

#ifdef USE_TCP_SOCKETS
    if (!(s = socket_server_new_ipv4(c->mainloop, INADDR_LOOPBACK, 4712)))
        return -1;
#else
    if (!(s = socket_server_new_unix(c->mainloop, "/tmp/polypsimple")))
        return -1;
#endif

    m->userdata = protocol_simple_new(c, s, PROTOCOL_SIMPLE_PLAYBACK);
    assert(m->userdata);
    return 0;
}

void module_done(struct core *c, struct module*m) {
    assert(c && m);

    protocol_simple_free(m->userdata);
}
