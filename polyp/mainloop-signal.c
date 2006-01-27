/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
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
#include <fcntl.h>

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include "mainloop-signal.h"
#include "util.h"
#include "xmalloc.h"
#include "log.h"
#include "gccmacro.h"

struct pa_signal_event {
    int sig;
#ifdef HAVE_SIGACTION
    struct sigaction saved_sigaction;
#else
    void (*saved_handler)(int sig);
#endif
    void (*callback) (pa_mainloop_api*a, pa_signal_event *e, int sig, void *userdata);
    void *userdata;
    void (*destroy_callback) (pa_mainloop_api*a, pa_signal_event*e, void *userdata);
    pa_signal_event *previous, *next;
};

static pa_mainloop_api *api = NULL;
static int signal_pipe[2] = { -1, -1 };
static pa_io_event* io_event = NULL;
static pa_defer_event *defer_event = NULL;
static pa_signal_event *signals = NULL;

#ifdef OS_IS_WIN32
static unsigned int waiting_signals = 0;
static CRITICAL_SECTION crit;
#endif

static void signal_handler(int sig) {
#ifndef HAVE_SIGACTION
    signal(sig, signal_handler);
#endif
    write(signal_pipe[1], &sig, sizeof(sig));

#ifdef OS_IS_WIN32
    EnterCriticalSection(&crit);
    waiting_signals++;
    LeaveCriticalSection(&crit);
#endif
}

static void dispatch(pa_mainloop_api*a, int sig) {
    pa_signal_event*s;

    for (s = signals; s; s = s->next) 
        if (s->sig == sig) {
            assert(s->callback);
            s->callback(a, s, sig, s->userdata);
            break;
        }
}

static void defer(pa_mainloop_api*a, PA_GCC_UNUSED pa_defer_event*e, PA_GCC_UNUSED void *userdata) {
    ssize_t r;
    int sig;
    unsigned int sigs;

#ifdef OS_IS_WIN32
    EnterCriticalSection(&crit);
    sigs = waiting_signals;
    waiting_signals = 0;
    LeaveCriticalSection(&crit);
#endif

    while (sigs) {
        if ((r = read(signal_pipe[0], &sig, sizeof(sig))) < 0) {
            pa_log(__FILE__": read(): %s\n", strerror(errno));
            return;
        }
        
        if (r != sizeof(sig)) {
            pa_log(__FILE__": short read()\n");
            return;
        }

        dispatch(a, sig);

        sigs--;
    }
}

static void callback(pa_mainloop_api*a, pa_io_event*e, int fd, pa_io_event_flags_t f, PA_GCC_UNUSED void *userdata) {
    ssize_t r;
    int sig;
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

    dispatch(a, sig);
}

int pa_signal_init(pa_mainloop_api *a) {
    assert(!api && a && signal_pipe[0] == -1 && signal_pipe[1] == -1 && !io_event && !defer_event);

#ifdef OS_IS_WIN32
    if (_pipe(signal_pipe, 200, _O_BINARY) < 0) {
#else
    if (pipe(signal_pipe) < 0) {
#endif
        pa_log(__FILE__": pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    pa_make_nonblock_fd(signal_pipe[0]);
    pa_make_nonblock_fd(signal_pipe[1]);
    pa_fd_set_cloexec(signal_pipe[0], 1);
    pa_fd_set_cloexec(signal_pipe[1], 1);

    api = a;

#ifndef OS_IS_WIN32
    io_event = api->io_new(api, signal_pipe[0], PA_IO_EVENT_INPUT, callback, NULL);
    assert(io_event);
#else
    defer_event = api->defer_new(api, defer, NULL);
    assert(defer_event);

    InitializeCriticalSection(&crit);
#endif

    return 0;
}

void pa_signal_done(void) {
    assert(api && signal_pipe[0] >= 0 && signal_pipe[1] >= 0 && (io_event || defer_event));

    while (signals)
        pa_signal_free(signals);


#ifndef OS_IS_WIN32
    api->io_free(io_event);
    io_event = NULL;
#else
    api->defer_free(defer_event);
    defer_event = NULL;

    DeleteCriticalSection(&crit);
#endif

    close(signal_pipe[0]);
    close(signal_pipe[1]);
    signal_pipe[0] = signal_pipe[1] = -1;

    api = NULL;
}

pa_signal_event* pa_signal_new(int sig, void (*_callback) (pa_mainloop_api *api, pa_signal_event*e, int sig, void *userdata), void *userdata) {
    pa_signal_event *e = NULL;

#ifdef HAVE_SIGACTION
    struct sigaction sa;
#endif

    assert(sig > 0 && _callback);
    
    for (e = signals; e; e = e->next)
        if (e->sig == sig)
            goto fail;
    
    e = pa_xmalloc(sizeof(pa_signal_event));
    e->sig = sig;
    e->callback = _callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;

#ifdef HAVE_SIGACTION
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(sig, &sa, &e->saved_sigaction) < 0)
#else
    if ((e->saved_handler = signal(sig, signal_handler)) == SIG_ERR)
#endif
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

void pa_signal_free(pa_signal_event *e) {
    assert(e);

    if (e->next)
        e->next->previous = e->previous;
    if (e->previous)
        e->previous->next = e->next;
    else
        signals = e->next;

#ifdef HAVE_SIGACTION
    sigaction(e->sig, &e->saved_sigaction, NULL);
#else
    signal(e->sig, e->saved_handler);
#endif

    if (e->destroy_callback)
        e->destroy_callback(api, e, e->userdata);
    
    pa_xfree(e);
}

void pa_signal_set_destroy(pa_signal_event *e, void (*_callback) (pa_mainloop_api *api, pa_signal_event*e, void *userdata)) {
    assert(e);
    e->destroy_callback = _callback;
}
