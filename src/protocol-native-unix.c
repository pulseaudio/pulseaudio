#include "module.h"

int module_init(struct core *c, struct module*m) {
    struct fn[PATH_MAX];
    struct socket_server *s;
    char *t;
    assert(c && m);

    if (!(t = getenv("TMP")))
        if (!(t = getenv("TEMP")))
            t = "/tmp";
    
    snprintf(fn, sizeof(fn), "%s/foosock", t);
             
    if (!(s = socket_server_new_unix(c->mainloop, fn)))
        return -1;

    m->userdata = protocol_native_new(s);
    assert(m->userdata);
    return 0;
}

void module_done(struct core *c, struct module*m) {
    assert(c && m);

    protocol_native_free(m->userdata);
}
