/***
  This file is part of PulseAudio.

  Copyright 2014 David Henningsson, Canonical Ltd.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

/* This test spawns two threads on distinct cpu-cores that pass a value
 * between each other through shared memory protected by pa_atomic_t.
 * Thread "left" continuously increments a value and writes its contents to memory.
 * Thread "right" continuously reads the value and checks whether it was incremented.
 *
 * With the pa_atomic_load/pa_atomic_store implementations based on __sync_synchronize,
 * this will fail after some time (sometimes 2 seconds, sometimes 8 hours) at least
 * on ARM Cortex-A53 and ARM Cortex-A57 systems.
 *
 * On x86_64, it does not.
 *
 * The chosen implementation in some way mimics a situation that can also occur
 * using memfd srbchannel transport.
 *
 * NOTE: This is a long-running test, so don't execute in normal test suite.
 *
 * */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <check.h>

#include <pulsecore/thread.h>
#include <pulse/rtclock.h>
#include <pulse/xmalloc.h>
#include <pulsecore/semaphore.h>
#include <pthread.h>
#include <pulsecore/atomic.h>

#define MEMORY_SIZE (8 * 2 * 1024 * 1024)


typedef struct io_t {
   pa_atomic_t *flag;
   char* memory;
   cpu_set_t cpuset;
} io_t;

static void read_func(void* data) {
   io_t *io = (io_t *) data;
   size_t expect = 0;
   size_t value = 0;
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &io->cpuset);
   while(1) {
      if(pa_atomic_load(io->flag) == 1) {
         memcpy(&value, io->memory, sizeof(value));
         pa_atomic_sub(io->flag, 1);
         ck_assert_uint_eq(value, expect);
         ++expect;
      }
   }
}

static void write_func(void* data) {
   io_t *io = (io_t *) data;
   size_t value = 0;
   pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &io->cpuset);
   while(1) {
      if(pa_atomic_load(io->flag) == 0) {
         memcpy(io->memory, &value, sizeof(value));
         pa_atomic_add(io->flag, 1);
         ++value;
      }
   }
}

START_TEST (atomic_test) {
    pa_thread *thread1, *thread2;
    io_t io1, io2;

    char* memory = pa_xmalloc0(MEMORY_SIZE);
    pa_atomic_t flag = PA_ATOMIC_INIT(0);
    memset(memory, 0, MEMORY_SIZE);

    /* intentionally misalign memory since srbchannel also does not
     * always read/write aligned. Might be a red hering. */
    io1.memory = io2.memory = memory + 1025;
    io1.flag = io2.flag = &flag;

    CPU_ZERO(&io1.cpuset);
    CPU_SET(1, &io1.cpuset);
    thread1 = pa_thread_new("left", &write_func, &io1);

    CPU_ZERO(&io2.cpuset);
    CPU_SET(3, &io2.cpuset);
    thread2 = pa_thread_new("right", &read_func, &io2);
    pa_thread_free(thread1);
    pa_thread_free(thread2);
    pa_xfree(memory);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("atomic");
    tc = tcase_create("atomic");
    tcase_add_test(tc, atomic_test);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
