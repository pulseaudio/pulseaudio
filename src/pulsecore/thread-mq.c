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

#include <unistd.h>
#include <errno.h>

#include <pulse/xmalloc.h>

#include <pulsecore/atomic.h>
#include <pulsecore/once.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/semaphore.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/flist.h>

#include "thread-mq.h"

PA_STATIC_TLS_DECLARE_NO_FREE(thread_mq);

static void asyncmsgq_cb(pa_mainloop_api*api, pa_io_event* e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_thread_mq *q = userdata;
    pa_asyncmsgq *aq;

    pa_assert(pa_asyncmsgq_get_fd(q->outq) == fd);
    pa_assert(events == PA_IO_EVENT_INPUT);

    pa_asyncmsgq_ref(aq = q->outq);
    pa_asyncmsgq_after_poll(aq);

    for (;;) {
        pa_msgobject *object;
        int code;
        void *data;
        int64_t offset;
        pa_memchunk chunk;

        /* Check whether there is a message for us to process */
        while (pa_asyncmsgq_get(aq, &object, &code, &data, &offset, &chunk, 0) == 0) {
            int ret;

            ret = pa_asyncmsgq_dispatch(object, code, data, offset, &chunk);
            pa_asyncmsgq_done(aq, ret);
        }

        if (pa_asyncmsgq_before_poll(aq) == 0)
            break;
    }

    pa_asyncmsgq_unref(aq);
}

void pa_thread_mq_init(pa_thread_mq *q, pa_mainloop_api *mainloop) {
    pa_assert(q);
    pa_assert(mainloop);

    q->mainloop = mainloop;
    pa_assert_se(q->inq = pa_asyncmsgq_new(0));
    pa_assert_se(q->outq = pa_asyncmsgq_new(0));

    pa_assert_se(pa_asyncmsgq_before_poll(q->outq) == 0);
    pa_assert_se(q->io_event = mainloop->io_new(mainloop, pa_asyncmsgq_get_fd(q->outq), PA_IO_EVENT_INPUT, asyncmsgq_cb, q));
}

void pa_thread_mq_done(pa_thread_mq *q) {
    pa_assert(q);

    q->mainloop->io_free(q->io_event);
    q->io_event = NULL;

    pa_asyncmsgq_unref(q->inq);
    pa_asyncmsgq_unref(q->outq);
    q->inq = q->outq = NULL;

    q->mainloop = NULL;
}

void pa_thread_mq_install(pa_thread_mq *q) {
    pa_assert(q);

    pa_assert(!(PA_STATIC_TLS_GET(thread_mq)));
    PA_STATIC_TLS_SET(thread_mq, q);
}

pa_thread_mq *pa_thread_mq_get(void) {
    return PA_STATIC_TLS_GET(thread_mq);
}
