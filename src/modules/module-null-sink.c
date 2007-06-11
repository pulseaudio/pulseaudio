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

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/macro.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>

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

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    int quit = 0;
    struct pollfd pollfd;
    int running = 1;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    memset(&pollfd, 0, sizeof(pollfd));
    pollfd.fd = pa_asyncmsgq_get_fd(u->sink->asyncmsgq, PA_ASYNCQ_POP);
    pollfd.events = POLLIN;

    pa_gettimeofday(u->timestamp);
    
    for (;;) {
        int code;
        void *data, *object;
        int r, timeout;
        struct timeval now;

        /* Check whether there is a message for us to process */
        if (pa_asyncmsgq_get(u->sink->asyncmsgq, &object, &code, &data) == 0) {


            /* Now process these messages our own way */
            if (!object) {

                switch (code) {
                    case PA_MESSAGE_SHUTDOWN:
                        goto finish;

                    default:
                        pa_sink_process_msg(u->sink->asyncmsgq, object, code, data);

                }
                
            } else if (object == u->sink) {

                switch (code) {
                    case PA_SINK_MESSAGE_STOP:
                        pa_assert(running);
                        running = 0;
                        break;
                        
                    case PA_SINK_MESSAGE_START:
                        pa_assert(!running);
                        running = 1;
                        
                        pa_gettimeofday(u->timestamp);
                        break;
                        
                    case PA_SINK_MESSAGE_GET_LATENCY:
                        
                        if (pa_timeval_cmp(&u->timestamp, &now) > 0)
                            *((pa_usec_t*) data) = 0;
                        else
                            *((pa_usec_t*) data) = pa_timeval_diff(&u->timestamp, &now);
                        break;
                        
                        /* ... */

                    default:
                        pa_sink_process_msg(u->sink->asyncmsgq, object, code, data);
                }
            }
            
            pa_asyncmsgq_done(u->sink->asyncmsgq);
            continue;
        }

        /* Render some data and drop it immediately */

        if (running) {
            pa_gettimeofday(&now);
            
            if (pa_timeval_cmp(u->timestamp, &now) <= 0) {
                pa_memchunk chunk;
                size_t l;
                
                if (pa_sink_render(u->sink, u->block_size, &chunk) >= 0) {
                    l = chunk.length;
                    pa_memblock_unref(chunk.memblock);
                } else
                    l = u->block_size;
                
                pa_timeval_add(&u->timestamp, pa_bytes_to_usec(l, &u->sink->sample_spec));
                continue;
            }

            timeout = pa_timeval_diff(&u->timestamp, &now)/1000;
            
            if (timeout < 1)
                timeout = 1;
        } else
            timeout = -1;

        /* Hmm, nothing to do. Let's sleep */
        
        if (pa_asyncmsgq_before_poll(u->sink->asyncmsgq) < 0)
            continue;

        r = poll(&pollfd, 1, timeout);
        pa_asyncmsgq_after_poll(u->sink->asyncmsgq);

        if (r < 0) {
            if (errno == EINTR)
                continue;

            pa_log("poll() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
        
        pa_assert(r == 0 || pollfd.revents == POLLIN);
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->core->asyncmsgq, u->core, PA_CORE_MESSAGE_UNLOAD_MODULE, pa_module_ref(u->module), NULL, pa_module_unref);
    pa_asyncmsgq_wait_for(PA_MESSAGE_SHUTDOWN);

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

    if (!(u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
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

    pa_sink_disconnect(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->sink->asyncmsgq, PA_SINK_MESSAGE_SHUTDOWN, NULL);
        pa_thread_free(u->thread);
    }
    
    pa_sink_unref(u->sink);

    pa_xfree(u);
}
