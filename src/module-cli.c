#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "module.h"
#include "iochannel.h"
#include "cli.h"
#include "sioman.h"

static void eof_cb(struct pa_cli*c, void *userdata) {
    struct pa_module *m = userdata;
    assert(c && m);

    pa_module_unload_request(m->core, m);
}

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct pa_iochannel *io;
    assert(c && m);

    if (pa_stdio_acquire() < 0) {
        fprintf(stderr, "STDIN/STDUSE already used\n");
        return -1;
    }

    io = pa_iochannel_new(c->mainloop, STDIN_FILENO, STDOUT_FILENO);
    assert(io);
    pa_iochannel_set_noclose(io, 1);

    m->userdata = pa_cli_new(c, io);
    assert(m->userdata);

    pa_cli_set_eof_callback(m->userdata, eof_cb, m);
    
    return 0;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    assert(c && m);

    pa_cli_free(m->userdata);
    pa_stdio_release();
}
