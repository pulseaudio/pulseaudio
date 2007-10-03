/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif

#include <unistd.h>
#include <errno.h>

#include <pulsecore/atomic.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulse/xmalloc.h>

#ifndef HAVE_PIPE
#include <pulsecore/pipe.h>
#endif

#ifdef __linux__

#if !defined(__NR_eventfd) && defined(__i386__)
#define __NR_eventfd 323
#endif

#if !defined(__NR_eventfd) && defined(__x86_64__)
#define __NR_eventfd 284
#endif

#if !defined(SYS_eventfd) && defined(__NR_eventfd)
#define SYS_eventfd __NR_eventfd
#endif

#ifdef SYS_eventfd
#define HAVE_EVENTFD

static inline long eventfd(unsigned count) {
    return syscall(SYS_eventfd, count);
}

#endif
#endif

#include "fdsem.h"

struct pa_fdsem {
    int fds[2];
#ifdef HAVE_EVENTFD
    int efd;
#endif
    pa_atomic_t waiting;
    pa_atomic_t signalled;
    pa_atomic_t in_pipe;
};

pa_fdsem *pa_fdsem_new(void) {
    pa_fdsem *f;

    f = pa_xnew(pa_fdsem, 1);

#ifdef HAVE_EVENTFD
    if ((f->efd = eventfd(0)) >= 0) {
        pa_make_fd_cloexec(f->efd);
        f->fds[0] = f->fds[1] = -1;

    } else
#endif
    {
        if (pipe(f->fds) < 0) {
            pa_xfree(f);
            return NULL;
        }

        pa_make_fd_cloexec(f->fds[0]);
        pa_make_fd_cloexec(f->fds[1]);
    }

    pa_atomic_store(&f->waiting, 0);
    pa_atomic_store(&f->signalled, 0);
    pa_atomic_store(&f->in_pipe, 0);

    return f;
}

void pa_fdsem_free(pa_fdsem *f) {
    pa_assert(f);

#ifdef HAVE_EVENTFD
    if (f->efd >= 0)
        pa_close(f->efd);
#endif
    pa_close_pipe(f->fds);

    pa_xfree(f);
}

static void flush(pa_fdsem *f) {
    ssize_t r;
    pa_assert(f);

    if (pa_atomic_load(&f->in_pipe) <= 0)
        return;

    do {
        char x[10];

#ifdef HAVE_EVENTFD
        if (f->efd >= 0) {
            uint64_t u;

            if ((r = read(f->efd, &u, sizeof(u))) != sizeof(u)) {
                pa_assert(r < 0 && errno == EINTR);
                continue;
            }
            r = (ssize_t) u;
        } else
#endif

        if ((r = read(f->fds[0], &x, sizeof(x))) <= 0) {
            pa_assert(r < 0 && errno == EINTR);
            continue;
        }

    } while (pa_atomic_sub(&f->in_pipe, r) > r);
}

void pa_fdsem_post(pa_fdsem *f) {
    pa_assert(f);

    if (pa_atomic_cmpxchg(&f->signalled, 0, 1)) {

        if (pa_atomic_load(&f->waiting)) {
            ssize_t r;
            char x = 'x';

            pa_atomic_inc(&f->in_pipe);

            for (;;) {

#ifdef HAVE_EVENTFD
                if (f->efd >= 0) {
                    uint64_t u = 1;

                    if ((r = write(f->efd, &u, sizeof(u))) != sizeof(u)) {
                        pa_assert(r < 0 && errno == EINTR);
                        continue;
                    }
                } else
#endif

                if ((r = write(f->fds[1], &x, 1)) != 1) {
                    pa_assert(r < 0 && errno == EINTR);
                    continue;
                }

                break;
            }
        }
    }
}

void pa_fdsem_wait(pa_fdsem *f) {
    pa_assert(f);

    flush(f);

    if (pa_atomic_cmpxchg(&f->signalled, 1, 0))
        return;

    pa_atomic_inc(&f->waiting);

    while (!pa_atomic_cmpxchg(&f->signalled, 1, 0)) {
        char x[10];
        ssize_t r;

#ifdef HAVE_EVENTFD
        if (f->efd >= 0) {
            uint64_t u;

            if ((r = read(f->efd, &u, sizeof(u))) != sizeof(u)) {
                pa_assert(r < 0 && errno == EINTR);
                continue;
            }

            r = (ssize_t) u;
        } else
#endif

        if ((r = read(f->fds[0], &x, sizeof(x))) <= 0) {
            pa_assert(r < 0 && errno == EINTR);
            continue;
        }

        pa_atomic_sub(&f->in_pipe, r);
    }

    pa_assert_se(pa_atomic_dec(&f->waiting) >= 1);
}

int pa_fdsem_try(pa_fdsem *f) {
    pa_assert(f);

    flush(f);

    if (pa_atomic_cmpxchg(&f->signalled, 1, 0))
        return 1;

    return 0;
}

int pa_fdsem_get(pa_fdsem *f) {
    pa_assert(f);

#ifdef HAVE_EVENTFD
    if (f->efd >= 0)
        return f->efd;
#endif

    return f->fds[0];
}

int pa_fdsem_before_poll(pa_fdsem *f) {
    pa_assert(f);

    flush(f);

    if (pa_atomic_cmpxchg(&f->signalled, 1, 0))
        return -1;

    pa_atomic_inc(&f->waiting);

    if (pa_atomic_cmpxchg(&f->signalled, 1, 0)) {
        pa_assert_se(pa_atomic_dec(&f->waiting) >= 1);
        return -1;
    }
    return 0;
}

int pa_fdsem_after_poll(pa_fdsem *f) {
    pa_assert(f);

    pa_assert_se(pa_atomic_dec(&f->waiting) >= 1);

    flush(f);

    if (pa_atomic_cmpxchg(&f->signalled, 1, 0))
        return 1;

    return 0;
}
