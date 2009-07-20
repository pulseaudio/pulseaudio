/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_POLL_H
#include <poll.h>
#else
#include <pulsecore/poll.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/macro.h>
#include <pulsecore/llist.h>
#include <pulsecore/flist.h>
#include <pulsecore/core-util.h>
#include <pulsecore/winsock.h>
#include <pulsecore/ratelimit.h>
#include <pulsecore/prioq.h>

#include "rtpoll.h"

/* #define DEBUG_TIMING */

struct pa_rtpoll {
    void *userdata;

    struct pollfd *pollfd, *pollfd2;
    unsigned n_pollfd_alloc, n_pollfd_used;

    pa_prioq *prioq;

    pa_usec_t elapse;
    pa_bool_t timer_enabled:1;

    pa_bool_t running:1;
    pa_bool_t scan_for_dead:1;
    pa_bool_t rebuild_needed:1;
    pa_bool_t quit:1;

#ifdef DEBUG_TIMING
    pa_usec_t timestamp;
    pa_usec_t slept, awake;
#endif

    PA_LLIST_HEAD(pa_rtpoll_item, items);
};

struct pa_rtpoll_item {
    pa_rtpoll *rtpoll;
    pa_rtpoll_priority_t priority;

    pa_bool_t dead:1;

    pa_bool_t timer_enabled:1;
    pa_usec_t elapse;

    pa_prioq_item *prioq_item;

    struct pollfd *pollfd;
    unsigned n_pollfd;

    int (*work_cb)(pa_rtpoll_item *i);
    int (*before_cb)(pa_rtpoll_item *i);
    void (*after_cb)(pa_rtpoll_item *i);
    void *userdata;

    PA_LLIST_FIELDS(pa_rtpoll_item);
};

PA_STATIC_FLIST_DECLARE(items, 0, pa_xfree);

static int item_compare(const void *_a, const void *_b) {
    const pa_rtpoll_item *a = _a, *b = _b;

    pa_assert(a->timer_enabled);
    pa_assert(b->timer_enabled);

    if (a->elapse < b->elapse)
        return -1;
    if (a->elapse > b->elapse)
        return 1;

    return 0;
}

pa_rtpoll *pa_rtpoll_new(void) {
    pa_rtpoll *p;

    p = pa_xnew(pa_rtpoll, 1);

    p->userdata = NULL;

    p->n_pollfd_alloc = 32;
    p->pollfd = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->pollfd2 = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->n_pollfd_used = 0;

    p->prioq = pa_prioq_new(item_compare);

    p->elapse = 0;
    p->timer_enabled = FALSE;

    p->running = FALSE;
    p->scan_for_dead = FALSE;
    p->rebuild_needed = FALSE;
    p->quit = FALSE;

#ifdef DEBUG_TIMING
    p->timestamp = pa_rtclock_now();
    p->slept = p->awake = 0;
#endif

    PA_LLIST_HEAD_INIT(pa_rtpoll_item, p->items);

    return p;
}

static void rtpoll_rebuild(pa_rtpoll *p) {

    struct pollfd *e, *t;
    pa_rtpoll_item *i;
    int ra = 0;

    pa_assert(p);

    p->rebuild_needed = FALSE;

    if (p->n_pollfd_used > p->n_pollfd_alloc) {
        /* Hmm, we have to allocate some more space */
        p->n_pollfd_alloc = p->n_pollfd_used * 2;
        p->pollfd2 = pa_xrealloc(p->pollfd2, p->n_pollfd_alloc * sizeof(struct pollfd));
        ra = 1;
    }

    e = p->pollfd2;

    for (i = p->items; i; i = i->next) {

        if (i->n_pollfd > 0)  {
            size_t l = i->n_pollfd * sizeof(struct pollfd);

            if (i->pollfd)
                memcpy(e, i->pollfd, l);
            else
                memset(e, 0, l);

            i->pollfd = e;
        } else
            i->pollfd = NULL;

        e += i->n_pollfd;
    }

    pa_assert((unsigned) (e - p->pollfd2) == p->n_pollfd_used);
    t = p->pollfd;
    p->pollfd = p->pollfd2;
    p->pollfd2 = t;

    if (ra)
        p->pollfd2 = pa_xrealloc(p->pollfd2, p->n_pollfd_alloc * sizeof(struct pollfd));
}

static void rtpoll_item_destroy(pa_rtpoll_item *i) {
    pa_rtpoll *p;

    pa_assert(i);

    p = i->rtpoll;

    PA_LLIST_REMOVE(pa_rtpoll_item, p->items, i);

    pa_assert(p->n_pollfd_used >= i->n_pollfd);
    p->n_pollfd_used -= i->n_pollfd;

    if (pa_flist_push(PA_STATIC_FLIST_GET(items), i) < 0)
        pa_xfree(i);

    if (i->prioq_item)
        pa_prioq_remove(p->prioq, i->prioq_item);

    p->rebuild_needed = TRUE;
}

void pa_rtpoll_free(pa_rtpoll *p) {
    pa_assert(p);

    while (p->items)
        rtpoll_item_destroy(p->items);

    pa_xfree(p->pollfd);
    pa_xfree(p->pollfd2);

    if (p->prioq)
        pa_prioq_free(p->prioq, NULL, NULL);

    pa_xfree(p);
}

static void reset_revents(pa_rtpoll_item *i) {
    struct pollfd *f;
    unsigned n;

    pa_assert(i);

    if (!(f = pa_rtpoll_item_get_pollfd(i, &n)))
        return;

    for (; n > 0; n--)
        f[n-1].revents = 0;
}

static void reset_all_revents(pa_rtpoll *p) {
    pa_rtpoll_item *i;

    pa_assert(p);

    for (i = p->items; i; i = i->next) {

        if (i->dead)
            continue;

        reset_revents(i);
    }
}

static pa_bool_t next_elapse(pa_rtpoll *p, pa_usec_t *usec) {
    pa_rtpoll_item *i;

    pa_assert(p);
    pa_assert(usec);

    i = pa_prioq_peek(p->prioq);

    if (p->timer_enabled) {

        if (i && i->timer_enabled)
            *usec = PA_MIN(i->elapse, p->elapse);
        else
            *usec = p->elapse;

        return TRUE;

    } else if (i && i->timer_enabled) {
        *usec = i->elapse;
        return TRUE;
    }

    return FALSE;
}

int pa_rtpoll_run(pa_rtpoll *p, pa_bool_t wait_op) {
    pa_rtpoll_item *i;
    int r = 0;
    pa_usec_t timeout;
    pa_bool_t timeout_valid;

    pa_assert(p);
    pa_assert(!p->running);

    p->running = TRUE;

    /* First, let's do some work */
    PA_LLIST_FOREACH(i, p->items) {
        int k;

        if (i->priority >= PA_RTPOLL_NEVER)
            break;

        if (i->dead)
            continue;

        if (!i->work_cb)
            continue;

        if (p->quit)
            goto finish;

        if ((k = i->work_cb(i)) != 0) {
            if (k < 0)
                r = k;

            goto finish;
        }
    }

    /* Now let's prepare for entering the sleep */
    PA_LLIST_FOREACH(i, p->items) {
        int k = 0;

        if (i->priority >= PA_RTPOLL_NEVER)
            break;

        if (i->dead)
            continue;

        if (!i->before_cb)
            continue;

        if (p->quit || (k = i->before_cb(i)) != 0) {

            /* Hmm, this one doesn't let us enter the poll, so rewind everything */

            for (i = i->prev; i; i = i->prev) {

                if (i->dead)
                    continue;

                if (!i->after_cb)
                    continue;

                i->after_cb(i);
            }

            if (k < 0)
                r = k;

            goto finish;
        }
    }

    if (p->rebuild_needed)
        rtpoll_rebuild(p);

    timeout = 0;
    timeout_valid = FALSE;

    /* Calculate timeout */
    if (wait_op && !p->quit) {
        pa_usec_t elapse;

        if (next_elapse(p, &elapse)) {
            pa_usec_t now;

            now = pa_rtclock_now();
            timeout = now >= elapse ? 0 : elapse - now;
            timeout_valid = TRUE;
        }
    }

#ifdef DEBUG_TIMING
    {
        pa_usec_t now = pa_rtclock_now();
        p->awake = now - p->timestamp;
        p->timestamp = now;
    }
#endif

    /* OK, now let's sleep */
    {
#ifdef HAVE_PPOLL
        struct timespec ts;
        r = ppoll(p->pollfd, p->n_pollfd_used,
                  (!wait_op || p->quit || timeout_valid) ?
                  pa_timespec_store(&ts, timeout) : NULL, NULL);
#else
        r = poll(p->pollfd, p->n_pollfd_used,
                 (!wait_op || p->quit || timeout_valid) ?
                 (int) (timeout / PA_USEC_PER_MSEC) : -1);
#endif
    }

#ifdef DEBUG_TIMING
    {
        pa_usec_t now = pa_rtclock_now();
        p->slept = now - p->timestamp;
        p->timestamp = now;

        pa_log("Process time %llu ms; sleep time %llu ms",
               (unsigned long long) (p->awake / PA_USEC_PER_MSEC),
               (unsigned long long) (p->slept / PA_USEC_PER_MSEC));
    }
#endif

    if (r < 0) {
        if (errno == EAGAIN || errno == EINTR)
            r = 0;
        else
            pa_log_error("poll(): %s", pa_cstrerror(errno));

        reset_all_revents(p);
    }

    /* Let's tell everyone that we left the sleep */
    PA_LLIST_FOREACH(i, p->items) {
        if (i->priority >= PA_RTPOLL_NEVER)
            break;

        if (i->dead)
            continue;

        if (!i->after_cb)
            continue;

        i->after_cb(i);
    }

finish:

    p->running = FALSE;

    if (p->scan_for_dead) {
        pa_rtpoll_item *n;

        p->scan_for_dead = FALSE;

        PA_LLIST_FOREACH_FOR_DELETE(i, n, p->items)
            if (i->dead)
                rtpoll_item_destroy(i);
    }

    return r < 0 ? r : !p->quit;
}

void pa_rtpoll_set_timer_absolute(pa_rtpoll *p, pa_usec_t usec) {
    pa_assert(p);

    if (p->timer_enabled && p->elapse == usec)
        return;

    p->elapse = usec;
    p->timer_enabled = TRUE;
}

void pa_rtpoll_set_timer_relative(pa_rtpoll *p, pa_usec_t usec) {
    pa_assert(p);

    /* Scheduling a timeout for more than an hour is very very suspicious */
    pa_assert(usec <= PA_USEC_PER_SEC*60ULL*60ULL);

    pa_rtpoll_set_timer_absolute(p, pa_rtclock_now() + usec);
}

void pa_rtpoll_disable_timer(pa_rtpoll *p) {
    pa_assert(p);

    if (!p->timer_enabled)
        return;

    p->elapse = 0;
    p->timer_enabled = FALSE;
}

void pa_rtpoll_set_userdata(pa_rtpoll *p, void *userdata) {
    pa_assert(p);

    p->userdata = userdata;
}

void* pa_rtpoll_get_userdata(pa_rtpoll *p) {
    pa_assert(p);

    return p->userdata;
}

pa_rtpoll_item *pa_rtpoll_item_new(pa_rtpoll *p, pa_rtpoll_priority_t prio, unsigned n_fds) {
    pa_rtpoll_item *i, *j, *l = NULL;

    pa_assert(p);

    if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        i = pa_xnew(pa_rtpoll_item, 1);

    i->rtpoll = p;
    i->dead = FALSE;
    i->n_pollfd = n_fds;
    i->pollfd = NULL;
    i->priority = prio;
    i->timer_enabled = FALSE;
    i->elapse = 0;
    i->prioq_item = NULL;

    i->userdata = NULL;
    i->before_cb = NULL;
    i->after_cb = NULL;
    i->work_cb = NULL;

    PA_LLIST_FOREACH(j, p->items) {
        if (prio <= j->priority)
            break;

        l = j;
    }

    PA_LLIST_INSERT_AFTER(pa_rtpoll_item, p->items, j ? j->prev : l, i);

    if (n_fds > 0) {
        p->rebuild_needed = TRUE;
        p->n_pollfd_used += n_fds;
    }

    return i;
}

void pa_rtpoll_item_free(pa_rtpoll_item *i) {
    pa_assert(i);

    if (i->rtpoll->running) {
        i->dead = TRUE;
        i->rtpoll->scan_for_dead = TRUE;
        return;
    }

    rtpoll_item_destroy(i);
}

struct pollfd *pa_rtpoll_item_get_pollfd(pa_rtpoll_item *i, unsigned *n_fds) {
    pa_assert(i);

    if (i->n_pollfd > 0)
        if (i->rtpoll->rebuild_needed)
            rtpoll_rebuild(i->rtpoll);

    if (n_fds)
        *n_fds = i->n_pollfd;

    return i->pollfd;
}

void pa_rtpoll_item_set_n_fds(pa_rtpoll_item *i, unsigned n_fds) {
    pa_assert(i);

    if (i->n_pollfd == n_fds)
        return;

    pa_assert(i->rtpoll->n_pollfd_used >= i->n_pollfd);
    i->rtpoll->n_pollfd_used = i->rtpoll->n_pollfd_used - i->n_pollfd + n_fds;
    i->n_pollfd = n_fds;

    i->pollfd = NULL;
    i->rtpoll->rebuild_needed = TRUE;
}

void pa_rtpoll_item_set_timer_absolute(pa_rtpoll_item *i, pa_usec_t usec){
    pa_assert(i);

    if (i->timer_enabled && i->elapse == usec)
        return;

    i->timer_enabled = TRUE;
    i->elapse = usec;

    if (i->prioq_item)
        pa_prioq_reshuffle(i->rtpoll->prioq, i->prioq_item);
    else
        i->prioq_item = pa_prioq_put(i->rtpoll->prioq, i);
}

void pa_rtpoll_item_set_timer_relative(pa_rtpoll_item *i, pa_usec_t usec) {
    pa_assert(i);

    /* Scheduling a timeout for more than an hour is very very suspicious */
    pa_assert(usec <= PA_USEC_PER_SEC*60ULL*60ULL);

    pa_rtpoll_item_set_timer_absolute(i, pa_rtclock_now() + usec);
}

void pa_rtpoll_item_disable_timer(pa_rtpoll_item *i) {
    pa_assert(i);

    if (!i->timer_enabled)
        return;

    i->timer_enabled = FALSE;
    i->elapse = 0;

    if (i->prioq_item) {
        pa_prioq_remove(i->rtpoll->prioq, i->prioq_item);
        i->prioq_item = NULL;
    }
}

void pa_rtpoll_item_set_before_callback(pa_rtpoll_item *i, int (*before_cb)(pa_rtpoll_item *i)) {
    pa_assert(i);
    pa_assert(i->priority < PA_RTPOLL_NEVER);

    i->before_cb = before_cb;
}

void pa_rtpoll_item_set_after_callback(pa_rtpoll_item *i, void (*after_cb)(pa_rtpoll_item *i)) {
    pa_assert(i);
    pa_assert(i->priority < PA_RTPOLL_NEVER);

    i->after_cb = after_cb;
}

void pa_rtpoll_item_set_work_callback(pa_rtpoll_item *i, int (*work_cb)(pa_rtpoll_item *i)) {
    pa_assert(i);
    pa_assert(i->priority < PA_RTPOLL_NEVER);

    i->work_cb = work_cb;
}

void pa_rtpoll_item_set_userdata(pa_rtpoll_item *i, void *userdata) {
    pa_assert(i);

    i->userdata = userdata;
}

void* pa_rtpoll_item_get_userdata(pa_rtpoll_item *i) {
    pa_assert(i);

    return i->userdata;
}

pa_rtpoll *pa_rtpoll_item_rtpoll(pa_rtpoll_item *i) {
    pa_assert(i);

    return i->rtpoll;
}

static int fdsem_before(pa_rtpoll_item *i) {

    if (pa_fdsem_before_poll(i->userdata) < 0)
        return 1; /* 1 means immediate restart of the loop */

    return 0;
}

static void fdsem_after(pa_rtpoll_item *i) {
    pa_assert(i);

    pa_assert((i->pollfd[0].revents & ~POLLIN) == 0);
    pa_fdsem_after_poll(i->userdata);
}

pa_rtpoll_item *pa_rtpoll_item_new_fdsem(pa_rtpoll *p, pa_rtpoll_priority_t prio, pa_fdsem *f) {
    pa_rtpoll_item *i;
    struct pollfd *pollfd;

    pa_assert(p);
    pa_assert(f);

    i = pa_rtpoll_item_new(p, prio, 1);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);

    pollfd->fd = pa_fdsem_get(f);
    pollfd->events = POLLIN;

    i->before_cb = fdsem_before;
    i->after_cb = fdsem_after;
    i->userdata = f;

    return i;
}

static int asyncmsgq_read_before(pa_rtpoll_item *i) {
    pa_assert(i);

    if (pa_asyncmsgq_read_before_poll(i->userdata) < 0)
        return 1; /* 1 means immediate restart of the loop */

    return 0;
}

static void asyncmsgq_read_after(pa_rtpoll_item *i) {
    pa_assert(i);

    pa_assert((i->pollfd[0].revents & ~POLLIN) == 0);
    pa_asyncmsgq_read_after_poll(i->userdata);
}

static int asyncmsgq_read_work(pa_rtpoll_item *i) {
    pa_msgobject *object;
    int code;
    void *data;
    pa_memchunk chunk;
    int64_t offset;

    pa_assert(i);

    if (pa_asyncmsgq_get(i->userdata, &object, &code, &data, &offset, &chunk, 0) == 0) {
        int ret;

        if (!object && code == PA_MESSAGE_SHUTDOWN) {
            pa_asyncmsgq_done(i->userdata, 0);
            pa_rtpoll_quit(i->rtpoll);
            return 1;
        }

        ret = pa_asyncmsgq_dispatch(object, code, data, offset, &chunk);
        pa_asyncmsgq_done(i->userdata, ret);
        return 1;
    }

    return 0;
}

pa_rtpoll_item *pa_rtpoll_item_new_asyncmsgq_read(pa_rtpoll *p, pa_rtpoll_priority_t prio, pa_asyncmsgq *q) {
    pa_rtpoll_item *i;
    struct pollfd *pollfd;

    pa_assert(p);
    pa_assert(q);

    i = pa_rtpoll_item_new(p, prio, 1);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);
    pollfd->fd = pa_asyncmsgq_read_fd(q);
    pollfd->events = POLLIN;

    i->before_cb = asyncmsgq_read_before;
    i->after_cb = asyncmsgq_read_after;
    i->work_cb = asyncmsgq_read_work;
    i->userdata = q;

    return i;
}

static int asyncmsgq_write_before(pa_rtpoll_item *i) {
    pa_assert(i);

    pa_asyncmsgq_write_before_poll(i->userdata);
    return 0;
}

static void asyncmsgq_write_after(pa_rtpoll_item *i) {
    pa_assert(i);

    pa_assert((i->pollfd[0].revents & ~POLLIN) == 0);
    pa_asyncmsgq_write_after_poll(i->userdata);
}

pa_rtpoll_item *pa_rtpoll_item_new_asyncmsgq_write(pa_rtpoll *p, pa_rtpoll_priority_t prio, pa_asyncmsgq *q) {
    pa_rtpoll_item *i;
    struct pollfd *pollfd;

    pa_assert(p);
    pa_assert(q);

    i = pa_rtpoll_item_new(p, prio, 1);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);
    pollfd->fd = pa_asyncmsgq_write_fd(q);
    pollfd->events = POLLIN;

    i->before_cb = asyncmsgq_write_before;
    i->after_cb = asyncmsgq_write_after;
    i->work_cb = NULL;
    i->userdata = q;

    return i;
}

void pa_rtpoll_quit(pa_rtpoll *p) {
    pa_assert(p);

    p->quit = TRUE;
}
