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

#include <assert.h>
#include <signal.h>
#include <stdio.h>

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#else
#include "../polypcore/poll.h"
#endif

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include <polypcore/log.h>
#include <polypcore/xmalloc.h>
#include <polypcore/hashmap.h>

#include "mainloop.h"
#include "thread-mainloop.h"

#if defined(HAVE_PTHREAD) || defined(OS_IS_WIN32)

struct pa_threaded_mainloop {
    pa_mainloop *real_mainloop;
    int n_waiting;
    int thread_running;

#ifdef OS_IS_WIN32
    DWORD thread_id;
    HANDLE thread;
    CRITICAL_SECTION mutex;
    pa_hashmap *cond_events;
    HANDLE accept_cond;
#else
    pthread_t thread_id;
    pthread_mutex_t mutex;
    pthread_cond_t cond, accept_cond;
#endif
};

static inline int in_worker(pa_threaded_mainloop *m) {
#ifdef OS_IS_WIN32
    return GetCurrentThreadId() == m->thread_id;
#else
    return pthread_equal(pthread_self(), m->thread_id);
#endif
}

static int poll_func(struct pollfd *ufds, unsigned long nfds, int timeout, void *userdata) {
#ifdef OS_IS_WIN32
    CRITICAL_SECTION *mutex = userdata;
#else
    pthread_mutex_t *mutex = userdata;
#endif

    int r;

    assert(mutex);

    /* Before entering poll() we unlock the mutex, so that
     * avahi_simple_poll_quit() can succeed from another thread. */

#ifdef OS_IS_WIN32
    LeaveCriticalSection(mutex);
#else    
    pthread_mutex_unlock(mutex);
#endif

    r = poll(ufds, nfds, timeout);

#ifdef OS_IS_WIN32
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif

    return r;
}

#ifdef OS_IS_WIN32
static DWORD WINAPI thread(void *userdata) {
#else
static void* thread(void *userdata) {
#endif
    pa_threaded_mainloop *m = userdata;

#ifndef OS_IS_WIN32
    sigset_t mask;

    /* Make sure that signals are delivered to the main thread */
    sigfillset(&mask);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
#endif

#ifdef OS_IS_WIN32
    EnterCriticalSection(&m->mutex);
#else
    pthread_mutex_lock(&m->mutex);
#endif

    pa_mainloop_run(m->real_mainloop, NULL);

#ifdef OS_IS_WIN32
    LeaveCriticalSection(&m->mutex);
#else
    pthread_mutex_unlock(&m->mutex);
#endif

#ifdef OS_IS_WIN32
    return 0;
#else
    return NULL;
#endif
}

pa_threaded_mainloop *pa_threaded_mainloop_new(void) {
    pa_threaded_mainloop *m;
#ifndef OS_IS_WIN32
    pthread_mutexattr_t a;
#endif

    m = pa_xnew(pa_threaded_mainloop, 1);

    if (!(m->real_mainloop = pa_mainloop_new())) {
        pa_xfree(m);
        return NULL;
    }

    pa_mainloop_set_poll_func(m->real_mainloop, poll_func, &m->mutex);

#ifdef OS_IS_WIN32
    InitializeCriticalSection(&m->mutex);

    m->cond_events = pa_hashmap_new(NULL, NULL);
    assert(m->cond_events);
    m->accept_cond = CreateEvent(NULL, FALSE, FALSE, NULL);
    assert(m->accept_cond);
#else
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mutex, &a);
    pthread_mutexattr_destroy(&a);
    
    pthread_cond_init(&m->cond, NULL);
    pthread_cond_init(&m->accept_cond, NULL);
#endif

    m->thread_running = 0;
    m->n_waiting = 0;

    return m;
}

void pa_threaded_mainloop_free(pa_threaded_mainloop* m) {
    assert(m);

    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !in_worker(m));

    if (m->thread_running)
        pa_threaded_mainloop_stop(m);

    if (m->real_mainloop)
        pa_mainloop_free(m->real_mainloop);

#ifdef OS_IS_WIN32
    pa_hashmap_free(m->cond_events, NULL, NULL);
    CloseHandle(m->accept_cond);
#else
    pthread_mutex_destroy(&m->mutex);
    pthread_cond_destroy(&m->cond);
    pthread_cond_destroy(&m->accept_cond);
#endif
    
    pa_xfree(m);
}

int pa_threaded_mainloop_start(pa_threaded_mainloop *m) {
    assert(m);

    assert(!m->thread_running);

#ifdef OS_IS_WIN32

    EnterCriticalSection(&m->mutex);

    m->thread = CreateThread(NULL, 0, thread, m, 0, &m->thread_id);
    if (!m->thread) {
        LeaveCriticalSection(&m->mutex);
        return -1;
    }

#else

    pthread_mutex_lock(&m->mutex);

    if (pthread_create(&m->thread_id, NULL, thread, m) < 0) {
        pthread_mutex_unlock(&m->mutex);
        return -1;
    }

#endif

    m->thread_running = 1;
    
#ifdef OS_IS_WIN32
    LeaveCriticalSection(&m->mutex);
#else
    pthread_mutex_unlock(&m->mutex);
#endif

    return 0;
}

void pa_threaded_mainloop_stop(pa_threaded_mainloop *m) {
    assert(m);

    if (!m->thread_running)
        return;

    /* Make sure that this function is not called from the helper thread */
    assert(!in_worker(m));

#ifdef OS_IS_WIN32
    EnterCriticalSection(&m->mutex);
#else
    pthread_mutex_lock(&m->mutex);
#endif

    pa_mainloop_quit(m->real_mainloop, 0);

#ifdef OS_IS_WIN32
    LeaveCriticalSection(&m->mutex);
#else
    pthread_mutex_unlock(&m->mutex);
#endif

#ifdef OS_IS_WIN32
    WaitForSingleObject(m->thread, INFINITE);
    CloseHandle(m->thread);
#else
    pthread_join(m->thread_id, NULL);
#endif

    m->thread_running = 0;

    return;
}

void pa_threaded_mainloop_lock(pa_threaded_mainloop *m) {
    assert(m);
    
    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !in_worker(m));

#ifdef OS_IS_WIN32
    EnterCriticalSection(&m->mutex);
#else
    pthread_mutex_lock(&m->mutex);
#endif
}

void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) {
    assert(m);
    
    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !in_worker(m));

#ifdef OS_IS_WIN32
    LeaveCriticalSection(&m->mutex);
#else
    pthread_mutex_unlock(&m->mutex);
#endif
}

void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_accept) {
#ifdef OS_IS_WIN32
    void *iter;
    const void *key;
    HANDLE event;
#endif

    assert(m);

#ifdef OS_IS_WIN32

    iter = NULL;
    while (1) {
        pa_hashmap_iterate(m->cond_events, &iter, &key);
        if (key == NULL)
            break;
        event = (HANDLE)pa_hashmap_get(m->cond_events, key);
        SetEvent(event);
    }

#else

    pthread_cond_broadcast(&m->cond);

#endif

    if (wait_for_accept && m->n_waiting > 0) {

#ifdef OS_IS_WIN32

        /* This is just to make sure it's unsignaled */
        WaitForSingleObject(m->accept_cond, 0);

        LeaveCriticalSection(&m->mutex);

        WaitForSingleObject(m->accept_cond, INFINITE);

        EnterCriticalSection(&m->mutex);

#else

        pthread_cond_wait(&m->accept_cond, &m->mutex);

#endif

    }
}

void pa_threaded_mainloop_wait(pa_threaded_mainloop *m) {
#ifdef OS_IS_WIN32
    HANDLE event;
    DWORD result;
#endif

    assert(m);
    
    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !in_worker(m));

    m->n_waiting ++;

#ifdef OS_IS_WIN32

    event = CreateEvent(NULL, FALSE, FALSE, NULL);
    assert(event);

    pa_hashmap_put(m->cond_events, event, event);

    LeaveCriticalSection(&m->mutex);

    result = WaitForSingleObject(event, INFINITE);
    assert(result == WAIT_OBJECT_0);

    EnterCriticalSection(&m->mutex);

    pa_hashmap_remove(m->cond_events, event);

    CloseHandle(event);

#else

    pthread_cond_wait(&m->cond, &m->mutex);

#endif

    assert(m->n_waiting > 0);
    m->n_waiting --;
}

void pa_threaded_mainloop_accept(pa_threaded_mainloop *m) {
    assert(m);
    
    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !in_worker(m));

#ifdef OS_IS_WIN32
    SetEvent(m->accept_cond);
#else
    pthread_cond_signal(&m->accept_cond);
#endif
}

int pa_threaded_mainloop_get_retval(pa_threaded_mainloop *m) {
    assert(m);

    return pa_mainloop_get_retval(m->real_mainloop);
}

pa_mainloop_api* pa_threaded_mainloop_get_api(pa_threaded_mainloop*m) {
    assert(m);

    return pa_mainloop_get_api(m->real_mainloop);
}

#else /* defined(OS_IS_WIN32) || defined(HAVE_PTHREAD) */

pa_threaded_mainloop *pa_threaded_mainloop_new(void) {
	pa_log_error(__FILE__": Threaded main loop not supported on this platform");
	return NULL;
}

void pa_threaded_mainloop_free(pa_threaded_mainloop* m) {
	assert(0);
}

int pa_threaded_mainloop_start(pa_threaded_mainloop *m) {
	assert(0);
	return -1;
}

void pa_threaded_mainloop_stop(pa_threaded_mainloop *m) {
	assert(0);
}

void pa_threaded_mainloop_lock(pa_threaded_mainloop *m) {
	assert(0);
}

void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) {
	assert(0);
}

void pa_threaded_mainloop_wait(pa_threaded_mainloop *m) {
	assert(0);
}

void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_release) {
	assert(0);
}

int pa_threaded_mainloop_get_retval(pa_threaded_mainloop *m) {
	assert(0);
}

pa_mainloop_api* pa_threaded_mainloop_get_api(pa_threaded_mainloop*m) {
	assert(0);
	return NULL;
}

#endif /* defined(OS_IS_WIN32) || defined(HAVE_PTHREAD) */
