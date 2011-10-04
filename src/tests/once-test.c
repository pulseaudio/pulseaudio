/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#include <pulsecore/thread.h>
#include <pulsecore/once.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/atomic.h>
#include <pulse/xmalloc.h>

static pa_once once = PA_ONCE_INIT;
static volatile unsigned n_run = 0;
static const char * volatile ran_by = NULL;
#ifdef HAVE_PTHREAD
static pthread_barrier_t barrier;
#endif
static unsigned n_cpu;

#define N_ITERATIONS 500
#define N_THREADS 100

static void once_func(void) {
    n_run++;
    ran_by = (const char*) pa_thread_get_data(pa_thread_self());
}

static void thread_func(void *data) {
#ifdef HAVE_PTHREAD
    int r;

#ifdef HAVE_PTHREAD_SETAFFINITY_NP
    static pa_atomic_t i_cpu = PA_ATOMIC_INIT(0);
    cpu_set_t mask;

    CPU_ZERO(&mask);
    CPU_SET((size_t) (pa_atomic_inc(&i_cpu) % n_cpu), &mask);
    pa_assert_se(pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) == 0);
#endif

    pa_log_debug("started up: %s", (char *) data);

    r = pthread_barrier_wait(&barrier);
    pa_assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
#endif /* HAVE_PTHREAD */

    pa_run_once(&once, once_func);
}

int main(int argc, char *argv[]) {
    unsigned n, i;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    n_cpu = pa_ncpus();

    for (n = 0; n < N_ITERATIONS; n++) {
        pa_thread* threads[N_THREADS];

#ifdef HAVE_PTHREAD
        pa_assert_se(pthread_barrier_init(&barrier, NULL, N_THREADS) == 0);
#endif

        /* Yes, kinda ugly */
        pa_zero(once);

        for (i = 0; i < N_THREADS; i++)
            threads[i] = pa_thread_new("once", thread_func, pa_sprintf_malloc("Thread #%i", i+1));

        for (i = 0; i < N_THREADS; i++)
            pa_thread_join(threads[i]);

        pa_assert(n_run == 1);
        pa_log_info("ran by %s", ran_by);

        for (i = 0; i < N_THREADS; i++) {
            pa_xfree(pa_thread_get_data(threads[i]));
            pa_thread_free(threads[i]);
        }

        n_run = 0;
        ran_by = NULL;

#ifdef HAVE_PTHREAD
        pa_assert_se(pthread_barrier_destroy(&barrier) == 0);
#endif
    }

    return 0;
}
