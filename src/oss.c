#include "module.h"

struct userdata {
    struct sink *sink;
    struct source *source;
    int fd;
};

int module_init(struct core *c, struct module*m) {
    struct userdata *u;
    assert(c && m);

    u = malloc(sizeof(struct userdata));
    assert(u);
    memset(u, 0, sizeof(struct userdata));
    m->userdata = u;

    return 0;
}

void module_done(struct core *c, struct module*m) {
    struct userdata *u;
    assert(c && m);

    u = m->userdata;

    sink_free(u->sink);
    source_free(u->source);
    free(u);
}
