#include <stdio.h>
#include <signal.h>
#include <stddef.h>
#include <assert.h>
#include <ltdl.h>
#include <memblock.h>

#include "core.h"
#include "mainloop.h"
#include "module.h"

static void signal_callback(struct mainloop_source *m, int sig, void *userdata) {
    mainloop_quit(mainloop_source_get_mainloop(m), -1);
    fprintf(stderr, "main: got signal.\n");
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

    module_load(c, "module-oss-mmap", "/dev/dsp1");
    module_load(c, "module-pipe-sink", NULL);
    module_load(c, "module-simple-protocol-tcp", NULL);
    
    fprintf(stderr, "main: mainloop entry.\n");
    while (mainloop_iterate(m, 1) == 0);
/*        fprintf(stderr, "main: %u blocks\n", n_blocks);*/
    fprintf(stderr, "main: mainloop exit.\n");
        

    mainloop_run(m);
    
    core_free(c);
    mainloop_free(m);

    lt_dlexit();
    
    return 0;
}
