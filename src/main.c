#include <stdio.h>
#include <signal.h>
#include <stddef.h>
#include <assert.h>
#include <ltdl.h>

#include "core.h"
#include "mainloop.h"
#include "module.h"

static void signal_callback(struct mainloop_source *m, int sig, void *userdata) {
    mainloop_quit(mainloop_source_get_mainloop(m), -1);
    fprintf(stderr, "Got signal.\n");
}

int main(int argc, char *argv[]) {
    struct mainloop *m;
    struct core *c;
    int r;

    r = lt_dlinit();
    assert(r == 0);
    
    m = mainloop_new();
    assert(m);
    c = core_new(m);
    assert(c);

    mainloop_source_new_signal(m, SIGINT, signal_callback, NULL);
    signal(SIGPIPE, SIG_IGN);

    module_load(c, "sink-pipe", NULL);
    module_load(c, "protocol-simple-tcp", NULL);
    
    mainloop_run(m);
    
    core_free(c);
    mainloop_free(m);

    lt_dlexit();
    
    return 0;
}
