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

#ifdef __linux__
#include <sys/utsname.h>
#endif

#ifdef HAVE_POLL_H
#include <poll.h>
#else
#include <pulsecore/poll.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/core-error.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/macro.h>
#include <pulsecore/llist.h>
#include <pulsecore/rtsig.h>
#include <pulsecore/flist.h>
#include <pulsecore/core-util.h>
#include <pulsecore/winsock.h>
#include <pulsecore/ratelimit.h>

#include "rtpoll.h"

/* #define DEBUG_TIMING */

struct pa_rtpoll {
    struct pollfd *pollfd, *pollfd2;
    unsigned n_pollfd_alloc, n_pollfd_used;

    struct timeval next_elapse;
    pa_bool_t timer_enabled:1;

    pa_bool_t scan_for_dead:1;
    pa_bool_t running:1;
    pa_bool_t installed:1;
    pa_bool_t rebuild_needed:1;
    pa_bool_t quit:1;

#ifdef HAVE_PPOLL
    pa_bool_t timer_armed:1;
#ifdef __linux__
    pa_bool_t dont_use_ppoll:1;
#endif
    int rtsig;
    sigset_t sigset_unblocked;
    timer_t timer;
#endif

#ifdef DEBUG_TIMING
    pa_usec_t timestamp;
    pa_usec_t slept, awake;
#endif

    PA_LLIST_HEAD(pa_rtpoll_item, items);
};

struct pa_rtpoll_item {
    pa_rtpoll *rtpoll;
    pa_bool_t dead;

    pa_rtpoll_priority_t priority;

    struct pollfd *pollfd;
    unsigned n_pollfd;

    int (*work_cb)(pa_rtpoll_item *i);
    int (*before_cb)(pa_rtpoll_item *i);
    void (*after_cb)(pa_rtpoll_item *i);
    void *userdata;

    PA_LLIST_FIELDS(pa_rtpoll_item);
};

PA_STATIC_FLIST_DECLARE(items, 0, pa_xfree);

static void signal_handler_noop(int s) { /* write(2, "signal\n", 7); */ }

pa_rtpoll *pa_rtpoll_new(void) {
    pa_rtpoll *p;

    p = pa_xnew(pa_rtpoll, 1);

#ifdef HAVE_PPOLL

#ifdef __linux__
    /* ppoll is broken on Linux < 2.6.16 */
    p->dont_use_ppoll = FALSE;

    {
        struct utsname u;
        unsigned major, minor, micro;

        pa_assert_se(uname(&u) == 0);

        if (sscanf(u.release, "%u.%u.%u", &major, &minor, &micro) != 3 ||
            (major < 2) ||
            (major == 2 && minor < 6) ||
            (major == 2 && minor == 6 && micro < 16))

            p->dont_use_ppoll = TRUE;
    }

#endif

    p->rtsig = -1;
    sigemptyset(&p->sigset_unblocked);
    p->timer = (timer_t) -1;
    p->timer_armed = FALSE;

#endif

    p->n_pollfd_alloc = 32;
    p->pollfd = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->pollfd2 = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->n_pollfd_used = 0;

    memset(&p->next_elapse, 0, sizeof(p->next_elapse));
    p->timer_enabled = FALSE;

    p->running = FALSE;
    p->installed = FALSE;
    p->scan_for_dead = FALSE;
    p->rebuild_needed = FALSE;
    p->quit = FALSE;

    PA_LLIST_HEAD_INIT(pa_rtpoll_item, p->items);

#ifdef DEBUG_TIMING
    p->timestamp = pa_rtclock_usec();
    p->slept = p->awake = 0;
#endif

    return p;
}

void pa_rtpoll_install(pa_rtpoll *p) {
    pa_assert(p);
    pa_assert(!p->installed);

    p->installed = TRUE;

#ifdef HAVE_PPOLL
# ifdef __linux__
    if (p->dont_use_ppoll)
        return;
# endif

    if ((p->rtsig = pa_rtsig_get_for_thread()) < 0) {
        pa_log_warn("Failed to reserve POSIX realtime signal.");
        return;
    }

    pa_log_debug("Acquired POSIX realtime signal %s", pa_sig2str(p->rtsig));

    {
        sigset_t ss;
        struct sigaction sa;

        pa_assert_se(sigemptyset(&ss) == 0);
        pa_assert_se(sigaddset(&ss, p->rtsig) == 0);
        pa_assert_se(pthread_sigmask(SIG_BLOCK, &ss, &p->sigset_unblocked) == 0);
        pa_assert_se(sigdelset(&p->sigset_unblocked, p->rtsig) == 0);

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler_noop;
        pa_assert_se(sigemptyset(&sa.sa_mask) == 0);

        pa_assert_se(sigaction(p->rtsig, &sa, NULL) == 0);

        /* We never reset the signal handler. Why should we? */
    }

#endif
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

    p->n_pollfd_used -= i->n_pollfd;

    if (pa_flist_push(PA_STATIC_FLIST_GET(items), i) < 0)
        pa_xfree(i);

    p->rebuild_needed = TRUE;
}

void pa_rtpoll_free(pa_rtpoll *p) {
    pa_assert(p);

    while (p->items)
        rtpoll_item_destroy(p->items);

    pa_xfree(p->pollfd);
    pa_xfree(p->pollfd2);

#ifdef HAVE_PPOLL
    if (p->timer != (timer_t) -1)
        timer_delete(p->timer);
#endif

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

int pa_rtpoll_run(pa_rtpoll *p, pa_bool_t wait) {
    pa_rtpoll_item *i;
    int r = 0;
    struct timeval timeout;

    pa_assert(p);
    pa_assert(!p->running);
    pa_assert(p->installed);

    p->running = TRUE;

    /* First, let's do some work */
    for (i = p->items; i && i->priority < PA_RTPOLL_NEVER; i = i->next) {
        int k;

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
    for (i = p->items; i && i->priority < PA_RTPOLL_NEVER; i = i->next) {
        int k = 0;

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

    memset(&timeout, 0, sizeof(timeout));

    /* Calculate timeout */
    if (wait && !p->quit && p->timer_enabled) {
        struct timeval now;
        pa_rtclock_get(&now);

        if (pa_timeval_cmp(&p->next_elapse, &now) > 0)
            pa_timeval_add(&timeout, pa_timeval_diff(&p->next_elapse, &now));
    }

#ifdef DEBUG_TIMING
    {
        pa_usec_t now = pa_rtclock_usec();
        p->awake = now - p->timestamp;
        p->timestamp = now;
    }
#endif

    /* OK, now let's sleep */
#ifdef HAVE_PPOLL

#ifdef __linux__
    if (!p->dont_use_ppoll)
#endif
    {
        struct timespec ts;
        ts.tv_sec = timeout.tv_sec;
        ts.tv_nsec = timeout.tv_usec * 1000;
        r = ppoll(p->pollfd, p->n_pollfd_used, (!wait || p->quit || p->timer_enabled) ? &ts : NULL, p->rtsig < 0 ? NULL : &p->sigset_unblocked);
    }
#ifdef __linux__
    else
#endif

#endif
        r = poll(p->pollfd, p->n_pollfd_used, (!wait || p->quit || p->timer_enabled) ? (int) ((timeout.tv_sec*1000) + (timeout.tv_usec / 1000)) : -1);

#ifdef DEBUG_TIMING
    {
        pa_usec_t now = pa_rtclock_usec();
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
    for (i = p->items; i && i->priority < PA_RTPOLL_NEVER; i = i->next) {

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

        for (i = p->items; i; i = n) {
            n = i->next;

            if (i->dead)
                rtpoll_item_destroy(i);
        }
    }

    return r < 0 ? r : !p->quit;
}

static void update_timer(pa_rtpoll *p) {
    pa_assert(p);

#ifdef HAVE_PPOLL

#ifdef __linux__
    if (p->dont_use_ppoll)
        return;
#endif

    if (p->timer == (timer_t) -1) {
        struct sigevent se;

        memset(&se, 0, sizeof(se));
        se.sigev_notify = SIGEV_SIGNAL;
        se.sigev_signo = p->rtsig;

        if (timer_create(CLOCK_MONOTONIC, &se, &p->timer) < 0)
            if (timer_create(CLOCK_REALTIME, &se, &p->timer) < 0) {
                pa_log_warn("Failed to allocate POSIX timer: %s", pa_cstrerror(errno));
                p->timer = (timer_t) -1;
            }
    }

    if (p->timer != (timer_t) -1) {
        struct itimerspec its;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
        sigset_t ss;

        if (p->timer_armed) {
            /* First disarm timer */
            memset(&its, 0, sizeof(its));
            pa_assert_se(timer_settime(p->timer, TIMER_ABSTIME, &its, NULL) == 0);

            /* Remove a signal that might be waiting in the signal q */
            pa_assert_se(sigemptyset(&ss) == 0);
            pa_assert_se(sigaddset(&ss, p->rtsig) == 0);
            sigtimedwait(&ss, NULL, &ts);
        }

        /* And install the new timer */
        if (p->timer_enabled) {
            memset(&its, 0, sizeof(its));

            its.it_value.tv_sec = p->next_elapse.tv_sec;
            its.it_value.tv_nsec = p->next_elapse.tv_usec*1000;

            /* Make sure that 0,0 is not understood as
             * "disarming" */
            if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
                its.it_value.tv_nsec = 1;
            pa_assert_se(timer_settime(p->timer, TIMER_ABSTIME, &its, NULL) == 0);
        }

        p->timer_armed = p->timer_enabled;
    }

#endif
}

void pa_rtpoll_set_timer_absolute(pa_rtpoll *p, pa_usec_t usec) {
    pa_assert(p);

    pa_timeval_store(&p->next_elapse, usec);
    p->timer_enabled = TRUE;

    update_timer(p);
}

void pa_rtpoll_set_timer_relative(pa_rtpoll *p, pa_usec_t usec) {
    pa_assert(p);

    /* Scheduling a timeout for more than an hour is very very suspicious */
    pa_assert(usec <= PA_USEC_PER_SEC*60ULL*60ULL);

    pa_rtclock_get(&p->next_elapse);
    pa_timeval_add(&p->next_elapse, usec);
    p->timer_enabled = TRUE;

    update_timer(p);
}

void pa_rtpoll_set_timer_disabled(pa_rtpoll *p) {
    pa_assert(p);

    memset(&p->next_elapse, 0, sizeof(p->next_elapse));
    p->timer_enabled = FALSE;

    update_timer(p);
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

    i->userdata = NULL;
    i->before_cb = NULL;
    i->after_cb = NULL;
    i->work_cb = NULL;

    for (j = p->items; j; j = j->next) {
        if (prio <= j->priority)
            break;

        l = j;
    }

    PA_LLIST_INSERT_AFTER(pa_rtpoll_item, p->items, j ? j->prev : l, i);

    if (n_fds > 0) {
        p->rebuild_needed = 1;
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
