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

#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mainloop-signal.h"
#include "util.h"
#include "xmalloc.h"

struct signal_info {
    int sig;
    struct sigaction saved_sigaction;
    void (*callback) (void *id, int signal, void *userdata);
    void *userdata;
    struct signal_info *previous, *next;
};

static struct pa_mainloop_api *api = NULL;
static int signal_pipe[2] = { -1, -1 };
static void* mainloop_source = NULL;
static struct signal_info *signals = NULL;

static void signal_handler(int sig) {
    write(signal_pipe[1], &sig, sizeof(sig));
}

static void callback(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    assert(a && id && events == PA_MAINLOOP_API_IO_EVENT_INPUT && id == mainloop_source && fd == signal_pipe[0]);

    for (;;) {
        ssize_t r;
        int sig;
        struct signal_info*s;
        
        if ((r = read(signal_pipe[0], &sig, sizeof(sig))) < 0) {
            if (errno == EAGAIN)
                return;

            fprintf(stderr, "signal.c: read(): %s\n", strerror(errno));
            return;
        }

        if (r != sizeof(sig)) {
            fprintf(stderr, "signal.c: short read()\n");
            return;
        }

        for (s = signals; s; s = s->next) 
            if (s->sig == sig) {
                assert(s->callback);
                s->callback(s, sig, s->userdata);
                break;
            }
    }
}

int pa_signal_init(struct pa_mainloop_api *a) {
    assert(a);
    if (pipe(signal_pipe) < 0) {
        fprintf(stderr, "pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    pa_make_nonblock_fd(signal_pipe[0]);
    pa_make_nonblock_fd(signal_pipe[1]);

    api = a;
    mainloop_source = api->source_io(api, signal_pipe[0], PA_MAINLOOP_API_IO_EVENT_INPUT, callback, NULL);
    assert(mainloop_source);
    return 0;
}

void pa_signal_done(void) {
    assert(api && signal_pipe[0] >= 0 && signal_pipe[1] >= 0 && mainloop_source);

    api->cancel_io(api, mainloop_source);
    mainloop_source = NULL;

    close(signal_pipe[0]);
    close(signal_pipe[1]);
    signal_pipe[0] = signal_pipe[1] = -1;

    while (signals)
        pa_signal_unregister(signals);
    
    api = NULL;
}

void* pa_signal_register(int sig, void (*callback) (void *id, int signal, void *userdata), void *userdata) {
    struct signal_info *s = NULL;
    struct sigaction sa;
    assert(sig > 0 && callback);

    for (s = signals; s; s = s->next)
        if (s->sig == sig)
            goto fail;
    
    s = pa_xmalloc(sizeof(struct signal_info));
    s->sig = sig;
    s->callback = callback;
    s->userdata = userdata;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(sig, &sa, &s->saved_sigaction) < 0)
        goto fail;

    s->previous = NULL;
    s->next = signals;
    signals = s;

    return s;
fail:
    if (s)
        pa_xfree(s);
    return NULL;
}

void pa_signal_unregister(void *id) {
    struct signal_info *s = id;
    assert(s);

    if (s->next)
        s->next->previous = s->previous;
    if (s->previous)
        s->previous->next = s->next;
    else
        signals = s->next;

    sigaction(s->sig, &s->saved_sigaction, NULL);
    pa_xfree(s);
}
