#include "module.h"

int module_init(struct core *c, struct module*m) {
    struct socket_server *s;
    assert(c && m);

    if (!(s = socket_server_new_ipv4(c->mainloop, INADDR_LOOPBACK, 4711)))
        return -1;

    m->userdata = protocol_native_new(s);
    assert(m->userdata);
    return 0;
}

void module_done(struct core *c, struct module*m) {
    assert(c && m);

    protocol_native_free(m->userdata);
}
