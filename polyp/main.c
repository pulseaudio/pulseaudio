/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
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
#include "cmdline.h"
#include "cli-command.h"
#include "util.h"
#include "sioman.h"
#include "xmalloc.h"
#include "cpulimit.h"

static struct pa_mainloop *mainloop;

static void drop_root(void) {
    if (getuid() != 0 && geteuid() == 0) {
        fprintf(stderr, __FILE__": started SUID root, dropping root rights.\n");
        setuid(getuid());
        seteuid(getuid());
    }
}

static const char* signal_name(int s) {
    switch(s) {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGUSR1: return "SIGUSR1";
        case SIGUSR2: return "SIGUSR2";
        case SIGXCPU: return "SIGXCPU";
        case SIGPIPE: return "SIGPIPE";
        default: return "UNKNOWN SIGNAL";
    }
}

static void signal_callback(struct pa_mainloop_api*m, struct pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, __FILE__": got signal %s.\n", signal_name(sig));

    switch (sig) {
        case SIGUSR1:
            pa_module_load(userdata, "module-cli", NULL);
            return;
            
        case SIGUSR2:
            pa_module_load(userdata, "module-cli-protocol-unix", NULL);
            return;
        
        case SIGINT:
        case SIGTERM:
        default:
            fprintf(stderr, "Exiting.\n");
            m->quit(m, 1);
            return;
    }
}

static void close_pipe(int p[2]) {
    if (p[0] != -1)
        close(p[0]);
    if (p[1] != -1)
        close(p[1]);
    p[0] = p[1] = -1;
}

int main(int argc, char *argv[]) {
    struct pa_core *c;
    struct pa_cmdline *cmdline = NULL;
    struct pa_strbuf *buf = NULL;
    char *s;
    int r, retval = 1;
    int daemon_pipe[2] = { -1, -1 };

    if (!(cmdline = pa_cmdline_parse(argc, argv))) {
        fprintf(stderr, __FILE__": failed to parse command line.\n");
        goto finish;
    }

    if (cmdline->help) {
        pa_cmdline_help(argv[0]);
        retval = 0;
        goto finish;
    }

    if (cmdline->version) {
        printf(PACKAGE_NAME" "PACKAGE_VERSION"\n");
        retval = 0;
        goto finish;
    }

    if (cmdline->high_priority)
        pa_raise_priority();
    
    if (!cmdline->stay_root)
        drop_root();

    if (cmdline->daemonize) {
        pid_t child;

        if (pa_stdio_acquire() < 0) {
            fprintf(stderr, __FILE__": failed to acquire stdio.\n");
            goto finish;
        }

        if (pipe(daemon_pipe) < 0) {
            fprintf(stderr, __FILE__": failed to create pipe.\n");
            goto finish;
        }
        
        if ((child = fork()) < 0) {
            fprintf(stderr, __FILE__": fork() failed: %s\n", strerror(errno));
            goto finish;
        }

        if (child != 0) {
            /* Father */

            close(daemon_pipe[1]);
            daemon_pipe[1] = -1;

            if (pa_loop_read(daemon_pipe[0], &retval, sizeof(retval)) != sizeof(retval)) {
                fprintf(stderr, __FILE__": read() failed: %s\n", strerror(errno));
                retval = 1;
            }

            goto finish;
        }

        close(daemon_pipe[0]);
        daemon_pipe[0] = -1;
        
        setsid();
        setpgrp();
        
        close(0);
        close(1);
    }
    
    r = lt_dlinit();
    assert(r == 0);
#ifdef DLSEARCHDIR
    lt_dladdsearchdir(DLSEARCHDIR);
#endif

    mainloop = pa_mainloop_new();
    assert(mainloop);

    r = pa_signal_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    pa_signal_new(SIGINT, signal_callback, c);
    pa_signal_new(SIGTERM, signal_callback, c);
    signal(SIGPIPE, SIG_IGN);

    c = pa_core_new(pa_mainloop_get_api(mainloop));
    assert(c);
    
    pa_signal_new(SIGUSR1, signal_callback, c);
    pa_signal_new(SIGUSR2, signal_callback, c);

    r = pa_cpu_limit_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    
    buf = pa_strbuf_new();
    assert(buf);
    r = pa_cli_command_execute(c, cmdline->cli_commands, buf, &cmdline->fail, &cmdline->verbose);
    fprintf(stderr, s = pa_strbuf_tostring_free(buf));
    pa_xfree(s);
    
    if (r < 0 && cmdline->fail) {
        fprintf(stderr, __FILE__": failed to initialize daemon.\n");
        if (cmdline->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval));
    } else if (!c->modules || pa_idxset_ncontents(c->modules) == 0) {
        fprintf(stderr, __FILE__": daemon startup without any loaded modules, refusing to work.\n");
        if (cmdline->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval));
    } else {
        retval = 0;
        if (cmdline->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval));

        c->disallow_module_loading = cmdline->disallow_module_loading;
        c->quit_after_last_client_time = cmdline->quit_after_last_client_time;
        
        fprintf(stderr, __FILE__": mainloop entry.\n");
        if (pa_mainloop_run(mainloop, &retval) < 0)
            retval = 1;
        fprintf(stderr, __FILE__": mainloop exit.\n");
    }
        
    pa_core_free(c);

    pa_cpu_limit_done();
    pa_signal_done();
    pa_mainloop_free(mainloop);
    
    lt_dlexit();

finish:

    if (cmdline)
        pa_cmdline_free(cmdline);

    close_pipe(daemon_pipe);
    
    return retval;
}
