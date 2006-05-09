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

#include <polypcore/xmalloc.h>

#include "mainloop.h"
#include "thread-mainloop.h"

/* FIXME: Add defined(OS_IS_WIN32) when support is added */
#if defined(HAVE_PTHREAD)

struct pa_threaded_mainloop {
    pa_mainloop *real_mainloop;
    pthread_t thread_id;
    pthread_mutex_t mutex;
    int n_waiting;
    pthread_cond_t cond, release_cond;
    int thread_running;
};

static int poll_func(struct pollfd *ufds, unsigned long nfds, int timeout, void *userdata) {
    pthread_mutex_t *mutex = userdata;
    int r;

    assert(mutex);
    
    /* Before entering poll() we unlock the mutex, so that
     * avahi_simple_poll_quit() can succeed from another thread. */

    pthread_mutex_unlock(mutex);
    r = poll(ufds, nfds, timeout);
    pthread_mutex_lock(mutex);

    return r;
}

static void* thread(void *userdata){
    pa_threaded_mainloop *m = userdata;
    sigset_t mask;

    /* Make sure that signals are delivered to the main thread */
    sigfillset(&mask);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    pthread_mutex_lock(&m->mutex);
    pa_mainloop_run(m->real_mainloop, NULL);
    pthread_mutex_unlock(&m->mutex);

    return NULL;
}

pa_threaded_mainloop *pa_threaded_mainloop_new(void) {
    pa_threaded_mainloop *m;
    pthread_mutexattr_t a;

    m = pa_xnew(pa_threaded_mainloop, 1);

    if (!(m->real_mainloop = pa_mainloop_new())) {
        pa_xfree(m);
        return NULL;
    }

    pa_mainloop_set_poll_func(m->real_mainloop, poll_func, &m->mutex);

    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mutex, &a);
    pthread_mutexattr_destroy(&a);
    
    pthread_cond_init(&m->cond, NULL);
    pthread_cond_init(&m->release_cond, NULL);
    m->thread_running = 0;
    m->n_waiting = 0;

    return m;
}

void pa_threaded_mainloop_free(pa_threaded_mainloop* m) {
    assert(m);

    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !pthread_equal(pthread_self(), m->thread_id));

    if (m->thread_running)
        pa_threaded_mainloop_stop(m);

    if (m->real_mainloop)
        pa_mainloop_free(m->real_mainloop);

    pthread_mutex_destroy(&m->mutex);
    pthread_cond_destroy(&m->cond);
    pthread_cond_destroy(&m->release_cond);
    
    pa_xfree(m);
}

int pa_threaded_mainloop_start(pa_threaded_mainloop *m) {
    assert(m);

    assert(!m->thread_running);

    pthread_mutex_lock(&m->mutex);

    if (pthread_create(&m->thread_id, NULL, thread, m) < 0) {
        pthread_mutex_unlock(&m->mutex);
        return -1;
    }

    m->thread_running = 1;
    
    pthread_mutex_unlock(&m->mutex);

    return 0;
}

void pa_threaded_mainloop_stop(pa_threaded_mainloop *m) {
    assert(m);

    if (!m->thread_running)
        return;

    /* Make sure that this function is not called from the helper thread */
    assert(!pthread_equal(pthread_self(), m->thread_id));

    pthread_mutex_lock(&m->mutex);
    pa_mainloop_quit(m->real_mainloop, 0);
    pthread_mutex_unlock(&m->mutex);

    pthread_join(m->thread_id, NULL);
    m->thread_running = 0;

    return;
}

void pa_threaded_mainloop_lock(pa_threaded_mainloop *m) {
    assert(m);
    
    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !pthread_equal(pthread_self(), m->thread_id));

    pthread_mutex_lock(&m->mutex);
}

void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) {
    assert(m);
    
    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !pthread_equal(pthread_self(), m->thread_id));

    pthread_mutex_unlock(&m->mutex);
}

void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_release) {
    assert(m);
    
    pthread_cond_broadcast(&m->cond);

    if (wait_for_release && m->n_waiting > 0)
        pthread_cond_wait(&m->release_cond, &m->mutex);
}

void pa_threaded_mainloop_wait(pa_threaded_mainloop *m) {
    assert(m);
    
    /* Make sure that this function is not called from the helper thread */
    assert(!m->thread_running || !pthread_equal(pthread_self(), m->thread_id));

    m->n_waiting ++;
    pthread_cond_wait(&m->cond, &m->mutex);
    assert(m->n_waiting > 0);
    m->n_waiting --;
    pthread_cond_signal(&m->release_cond);
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
