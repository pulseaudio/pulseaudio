#include <stdio.h>
#include <signal.h>
#include <stddef.h>
#include <assert.h>
#include <ltdl.h>
#include <memblock.h>

#include "core.h"
#include "mainloop.h"
#include "module.h"
#include "mainloop-signal.h"

int stdin_inuse = 0, stdout_inuse = 0;

static struct pa_mainloop *mainloop;

static void signal_callback(void *id, int sig, void *userdata) {
    struct pa_mainloop_api* m = pa_mainloop_get_api(mainloop);
    m->quit(m, 1);
    fprintf(stderr, "main: got signal.\n");
}

int main(int argc, char *argv[]) {
    struct core *c;
    int r, retval = 0;

    r = lt_dlinit();
    assert(r == 0);
    
    mainloop = pa_mainloop_new();
    assert(mainloop);

    r = pa_signal_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    pa_signal_register(SIGINT, signal_callback, NULL);
    signal(SIGPIPE, SIG_IGN);

    c = core_new(pa_mainloop_get_api(mainloop));
    assert(c);

    module_load(c, "module-oss", "/dev/dsp1");
/*    module_load(c, "module-pipe-sink", NULL);
    module_load(c, "module-simple-protocol-tcp", NULL);
    module_load(c, "module-simple-protocol-unix", NULL);
    module_load(c, "module-cli-protocol-tcp", NULL);
    module_load(c, "module-cli-protocol-unix", NULL);
    module_load(c, "module-native-protocol-tcp", NULL);
    module_load(c, "module-native-protocol-unix", NULL);*/
    module_load(c, "module-esound-protocol-tcp", NULL);
    module_load(c, "module-cli", NULL);
    
    fprintf(stderr, "main: mainloop entry.\n");
    if (pa_mainloop_run(mainloop, &retval) < 0)
        retval = 1;
    fprintf(stderr, "main: mainloop exit.\n");
    
    core_free(c);

    pa_signal_done();
    pa_mainloop_free(mainloop);

    lt_dlexit();
    
    return retval;
}
