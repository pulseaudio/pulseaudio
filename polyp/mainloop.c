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
#include <signal.h>
#include <unistd.h>
#include <sys/poll.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>

#include "mainloop.h"
#include "util.h"
#include "idxset.h"
#include "xmalloc.h"
#include "log.h"

struct pa_io_event {
    struct pa_mainloop *mainloop;
    int dead;
    int fd;
    enum pa_io_event_flags events;
    void (*callback) (struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata);
    struct pollfd *pollfd;
    void *userdata;
    void (*destroy_callback) (struct pa_mainloop_api*a, struct pa_io_event *e, void *userdata);
};

struct pa_time_event {
    struct pa_mainloop *mainloop;
    int dead;
    int enabled;
    struct timeval timeval;
    void (*callback)(struct pa_mainloop_api*a, struct pa_time_event *e, const struct timeval*tv, void *userdata);
    void *userdata;
    void (*destroy_callback) (struct pa_mainloop_api*a, struct pa_time_event *e, void *userdata);
};

struct pa_defer_event {
    struct pa_mainloop *mainloop;
    int dead;
    int enabled;
    void (*callback)(struct pa_mainloop_api*a, struct pa_defer_event*e, void *userdata);
    void *userdata;
    void (*destroy_callback) (struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata);
};

struct pa_mainloop {
    struct pa_idxset *io_events, *time_events, *defer_events;
    int io_events_scan_dead, defer_events_scan_dead, time_events_scan_dead;

    struct pollfd *pollfds;
    unsigned max_pollfds, n_pollfds;
    int rebuild_pollfds;

    int quit, running, retval;
    struct pa_mainloop_api api;
};

/* IO events */
static struct pa_io_event* mainloop_io_new(struct pa_mainloop_api*a, int fd, enum pa_io_event_flags events, void (*callback) (struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags events, void *userdata), void *userdata) {
    struct pa_mainloop *m;
    struct pa_io_event *e;

    assert(a && a->userdata && fd >= 0 && callback);
    m = a->userdata;
    assert(a == &m->api);

    e = pa_xmalloc(sizeof(struct pa_io_event));
    e->mainloop = m;
    e->dead = 0;

    e->fd = fd;
    e->events = events;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;
    e->pollfd = NULL;

    pa_idxset_put(m->io_events, e, NULL);
    m->rebuild_pollfds = 1;
    return e;
}

static void mainloop_io_enable(struct pa_io_event *e, enum pa_io_event_flags events) {
    assert(e && e->mainloop);

    e->events = events;
    if (e->pollfd)
        e->pollfd->events =
            (events & PA_IO_EVENT_INPUT ? POLLIN : 0) |
            (events & PA_IO_EVENT_OUTPUT ? POLLOUT : 0) |
            POLLHUP |
            POLLERR;
}

static void mainloop_io_free(struct pa_io_event *e) {
    assert(e && e->mainloop);
    e->dead = e->mainloop->io_events_scan_dead = e->mainloop->rebuild_pollfds = 1;
}

static void mainloop_io_set_destroy(struct pa_io_event *e, void (*callback)(struct pa_mainloop_api*a, struct pa_io_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* Defer events */
struct pa_defer_event* mainloop_defer_new(struct pa_mainloop_api*a, void (*callback) (struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata), void *userdata) {
    struct pa_mainloop *m;
    struct pa_defer_event *e;

    assert(a && a->userdata && callback);
    m = a->userdata;
    assert(a == &m->api);

    e = pa_xmalloc(sizeof(struct pa_defer_event));
    e->mainloop = m;
    e->dead = 0;

    e->enabled = 1;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;

    pa_idxset_put(m->defer_events, e, NULL);
    return e;
}

static void mainloop_defer_enable(struct pa_defer_event *e, int b) {
    assert(e);
    e->enabled = b;
}

static void mainloop_defer_free(struct pa_defer_event *e) {
    assert(e);
    e->dead = e->mainloop->defer_events_scan_dead = 1;
}

static void mainloop_defer_set_destroy(struct pa_defer_event *e, void (*callback)(struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* Time events */
static struct pa_time_event* mainloop_time_new(struct pa_mainloop_api*a, const struct timeval *tv, void (*callback) (struct pa_mainloop_api*a, struct pa_time_event*e, const struct timeval *tv, void *userdata), void *userdata) {
    struct pa_mainloop *m;
    struct pa_time_event *e;

    assert(a && a->userdata && callback);
    m = a->userdata;
    assert(a == &m->api);

    e = pa_xmalloc(sizeof(struct pa_time_event));
    e->mainloop = m;
    e->dead = 0;

    e->enabled = !!tv;
    if (tv)
        e->timeval = *tv;

    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;

    pa_idxset_put(m->time_events, e, NULL);
    
    return e;
}

static void mainloop_time_restart(struct pa_time_event *e, const struct timeval *tv) {
    assert(e);

    if (tv) {
        e->enabled = 1;
        e->timeval = *tv;
    } else
        e->enabled = 0;
}

static void mainloop_time_free(struct pa_time_event *e) {
    assert(e);

    e->dead = e->mainloop->time_events_scan_dead = 1;
}

static void mainloop_time_set_destroy(struct pa_time_event *e, void (*callback)(struct pa_mainloop_api*a, struct pa_time_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* quit() */

static void mainloop_quit(struct pa_mainloop_api*a, int retval) {
    struct pa_mainloop *m;
    assert(a && a->userdata);
    m = a->userdata;
    assert(a == &m->api);

    m->quit = 1;
    m->retval = retval;
}
    
static const struct pa_mainloop_api vtable = {
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

struct pa_mainloop *pa_mainloop_new(void) {
    struct pa_mainloop *m;

    m = pa_xmalloc(sizeof(struct pa_mainloop));

    m->io_events = pa_idxset_new(NULL, NULL);
    m->defer_events = pa_idxset_new(NULL, NULL);
    m->time_events = pa_idxset_new(NULL, NULL);

    assert(m->io_events && m->defer_events && m->time_events);

    m->io_events_scan_dead = m->defer_events_scan_dead = m->time_events_scan_dead = 0;
    
    m->pollfds = NULL;
    m->max_pollfds = m->n_pollfds = m->rebuild_pollfds = 0;

    m->quit = m->running = m->retval = 0;

    m->api = vtable;
    m->api.userdata = m;
    
    return m;
}

static int io_foreach(void *p, uint32_t index, int *del, void*userdata) {
    struct pa_io_event *e = p;
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

static int time_foreach(void *p, uint32_t index, int *del, void*userdata) {
    struct pa_time_event *e = p;
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

static int defer_foreach(void *p, uint32_t index, int *del, void*userdata) {
    struct pa_defer_event *e = p;
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

void pa_mainloop_free(struct pa_mainloop* m) {
    int all = 1;
    assert(m);

    pa_idxset_foreach(m->io_events, io_foreach, &all);
    pa_idxset_foreach(m->time_events, time_foreach, &all);
    pa_idxset_foreach(m->defer_events, defer_foreach, &all);

    pa_idxset_free(m->io_events, NULL, NULL);
    pa_idxset_free(m->time_events, NULL, NULL);
    pa_idxset_free(m->defer_events, NULL, NULL);

    pa_xfree(m->pollfds);
    pa_xfree(m);
}

static void scan_dead(struct pa_mainloop *m) {
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

static void rebuild_pollfds(struct pa_mainloop *m) {
    struct pa_io_event*e;
    struct pollfd *p;
    uint32_t index = PA_IDXSET_INVALID;
    unsigned l;

    l = pa_idxset_ncontents(m->io_events);
    if (m->max_pollfds < l) {
        m->pollfds = pa_xrealloc(m->pollfds, sizeof(struct pollfd)*l);
        m->max_pollfds = l;
    }

    m->n_pollfds = 0;
    p = m->pollfds;
    for (e = pa_idxset_first(m->io_events, &index); e; e = pa_idxset_next(m->io_events, &index)) {
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
}

static void dispatch_pollfds(struct pa_mainloop *m) {
    uint32_t index = PA_IDXSET_INVALID;
    struct pa_io_event *e;

    for (e = pa_idxset_first(m->io_events, &index); e; e = pa_idxset_next(m->io_events, &index)) {
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
    }
}

static void dispatch_defer(struct pa_mainloop *m) {
    uint32_t index;
    struct pa_defer_event *e;

    for (e = pa_idxset_first(m->defer_events, &index); e; e = pa_idxset_next(m->defer_events, &index)) {
        if (e->dead || !e->enabled)
            continue;
 
        assert(e->callback);
        e->callback(&m->api, e, e->userdata);
    }
}

static int calc_next_timeout(struct pa_mainloop *m) {
    uint32_t index;
    struct pa_time_event *e;
    struct timeval now;
    int t = -1;
    int got_time = 0;

    if (pa_idxset_isempty(m->time_events))
        return -1;

    for (e = pa_idxset_first(m->time_events, &index); e; e = pa_idxset_next(m->time_events, &index)) {
        int tmp;
        
        if (e->dead || !e->enabled)
            continue;

        /* Let's save a system call */
        if (!got_time) {
            gettimeofday(&now, NULL);
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

static void dispatch_timeout(struct pa_mainloop *m) {
    uint32_t index;
    struct pa_time_event *e;
    struct timeval now;
    int got_time = 0;
    assert(m);

    if (pa_idxset_isempty(m->time_events))
        return;

    for (e = pa_idxset_first(m->time_events, &index); e; e = pa_idxset_next(m->time_events, &index)) {
        
        if (e->dead || !e->enabled)
            continue;

        /* Let's save a system call */
        if (!got_time) {
            gettimeofday(&now, NULL);
            got_time = 1;
        }
        
        if (e->timeval.tv_sec < now.tv_sec || (e->timeval.tv_sec == now.tv_sec && e->timeval.tv_usec <= now.tv_usec)) {
            assert(e->callback);

            e->enabled = 0;
            e->callback(&m->api, e, &e->timeval, e->userdata);
        }
    }
}

int pa_mainloop_iterate(struct pa_mainloop *m, int block, int *retval) {
    int r;
    assert(m && !m->running);
    
    if(m->quit) {
        if (retval)
            *retval = m->retval;
        return 1;
    }

    m->running = 1;

    scan_dead(m);
    dispatch_defer(m);

    if (m->rebuild_pollfds) {
        rebuild_pollfds(m);
        m->rebuild_pollfds = 0;
    }

    do {
        int t = block ? calc_next_timeout(m) : 0;
        /*pa_log(__FILE__": %u\n", t);*/
        r = poll(m->pollfds, m->n_pollfds, t);
    } while (r < 0 && errno == EINTR);

    dispatch_timeout(m);
    
    if (r > 0)
        dispatch_pollfds(m);
    else if (r < 0)
        pa_log(__FILE__": select(): %s\n", strerror(errno));
    
    m->running = 0;
    return r < 0 ? -1 : 0;
}

int pa_mainloop_run(struct pa_mainloop *m, int *retval) {
    int r;
    while ((r = pa_mainloop_iterate(m, 1, retval)) == 0);
    return r;
}

void pa_mainloop_quit(struct pa_mainloop *m, int r) {
    assert(m);
    m->quit = r;
}

struct pa_mainloop_api* pa_mainloop_get_api(struct pa_mainloop*m) {
    assert(m);
    return &m->api;
}
