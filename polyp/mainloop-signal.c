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
#include "log.h"

struct pa_signal_event {
    int sig;
    struct sigaction saved_sigaction;
    void (*callback) (struct pa_mainloop_api*a, struct pa_signal_event *e, int signal, void *userdata);
    void *userdata;
    void (*destroy_callback) (struct pa_mainloop_api*a, struct pa_signal_event*e, void *userdata);
    struct pa_signal_event *previous, *next;
};

static struct pa_mainloop_api *api = NULL;
static int signal_pipe[2] = { -1, -1 };
static struct pa_io_event* io_event = NULL;
static struct pa_signal_event *signals = NULL;

static void signal_handler(int sig) {
    write(signal_pipe[1], &sig, sizeof(sig));
}

static void callback(struct pa_mainloop_api*a, struct pa_io_event*e, int fd, enum pa_io_event_flags f, void *userdata) {
    ssize_t r;
    int sig;
    struct pa_signal_event*s;
    assert(a && e && f == PA_IO_EVENT_INPUT && e == io_event && fd == signal_pipe[0]);

        
    if ((r = read(signal_pipe[0], &sig, sizeof(sig))) < 0) {
        if (errno == EAGAIN)
            return;

        pa_log(__FILE__": read(): %s\n", strerror(errno));
        return;
    }
    
    if (r != sizeof(sig)) {
        pa_log(__FILE__": short read()\n");
        return;
    }
    
    for (s = signals; s; s = s->next) 
        if (s->sig == sig) {
            assert(s->callback);
            s->callback(a, s, sig, s->userdata);
            break;
        }
}

int pa_signal_init(struct pa_mainloop_api *a) {
    assert(!api && a && signal_pipe[0] == -1 && signal_pipe[1] == -1 && !io_event);
    
    if (pipe(signal_pipe) < 0) {
        pa_log(__FILE__": pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    pa_make_nonblock_fd(signal_pipe[0]);
    pa_make_nonblock_fd(signal_pipe[1]);
    pa_fd_set_cloexec(signal_pipe[0], 1);
    pa_fd_set_cloexec(signal_pipe[1], 1);

    api = a;
    io_event = api->io_new(api, signal_pipe[0], PA_IO_EVENT_INPUT, callback, NULL);
    assert(io_event);
    return 0;
}

void pa_signal_done(void) {
    assert(api && signal_pipe[0] >= 0 && signal_pipe[1] >= 0 && io_event);

    while (signals)
        pa_signal_free(signals);


        api->io_free(io_event);
    io_event = NULL;

    close(signal_pipe[0]);
    close(signal_pipe[1]);
    signal_pipe[0] = signal_pipe[1] = -1;

    api = NULL;
}

struct pa_signal_event* pa_signal_new(int sig, void (*callback) (struct pa_mainloop_api *api, struct pa_signal_event*e, int sig, void *userdata), void *userdata) {
    struct pa_signal_event *e = NULL;
    struct sigaction sa;
    assert(sig > 0 && callback);
    
    for (e = signals; e; e = e->next)
        if (e->sig == sig)
            goto fail;
    
    e = pa_xmalloc(sizeof(struct pa_signal_event));
    e->sig = sig;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(sig, &sa, &e->saved_sigaction) < 0)
        goto fail;

    e->previous = NULL;
    e->next = signals;
    signals = e;

    return e;
fail:
    if (e)
        pa_xfree(e);
    return NULL;
}

void pa_signal_free(struct pa_signal_event *e) {
    assert(e);

    if (e->next)
        e->next->previous = e->previous;
    if (e->previous)
        e->previous->next = e->next;
    else
        signals = e->next;

    sigaction(e->sig, &e->saved_sigaction, NULL);

    if (e->destroy_callback)
        e->destroy_callback(api, e, e->userdata);
    
    pa_xfree(e);
}

void pa_signal_set_destroy(struct pa_signal_event *e, void (*callback) (struct pa_mainloop_api *api, struct pa_signal_event*e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}
