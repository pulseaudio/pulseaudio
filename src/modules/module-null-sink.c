/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/poll.h>

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/macro.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>

#include "module-null-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Clocked NULL sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "sink_name=<name of sink>"
        "channel_map=<channel map>"
        "description=<description for the sink>")

#define DEFAULT_SINK_NAME "null"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_thread *thread;
    pa_asyncmsgq *asyncmsgq;
    size_t block_size;
    
    struct timeval timestamp;
};

static const char* const valid_modargs[] = {
    "rate",
    "format",
    "channels",
    "sink_name",
    "channel_map",
    "description",
    NULL
};

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_SET_STATE:

            if (PA_PTR_TO_UINT(data) == PA_SINK_RUNNING)
                pa_gettimeofday(&u->timestamp);
            
            break;
            
        case PA_SINK_MESSAGE_GET_LATENCY: {
            struct timeval now;
    
            pa_gettimeofday(&now);
            
            if (pa_timeval_cmp(&u->timestamp, &now) > 0)
                *((pa_usec_t*) data) = 0;
            else
                *((pa_usec_t*) data) = pa_timeval_diff(&u->timestamp, &now);
            break;
        }
    }
    
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    struct pollfd pollfd;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_gettimeofday(&u->timestamp);

    memset(&pollfd, 0, sizeof(pollfd));
    pollfd.fd = pa_asyncmsgq_get_fd(u->asyncmsgq);
    pollfd.events = POLLIN;

    for (;;) {
        pa_msgobject *object;
        int code;
        void *data;
        pa_memchunk chunk;
        int r, timeout;
        struct timeval now;
        int64_t offset;

        /* Check whether there is a message for us to process */
        if (pa_asyncmsgq_get(u->asyncmsgq, &object, &code, &data, &offset, &chunk, 0) == 0) {
            int ret;

            if (!object && code == PA_MESSAGE_SHUTDOWN) {
                pa_asyncmsgq_done(u->asyncmsgq, 0);
                goto finish;
            }

            ret = pa_asyncmsgq_dispatch(object, code, data, offset, &chunk);
            pa_asyncmsgq_done(u->asyncmsgq, ret);
            continue;
        }

        /* Render some data and drop it immediately */
        if (u->sink->thread_info.state == PA_SINK_RUNNING) {
            pa_gettimeofday(&now);

            if (pa_timeval_cmp(&u->timestamp, &now) <= 0) {

                pa_sink_render(u->sink, u->block_size, &chunk);
                pa_memblock_unref(chunk.memblock);

                pa_timeval_add(&u->timestamp, pa_bytes_to_usec(chunk.length, &u->sink->sample_spec));
                continue;
            }

            timeout = pa_timeval_diff(&u->timestamp, &now)/1000;

            if (timeout < 1)
                timeout = 1;
        } else
            timeout = -1;

        /* Hmm, nothing to do. Let's sleep */

        if (pa_asyncmsgq_before_poll(u->asyncmsgq) < 0)
            continue;

        r = poll(&pollfd, 1, timeout);
        pa_asyncmsgq_after_poll(u->asyncmsgq);

        if (r < 0) {
            if (errno == EINTR) {
                pollfd.revents = 0;
                continue;
            }

            pa_log("poll() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        pa_assert(r == 0 || pollfd.revents == POLLIN);
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->core->asyncmsgq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->asyncmsgq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;

    pa_assert(c);
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = c;
    u->module = m;
    m->userdata = u;

    pa_assert_se(u->asyncmsgq = pa_asyncmsgq_new(0));
    
    if (!(u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;

    pa_sink_set_module(u->sink, m);
    pa_sink_set_asyncmsgq(u->sink, u->asyncmsgq);
    pa_sink_set_description(u->sink, pa_modargs_get_value(ma, "description", "NULL sink"));

    u->block_size = pa_bytes_per_second(&ss) / 20; /* 50 ms */
    if (u->block_size <= 0)
        u->block_size = pa_frame_size(&ss);
    
    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(c, m);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;

    pa_assert(c);
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_disconnect(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->asyncmsgq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    if (u->asyncmsgq)
        pa_asyncmsgq_free(u->asyncmsgq);

    if (u->sink)
        pa_sink_unref(u->sink);

    pa_xfree(u);
}
