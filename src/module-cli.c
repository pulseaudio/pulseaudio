#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "main.h"
#include "module.h"
#include "iochannel.h"
#include "cli.h"

int module_init(struct core *c, struct module*m) {
    struct iochannel *io;
    assert(c && m);

    if (stdin_inuse || stdout_inuse) {
        fprintf(stderr, "STDIN/STDUSE already used\n");
        return -1;
    }

    stdin_inuse = stdout_inuse = 1;
    io = iochannel_new(c->mainloop, STDIN_FILENO, STDOUT_FILENO);
    assert(io);
    iochannel_set_noclose(io, 1);

    m->userdata = cli_new(c, io);
    assert(m->userdata);
    return 0;
}

void module_done(struct core *c, struct module*m) {
    assert(c && m);

    cli_free(m->userdata);
    assert(stdin_inuse && stdout_inuse);
    stdin_inuse = stdout_inuse = 0;
}
