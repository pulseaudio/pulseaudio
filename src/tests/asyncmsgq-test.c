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

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include <pulse/util.h>
#include <pulse/xmalloc.h>
#include <pulsecore/asyncmsgq.h>
#include <pulsecore/thread.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

enum {
    OPERATION_A,
    OPERATION_B,
    OPERATION_C,
    QUIT
};

static void the_thread(void *_q) {
    pa_asyncmsgq *q = _q;
    int quit = 0;

    do {
        int code = 0;

        pa_assert_se(pa_asyncmsgq_get(q, NULL, &code, NULL, NULL, NULL, 1) == 0);

        switch (code) {

            case OPERATION_A:
                printf("Operation A\n");
                break;

            case OPERATION_B:
                printf("Operation B\n");
                break;

            case OPERATION_C:
                printf("Operation C\n");
                break;

            case QUIT:
                printf("quit\n");
                quit = 1;
                break;
        }

        pa_asyncmsgq_done(q, 0);

    } while (!quit);
}

int main(int argc, char *argv[]) {
    pa_asyncmsgq *q;
    pa_thread *t;

    pa_assert_se(q = pa_asyncmsgq_new(0));

    pa_assert_se(t = pa_thread_new(the_thread, q));

    printf("Operation A post\n");
    pa_asyncmsgq_post(q, NULL, OPERATION_A, NULL, 0, NULL, NULL);

    pa_thread_yield();

    printf("Operation B post\n");
    pa_asyncmsgq_post(q, NULL, OPERATION_B, NULL, 0, NULL, NULL);

    pa_thread_yield();

    printf("Operation C send\n");
    pa_asyncmsgq_send(q, NULL, OPERATION_C, NULL, 0, NULL);

    pa_thread_yield();

    printf("Quit post\n");
    pa_asyncmsgq_post(q, NULL, QUIT, NULL, 0, NULL, NULL);

    pa_thread_free(t);

    pa_asyncmsgq_unref(q);

    return 0;
}
