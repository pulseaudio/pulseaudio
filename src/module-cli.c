#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "main.h"
#include "module.h"
#include "iochannel.h"
#include "cli.h"

int module_init(struct pa_core *c, struct pa_module*m) {
    struct pa_iochannel *io;
    assert(c && m);

    if (pa_stdin_inuse || pa_stdout_inuse) {
        fprintf(stderr, "STDIN/STDUSE already used\n");
        return -1;
    }

    pa_stdin_inuse = pa_stdout_inuse = 1;
    io = pa_iochannel_new(c->mainloop, STDIN_FILENO, STDOUT_FILENO);
    assert(io);
    pa_iochannel_set_noclose(io, 1);

    m->userdata = pa_cli_new(c, io);
    assert(m->userdata);
    return 0;
}

void module_done(struct pa_core *c, struct pa_module*m) {
    assert(c && m);

    pa_cli_free(m->userdata);
    assert(pa_stdin_inuse && pa_stdout_inuse);
    pa_stdin_inuse = pa_stdout_inuse = 0;
}
