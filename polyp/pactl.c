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

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "polyplib.h"
#include "polyplib-error.h"
#include "mainloop.h"
#include "mainloop-signal.h"

static struct pa_context *context = NULL;
static struct pa_mainloop_api *mainloop_api = NULL;

static enum {
    NONE,
    EXIT,
    STAT
} action = NONE;

static void quit(int ret) {
    assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}

static void context_die_callback(struct pa_context *c, void *userdata) {
    assert(c);
    fprintf(stderr, "Connection to server shut down, exiting.\n");
    quit(1);
}

static void context_drain_complete(struct pa_context *c, void *userdata) {
    assert(c);
    fprintf(stderr, "Connection to server shut down, exiting.\n");
    quit(0);
}

static void drain(void) {
    if (pa_context_drain(context, context_drain_complete, NULL) < 0)
        quit(0);
}

static void stat_callback(struct pa_context *c, uint32_t blocks, uint32_t total, void *userdata) {
    if (blocks == (uint32_t) -1) {
        fprintf(stderr, "Failed to get statistics: %s\n", pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }
    
    fprintf(stderr, "Currently in use: %u blocks containing %u bytes total.\n", blocks, total);
    drain();
}

static void context_complete_callback(struct pa_context *c, int success, void *userdata) {
    assert(c);

    if (!success) {
        fprintf(stderr, "Connection failed: %s\n", pa_strerror(pa_context_errno(c)));
        goto fail;
    }

    fprintf(stderr, "Connection established.\n");

    if (action == STAT)
        pa_context_stat(c, stat_callback, NULL);
    else {
        assert(action == EXIT);
        pa_context_exit(c);
        drain();
    }
    
    return;
    
fail:
    quit(1);
}

static void exit_signal_callback(void *id, int sig, void *userdata) {
    fprintf(stderr, "Got SIGINT, exiting.\n");
    quit(0);
    
}

int main(int argc, char *argv[]) {
    struct pa_mainloop* m = NULL;
    int ret = 1, r;

    if (argc >= 2) {
        if (!strcmp(argv[1], "stat"))
            action = STAT;
        else if (!strcmp(argv[1], "exit"))
            action = EXIT;
    }

    if (action == NONE) {
        fprintf(stderr, "No valid action specified. Use one of: stat, exit\n");
        goto quit;
    }

    if (!(m = pa_mainloop_new())) {
        fprintf(stderr, "pa_mainloop_new() failed.\n");
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    r = pa_signal_init(mainloop_api);
    assert(r == 0);
    pa_signal_register(SIGINT, exit_signal_callback, NULL);
    signal(SIGPIPE, SIG_IGN);
    
    if (!(context = pa_context_new(mainloop_api, argv[0]))) {
        fprintf(stderr, "pa_context_new() failed.\n");
        goto quit;
    }

    if (pa_context_connect(context, NULL, context_complete_callback, NULL) < 0) {
        fprintf(stderr, "pa_context_connext() failed.\n");
        goto quit;
    }
        
    pa_context_set_die_callback(context, context_die_callback, NULL);

    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, "pa_mainloop_run() failed.\n");
        goto quit;
    }

quit:
    if (context)
        pa_context_free(context);

    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }
    
    return ret;
}
