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
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#else
#include "../polypcore/poll.h"
#endif

#include "../polypcore/winsock.h"

#ifndef HAVE_PIPE
#include "../polypcore/pipe.h"
#endif

#include <polyp/timeval.h>
#include <polyp/xmalloc.h>

#include <polypcore/core-util.h>
#include <polypcore/idxset.h>
#include <polypcore/log.h>

#include "mainloop.h"

struct pa_io_event {
    pa_mainloop *mainloop;
    int dead;
    int fd;
    pa_io_event_flags_t events;
    void (*callback) (pa_mainloop_api*a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata);
    struct pollfd *pollfd;
    void *userdata;
    void (*destroy_callback) (pa_mainloop_api*a, pa_io_event *e, void *userdata);
};

struct pa_time_event {
    pa_mainloop *mainloop;
    int dead;
    int enabled;
    struct timeval timeval;
    void (*callback)(pa_mainloop_api*a, pa_time_event *e, const struct timeval*tv, void *userdata);
    void *userdata;
    void (*destroy_callback) (pa_mainloop_api*a, pa_time_event *e, void *userdata);
};

struct pa_defer_event {
    pa_mainloop *mainloop;
    int dead;
    int enabled;
    void (*callback)(pa_mainloop_api*a, pa_defer_event*e, void *userdata);
    void *userdata;
    void (*destroy_callback) (pa_mainloop_api*a, pa_defer_event *e, void *userdata);
};

struct pa_mainloop {
    pa_idxset *io_events, *time_events, *defer_events;
    int io_events_scan_dead, defer_events_scan_dead, time_events_scan_dead;

    struct pollfd *pollfds;
    unsigned max_pollfds, n_pollfds;
    int rebuild_pollfds;

    int prepared_timeout;

    int quit, retval;
    pa_mainloop_api api;

    int deferred_pending;

    int wakeup_pipe[2];

    enum {
        STATE_PASSIVE,
        STATE_PREPARED,
        STATE_POLLING,
        STATE_POLLED,
        STATE_QUIT
    } state;

    pa_poll_func poll_func;
    void *poll_func_userdata;
};

/* IO events */
static pa_io_event* mainloop_io_new(
    pa_mainloop_api*a,
    int fd,
    pa_io_event_flags_t events,
    void (*callback) (pa_mainloop_api*a, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata),
    void *userdata) {
    
    pa_mainloop *m;
    pa_io_event *e;

    assert(a && a->userdata && fd >= 0 && callback);
    m = a->userdata;
    assert(a == &m->api);

    e = pa_xmalloc(sizeof(pa_io_event));
    e->mainloop = m;
    e->dead = 0;

    e->fd = fd;
    e->events = events;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;
    e->pollfd = NULL;

#ifdef OS_IS_WIN32
    {
        fd_set xset;
        struct timeval tv;

        tv.tv_sec = 0;
        tv.tv_usec = 0;

        FD_ZERO (&xset);
        FD_SET (fd, &xset);

        if ((select((SELECT_TYPE_ARG1) fd, NULL, NULL, SELECT_TYPE_ARG234 &xset,
                    SELECT_TYPE_ARG5 &tv) == -1) &&
             (WSAGetLastError() == WSAENOTSOCK)) {
            pa_log_warn(__FILE__": WARNING: cannot monitor non-socket file descriptors.");
            e->dead = 1;
        }
    }
#endif

    pa_idxset_put(m->io_events, e, NULL);
    m->rebuild_pollfds = 1;

    pa_mainloop_wakeup(m);

    return e;
}

static void mainloop_io_enable(pa_io_event *e, pa_io_event_flags_t events) {
    assert(e && e->mainloop);

    e->events = events;
    e->mainloop->rebuild_pollfds = 1;

    pa_mainloop_wakeup(e->mainloop);
}

static void mainloop_io_free(pa_io_event *e) {
    assert(e && e->mainloop);

    e->dead = e->mainloop->io_events_scan_dead = e->mainloop->rebuild_pollfds = 1;

    pa_mainloop_wakeup(e->mainloop);
}

static void mainloop_io_set_destroy(pa_io_event *e, void (*callback)(pa_mainloop_api*a, pa_io_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* Defer events */
static pa_defer_event* mainloop_defer_new(pa_mainloop_api*a, void (*callback) (pa_mainloop_api*a, pa_defer_event *e, void *userdata), void *userdata) {
    pa_mainloop *m;
    pa_defer_event *e;

    assert(a && a->userdata && callback);
    m = a->userdata;
    assert(a == &m->api);

    e = pa_xmalloc(sizeof(pa_defer_event));
    e->mainloop = m;
    e->dead = 0;

    e->enabled = 1;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;

    pa_idxset_put(m->defer_events, e, NULL);

    m->deferred_pending++;

    pa_mainloop_wakeup(e->mainloop);

    return e;
}

static void mainloop_defer_enable(pa_defer_event *e, int b) {
    assert(e);

    if (e->enabled && !b) {
        assert(e->mainloop->deferred_pending > 0);
        e->mainloop->deferred_pending--;
    } else if (!e->enabled && b) {
        e->mainloop->deferred_pending++;
        pa_mainloop_wakeup(e->mainloop);
    }
    
    e->enabled = b;
}

static void mainloop_defer_free(pa_defer_event *e) {
    assert(e);
    e->dead = e->mainloop->defer_events_scan_dead = 1;

    if (e->enabled) {
        e->enabled = 0;
        assert(e->mainloop->deferred_pending > 0);
        e->mainloop->deferred_pending--;
    }
}

static void mainloop_defer_set_destroy(pa_defer_event *e, void (*callback)(pa_mainloop_api*a, pa_defer_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* Time events */
static pa_time_event* mainloop_time_new(pa_mainloop_api*a, const struct timeval *tv, void (*callback) (pa_mainloop_api*a, pa_time_event*e, const struct timeval *tv, void *userdata), void *userdata) {
    pa_mainloop *m;
    pa_time_event *e;

    assert(a && a->userdata && callback);
    m = a->userdata;
    assert(a == &m->api);

    e = pa_xmalloc(sizeof(pa_time_event));
    e->mainloop = m;
    e->dead = 0;

    e->enabled = !!tv;
    if (tv)
        e->timeval = *tv;

    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;

    pa_idxset_put(m->time_events, e, NULL);

    if (e->enabled)
        pa_mainloop_wakeup(m);
    
    return e;
}

static void mainloop_time_restart(pa_time_event *e, const struct timeval *tv) {
    assert(e);

    if (tv) {
        e->enabled = 1;
        e->timeval = *tv;

        pa_mainloop_wakeup(e->mainloop);
    } else
        e->enabled = 0;
}

static void mainloop_time_free(pa_time_event *e) {
    assert(e);

    e->dead = e->mainloop->time_events_scan_dead = 1;

    /* no wakeup needed here. Think about it! */
}

static void mainloop_time_set_destroy(pa_time_event *e, void (*callback)(pa_mainloop_api*a, pa_time_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* quit() */

static void mainloop_quit(pa_mainloop_api*a, int retval) {
    pa_mainloop *m;
    assert(a && a->userdata);
    m = a->userdata;
    assert(a == &m->api);

    pa_mainloop_quit(m, retval);
}
    
static const pa_mainloop_api vtable = {
    .userdata = NULL,

    .io_new= mainloop_io_new,
    .io_enable= mainloop_io_enable,
    .io_free= mainloop_io_free,
    .io_set_destroy= mainloop_io_set_destroy,

    .time_new = mainloop_time_new,
    .time_restart = mainloop_time_restart,
    .time_free = mainloop_time_free,
    .time_set_destroy = mainloop_time_set_destroy,
    
    .defer_new = mainloop_defer_new,
    .defer_enable = mainloop_defer_enable,
    .defer_free = mainloop_defer_free,
    .defer_set_destroy = mainloop_defer_set_destroy,
    
    .quit = mainloop_quit,
};

pa_mainloop *pa_mainloop_new(void) {
    pa_mainloop *m;

    m = pa_xmalloc(sizeof(pa_mainloop));

    if (pipe(m->wakeup_pipe) < 0) {
        pa_log_error(__FILE__": ERROR: cannot create wakeup pipe");
        pa_xfree(m);
        return NULL;
    }

    pa_make_nonblock_fd(m->wakeup_pipe[0]);
    pa_make_nonblock_fd(m->wakeup_pipe[1]);

    m->io_events = pa_idxset_new(NULL, NULL);
    m->defer_events = pa_idxset_new(NULL, NULL);
    m->time_events = pa_idxset_new(NULL, NULL);

    assert(m->io_events && m->defer_events && m->time_events);

    m->io_events_scan_dead = m->defer_events_scan_dead = m->time_events_scan_dead = 0;
    
    m->pollfds = NULL;
    m->max_pollfds = m->n_pollfds = 0;
    m->rebuild_pollfds = 1;

    m->quit = m->retval = 0;

    m->api = vtable;
    m->api.userdata = m;

    m->deferred_pending = 0;

    m->state = STATE_PASSIVE;

    m->poll_func = NULL;
    m->poll_func_userdata = NULL;

    m->retval = -1;
    
    return m;
}

static int io_foreach(void *p, uint32_t PA_GCC_UNUSED idx, int *del, void*userdata) {
    pa_io_event *e = p;
    int *all = userdata;
    assert(e && del && all);

    if (!*all && !e->dead)
        return 0;
    
    if (e->destroy_callback)
        e->destroy_callback(&e->mainloop->api, e, e->userdata);
    pa_xfree(e);
    *del = 1;
    return 0;
}

static int time_foreach(void *p, uint32_t PA_GCC_UNUSED idx, int *del, void*userdata) {
    pa_time_event *e = p;
    int *all = userdata;
    assert(e && del && all);

    if (!*all && !e->dead)
        return 0;
    
    if (e->destroy_callback)
        e->destroy_callback(&e->mainloop->api, e, e->userdata);
    pa_xfree(e);
    *del = 1;
    return 0;
}

static int defer_foreach(void *p, PA_GCC_UNUSED uint32_t idx, int *del, void*userdata) {
    pa_defer_event *e = p;
    int *all = userdata;
    assert(e && del && all);

    if (!*all && !e->dead)
        return 0;
    
    if (e->destroy_callback)
        e->destroy_callback(&e->mainloop->api, e, e->userdata);
    pa_xfree(e);
    *del = 1;
    return 0;
}

void pa_mainloop_free(pa_mainloop* m) {
    int all = 1;
    assert(m);

    pa_idxset_foreach(m->io_events, io_foreach, &all);
    pa_idxset_foreach(m->time_events, time_foreach, &all);
    pa_idxset_foreach(m->defer_events, defer_foreach, &all);

    pa_idxset_free(m->io_events, NULL, NULL);
    pa_idxset_free(m->time_events, NULL, NULL);
    pa_idxset_free(m->defer_events, NULL, NULL);

    pa_xfree(m->pollfds);

    if (m->wakeup_pipe[0] >= 0)
        close(m->wakeup_pipe[0]);
    if (m->wakeup_pipe[1] >= 0)
        close(m->wakeup_pipe[1]);

    pa_xfree(m);
}

static void scan_dead(pa_mainloop *m) {
    int all = 0;
    assert(m);

    if (m->io_events_scan_dead)
        pa_idxset_foreach(m->io_events, io_foreach, &all);
    if (m->time_events_scan_dead)
        pa_idxset_foreach(m->time_events, time_foreach, &all);
    if (m->defer_events_scan_dead)
        pa_idxset_foreach(m->defer_events, defer_foreach, &all);

    m->io_events_scan_dead = m->time_events_scan_dead = m->defer_events_scan_dead = 0;
}

static void rebuild_pollfds(pa_mainloop *m) {
    pa_io_event*e;
    struct pollfd *p;
    uint32_t idx = PA_IDXSET_INVALID;
    unsigned l;

    l = pa_idxset_size(m->io_events) + 1;
    if (m->max_pollfds < l) {
        m->pollfds = pa_xrealloc(m->pollfds, sizeof(struct pollfd)*l);
        m->max_pollfds = l;
    }

    m->n_pollfds = 0;
    p = m->pollfds;

    if (m->wakeup_pipe[0] >= 0) {
        m->pollfds[0].fd = m->wakeup_pipe[0];
        m->pollfds[0].events = POLLIN;
        m->pollfds[0].revents = 0;
        p++;
        m->n_pollfds++;
    }

    for (e = pa_idxset_first(m->io_events, &idx); e; e = pa_idxset_next(m->io_events, &idx)) {
        if (e->dead) {
            e->pollfd = NULL;
            continue;
        }

        e->pollfd = p;
        p->fd = e->fd;
        p->events =
            ((e->events & PA_IO_EVENT_INPUT) ? POLLIN : 0) |
            ((e->events & PA_IO_EVENT_OUTPUT) ? POLLOUT : 0) |
            POLLHUP |
            POLLERR;
        p->revents = 0;

        p++;
        m->n_pollfds++;
    }

    m->rebuild_pollfds = 0;
}

static int dispatch_pollfds(pa_mainloop *m) {
    uint32_t idx = PA_IDXSET_INVALID;
    pa_io_event *e;
    int r = 0;

    for (e = pa_idxset_first(m->io_events, &idx); e && !m->quit; e = pa_idxset_next(m->io_events, &idx)) {
        if (e->dead || !e->pollfd || !e->pollfd->revents)
            continue;
        
        assert(e->pollfd->fd == e->fd && e->callback);
        e->callback(&m->api, e, e->fd,
                    (e->pollfd->revents & POLLHUP ? PA_IO_EVENT_HANGUP : 0) |
                    (e->pollfd->revents & POLLIN ? PA_IO_EVENT_INPUT : 0) |
                    (e->pollfd->revents & POLLOUT ? PA_IO_EVENT_OUTPUT : 0) |
                    (e->pollfd->revents & POLLERR ? PA_IO_EVENT_ERROR : 0),
                    e->userdata);
        e->pollfd->revents = 0;
        r++;
    }

    return r;
}

static int dispatch_defer(pa_mainloop *m) {
    uint32_t idx;
    pa_defer_event *e;
    int r = 0;

    if (!m->deferred_pending)
        return 0;

    for (e = pa_idxset_first(m->defer_events, &idx); e && !m->quit; e = pa_idxset_next(m->defer_events, &idx)) {
        if (e->dead || !e->enabled)
            continue;
 
        assert(e->callback);
        e->callback(&m->api, e, e->userdata);
        r++;
    }

    return r;
}

static int calc_next_timeout(pa_mainloop *m) {
    uint32_t idx;
    pa_time_event *e;
    struct timeval now;
    int t = -1;
    int got_time = 0;

    if (pa_idxset_isempty(m->time_events))
        return -1;

    for (e = pa_idxset_first(m->time_events, &idx); e; e = pa_idxset_next(m->time_events, &idx)) {
        int tmp;
        
        if (e->dead || !e->enabled)
            continue;

        /* Let's save a system call */
        if (!got_time) {
            pa_gettimeofday(&now);
            got_time = 1;
        }

        if (e->timeval.tv_sec < now.tv_sec || (e->timeval.tv_sec == now.tv_sec && e->timeval.tv_usec <= now.tv_usec)) 
            return 0;

        tmp = (e->timeval.tv_sec - now.tv_sec)*1000;
            
        if (e->timeval.tv_usec > now.tv_usec)
            tmp += (e->timeval.tv_usec - now.tv_usec)/1000;
        else
            tmp -= (now.tv_usec - e->timeval.tv_usec)/1000;

        if (tmp == 0)
            return 0;
        else if (t == -1 || tmp < t)
            t = tmp;
    }

    return t;
}

static int dispatch_timeout(pa_mainloop *m) {
    uint32_t idx;
    pa_time_event *e;
    struct timeval now;
    int got_time = 0;
    int r = 0;
    assert(m);

    if (pa_idxset_isempty(m->time_events))
        return 0;

    for (e = pa_idxset_first(m->time_events, &idx); e && !m->quit; e = pa_idxset_next(m->time_events, &idx)) {
        
        if (e->dead || !e->enabled)
            continue;

        /* Let's save a system call */
        if (!got_time) {
            pa_gettimeofday(&now);
            got_time = 1;
        }
        
        if (e->timeval.tv_sec < now.tv_sec || (e->timeval.tv_sec == now.tv_sec && e->timeval.tv_usec <= now.tv_usec)) {
            assert(e->callback);

            e->enabled = 0;
            e->callback(&m->api, e, &e->timeval, e->userdata);

            r++;
        }
    }

    return r;
}

void pa_mainloop_wakeup(pa_mainloop *m) {
    char c = 'W';
    assert(m);

    if (m->wakeup_pipe[1] >= 0)
        pa_write(m->wakeup_pipe[1], &c, sizeof(c));
}

static void clear_wakeup(pa_mainloop *m) {
    char c[10];

    assert(m);

    if (m->wakeup_pipe[0] < 0)
        return;

    while (pa_read(m->wakeup_pipe[0], &c, sizeof(c)) == sizeof(c));
}

int pa_mainloop_prepare(pa_mainloop *m, int timeout) {
    assert(m);
    assert(m->state == STATE_PASSIVE);

    clear_wakeup(m);
    scan_dead(m);

    if (m->quit)
        goto quit;

    if (!m->deferred_pending) {
    
        if (m->rebuild_pollfds)
            rebuild_pollfds(m);
        
        m->prepared_timeout = calc_next_timeout(m);
        if (timeout >= 0 && (timeout < m->prepared_timeout || m->prepared_timeout < 0))
            m->prepared_timeout = timeout;
    }

    m->state = STATE_PREPARED;
    return 0;

quit:
    m->state = STATE_QUIT;
    return -2;
}

int pa_mainloop_poll(pa_mainloop *m) {
    int r;

    assert(m);
    assert(m->state == STATE_PREPARED);

    if (m->quit)
        goto quit;

    m->state = STATE_POLLING;

    if (m->deferred_pending)
        r = 0;
    else {
        if (m->poll_func)
            r = m->poll_func(m->pollfds, m->n_pollfds, m->prepared_timeout, m->poll_func_userdata);
        else
            r = poll(m->pollfds, m->n_pollfds, m->prepared_timeout);

        if (r < 0) {
            if (errno == EINTR)
                r = 0;
            else
                pa_log(__FILE__": poll(): %s", strerror(errno));
        }
    }

    m->state = r < 0 ? STATE_PASSIVE : STATE_POLLED;
    return r;

quit:
    m->state = STATE_QUIT;
    return -2;
}

int pa_mainloop_dispatch(pa_mainloop *m) {
    int dispatched = 0;

    assert(m);
    assert(m->state == STATE_POLLED);

    if (m->quit)
        goto quit;
    
    if (m->deferred_pending)
        dispatched += dispatch_defer(m);
    else {
        dispatched += dispatch_timeout(m);
        
        if (m->quit)
            goto quit;
        
        dispatched += dispatch_pollfds(m);

    }
    
    if (m->quit)
        goto quit;
    
    m->state = STATE_PASSIVE;

    return dispatched;

quit:
    m->state = STATE_QUIT;
    return -2;
}

int pa_mainloop_get_retval(pa_mainloop *m) {
    assert(m);
    return m->retval;
}

int pa_mainloop_iterate(pa_mainloop *m, int block, int *retval) {
    int r;
    assert(m);

    if ((r = pa_mainloop_prepare(m, block ? -1 : 0)) < 0)
        goto quit;

    if ((r = pa_mainloop_poll(m)) < 0)
        goto quit;

    if ((r = pa_mainloop_dispatch(m)) < 0)
        goto quit;

    return r;

quit:
    
    if ((r == -2) && retval)
        *retval = pa_mainloop_get_retval(m);
    return r;
}

int pa_mainloop_run(pa_mainloop *m, int *retval) {
    int r;
    
    while ((r = pa_mainloop_iterate(m, 1, retval)) >= 0);

    if (r == -2)
        return 1;
    else if (r < 0)
        return -1;
    else
        return 0;
}

void pa_mainloop_quit(pa_mainloop *m, int retval) {
    assert(m);

    m->quit = 1;
    m->retval = retval;
    pa_mainloop_wakeup(m);
}

pa_mainloop_api* pa_mainloop_get_api(pa_mainloop*m) {
    assert(m);
    return &m->api;
}

void pa_mainloop_set_poll_func(pa_mainloop *m, pa_poll_func poll_func, void *userdata) {
    assert(m);

    m->poll_func = poll_func;
    m->poll_func_userdata = userdata;
}


#if 0
void pa_mainloop_dump(pa_mainloop *m) {
    assert(m);

    pa_log(__FILE__": Dumping mainloop sources START");
    
    {
        uint32_t idx = PA_IDXSET_INVALID;
        pa_io_event *e;
        for (e = pa_idxset_first(m->io_events, &idx); e; e = pa_idxset_next(m->io_events, &idx)) {
            if (e->dead)
                continue;
            
            pa_log(__FILE__": kind=io fd=%i events=%i callback=%p userdata=%p", e->fd, (int) e->events, (void*) e->callback, (void*) e->userdata);
        }
    }
    {
        uint32_t idx = PA_IDXSET_INVALID;
        pa_defer_event *e;
        for (e = pa_idxset_first(m->defer_events, &idx); e; e = pa_idxset_next(m->defer_events, &idx)) {
            if (e->dead)
                continue;
            
            pa_log(__FILE__": kind=defer enabled=%i callback=%p userdata=%p", e->enabled, (void*) e->callback, (void*) e->userdata);
        }
    }
    {
        uint32_t idx = PA_IDXSET_INVALID;
        pa_time_event *e;
        for (e = pa_idxset_first(m->time_events, &idx); e; e = pa_idxset_next(m->time_events, &idx)) {
            if (e->dead)
                continue;
            
            pa_log(__FILE__": kind=time enabled=%i time=%lu.%lu callback=%p userdata=%p", e->enabled, (unsigned long) e->timeval.tv_sec, (unsigned long) e->timeval.tv_usec, (void*) e->callback, (void*) e->userdata);
        }
    }

    pa_log(__FILE__": Dumping mainloop sources STOP");

}
#endif 
