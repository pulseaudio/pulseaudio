/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <sys/mman.h>

#include <pulse/xmalloc.h>

#include <pulsecore/semaphore.h>
#include <pulsecore/macro.h>
#include <pulsecore/mutex.h>
#include <pulsecore/core-util.h>

#include "memtrap.h"

struct pa_memtrap {
    void *start;
    size_t size;
    pa_atomic_t bad;
    pa_memtrap *next[2], *prev[2];
};

static pa_memtrap *memtraps[2] = { NULL, NULL };
static pa_atomic_t read_lock = PA_ATOMIC_INIT(0);
static pa_static_semaphore semaphore = PA_STATIC_SEMAPHORE_INIT;
static pa_static_mutex write_lock = PA_STATIC_MUTEX_INIT;

#define MSB (1U << (sizeof(unsigned)*8U-1))
#define WHICH(n) (!!((n) & MSB))
#define COUNTER(n) ((n) & ~MSB)

pa_bool_t pa_memtrap_is_good(pa_memtrap *m) {
    pa_assert(m);

    return !pa_atomic_load(&m->bad);
}

static void sigsafe_error(const char *s) {
    write(STDERR_FILENO, s, strlen(s));
}

static void signal_handler(int sig, siginfo_t* si, void *data) {
    unsigned n, j;
    pa_memtrap *m;
    void *r;

    /* Increase the lock counter */
    n = (unsigned) pa_atomic_inc(&read_lock);

    /* The uppermost bit tells us which list to look at */
    j = WHICH(n);

    /* When n is 0 we have about 2^31 threads running that
     * all got a sigbus at the same time, oh my! */
    pa_assert(COUNTER(n)+1 > 0);

    for (m = memtraps[j]; m; m = m->next[j])
        if (si->si_addr >= m->start &&
            (uint8_t*) si->si_addr < (uint8_t*) m->start + m->size)
            break;

    if (!m)
        goto fail;

    pa_atomic_store(&m->bad, 1);

    /* Remap anonymous memory into the bad segment */
    if ((r = mmap(m->start, m->size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_FIXED|MAP_PRIVATE, -1, 0)) == MAP_FAILED) {
        sigsafe_error("mmap() failed.\n");
        goto fail;
    }

    pa_assert(r == m->start);

    pa_atomic_dec(&read_lock);

    /* Post the semaphore */
    pa_semaphore_post(pa_static_semaphore_get(&semaphore, 0));

    return;

fail:
    sigsafe_error("Failed to handle SIGBUS.\n");
    pa_atomic_dec(&read_lock);
    abort();
}

static void memtrap_swap(unsigned n) {

    for (;;) {

        /* If the read counter is > 0 wait; if it is 0 try to swap the lists */
        if (COUNTER(n) > 0)
            pa_semaphore_wait(pa_static_semaphore_get(&semaphore, 0));
        else if (pa_atomic_cmpxchg(&read_lock, (int) n, (int) (n ^ MSB)))
            break;

        n = (unsigned) pa_atomic_load(&read_lock);
    }
}

static void memtrap_link(pa_memtrap *m, unsigned j) {
    pa_assert(m);

    m->prev[j] = NULL;
    m->next[j] = memtraps[j];
    memtraps[j] = m;
}

static void memtrap_unlink(pa_memtrap *m, int j) {
    pa_assert(m);

    if (m->next[j])
        m->next[j]->prev[j] = m->prev[j];

    if (m->prev[j])
        m->prev[j]->next[j] = m->next[j];
    else
        memtraps[j] = m->next[j];
}

pa_memtrap* pa_memtrap_add(const void *start, size_t size) {
    pa_memtrap *m = NULL;
    pa_mutex *lock;
    unsigned n, j;

    pa_assert(start);
    pa_assert(size > 0);
    pa_assert(PA_PAGE_ALIGN_PTR(start) == start);
    pa_assert(PA_PAGE_ALIGN(size) == size);

    lock = pa_static_mutex_get(&write_lock, FALSE, FALSE);
    pa_mutex_lock(lock);

    if (!memtraps[0]) {
        struct sigaction sa;

        /* Before we install the signal handler, make sure the
         * semaphore is valid so that the initialization of the
         * semaphore doesn't have to happen from the signal handler */
        pa_static_semaphore_get(&semaphore, 0);

        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = signal_handler;
        sa.sa_flags = SA_RESTART|SA_SIGINFO;

        pa_assert_se(sigaction(SIGBUS, &sa, NULL) == 0);
    }

    n = (unsigned) pa_atomic_load(&read_lock);
    j = WHICH(n);

    m = pa_xnew(pa_memtrap, 1);
    m->start = (void*) start;
    m->size = size;
    pa_atomic_store(&m->bad, 0);

    memtrap_link(m, !j);
    memtrap_swap(n);
    memtrap_link(m, j);

    pa_mutex_unlock(lock);

    return m;
}

void pa_memtrap_remove(pa_memtrap *m) {
    unsigned n, j;
    pa_mutex *lock;

    pa_assert(m);

    lock = pa_static_mutex_get(&write_lock, FALSE, FALSE);
    pa_mutex_lock(lock);

    n = (unsigned) pa_atomic_load(&read_lock);
    j = WHICH(n);

    memtrap_unlink(m, !j);
    memtrap_swap(n);
    memtrap_unlink(m, j);

    pa_xfree(m);

    if (!memtraps[0]) {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        pa_assert_se(sigaction(SIGBUS, &sa, NULL) == 0);
    }

    pa_mutex_unlock(lock);
}

pa_memtrap *pa_memtrap_update(pa_memtrap *m, const void *start, size_t size) {
    unsigned n, j;
    pa_mutex *lock;

    pa_assert(m);

    pa_assert(start);
    pa_assert(size > 0);
    pa_assert(PA_PAGE_ALIGN_PTR(start) == start);
    pa_assert(PA_PAGE_ALIGN(size) == size);

    lock = pa_static_mutex_get(&write_lock, FALSE, FALSE);
    pa_mutex_lock(lock);

    if (m->start == start && m->size == size)
        goto unlock;

    n = (unsigned) pa_atomic_load(&read_lock);
    j = WHICH(n);

    memtrap_unlink(m, !j);
    memtrap_swap(n);
    memtrap_unlink(m, j);

    m->start = (void*) start;
    m->size = size;
    pa_atomic_store(&m->bad, 0);

    n = (unsigned) pa_atomic_load(&read_lock);
    j = WHICH(n);

    memtrap_link(m, !j);
    memtrap_swap(n);
    memtrap_link(m, j);

unlock:
    pa_mutex_unlock(lock);

    return m;
}
