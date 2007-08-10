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
#include <sys/ioctl.h>
#include <sys/poll.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>

#include "module-pipe-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("UNIX pipe sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "file=<path of the FIFO> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate>"
        "channel_map=<channel map>")

#define DEFAULT_FILE_NAME "/tmp/music.output"
#define DEFAULT_SINK_NAME "fifo_output"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_thread *thread;
    pa_asyncmsgq *asyncmsgq;
    char *filename;
    int fd;

    pa_memchunk memchunk;
};

static const char* const valid_modargs[] = {
    "file",
    "rate",
    "format",
    "channels",
    "sink_name",
    "channel_map",
    NULL
};

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
            
        case PA_SINK_MESSAGE_GET_LATENCY: {
            size_t n = 0;
            int l;
            
            if (ioctl(u->fd, TIOCINQ, &l) >= 0 && l > 0)
                n = (size_t) l;
            
            n += u->memchunk.length;
            
            *((pa_usec_t*) data) = pa_bytes_to_usec(n, &u->sink->sample_spec);
            break;
        }
    }
    
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void thread_func(void *userdata) {
    enum {
        POLLFD_ASYNCQ,
        POLLFD_FIFO,
        POLLFD_MAX,
    };
    
    struct userdata *u = userdata;
    struct pollfd pollfd[POLLFD_MAX];
    int write_type = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    memset(&pollfd, 0, sizeof(pollfd));
    
    pollfd[POLLFD_ASYNCQ].fd = pa_asyncmsgq_get_fd(u->asyncmsgq);
    pollfd[POLLFD_ASYNCQ].events = POLLIN;
    pollfd[POLLFD_FIFO].fd = u->fd;

    for (;;) {
        pa_msgobject *object;
        int code;
        void *data;
        pa_memchunk chunk;
        int r;
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

        /* Render some data and write it to the fifo */

        if (u->sink->thread_info.state == PA_SINK_RUNNING && pollfd[POLLFD_FIFO].revents) {
            ssize_t l;
            void *p;

            if (u->memchunk.length <= 0)
                pa_sink_render(u->sink, PIPE_BUF, &u->memchunk);

            pa_assert(u->memchunk.length > 0);

            p = pa_memblock_acquire(u->memchunk.memblock);
            l = pa_write(u->fd, (uint8_t*) p + u->memchunk.index, u->memchunk.length, &write_type);
            pa_memblock_release(u->memchunk.memblock);

            pa_assert(l != 0);

            if (l < 0) {

                if (errno == EINTR)
                    continue;
                else if (errno != EAGAIN) {
                    pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
                    goto fail;
                }

            } else {

                u->memchunk.index += l;
                u->memchunk.length -= l;

                if (u->memchunk.length <= 0) {
                    pa_memblock_unref(u->memchunk.memblock);
                    pa_memchunk_reset(&u->memchunk);
                }

                pollfd[POLLFD_FIFO].revents = 0;
                continue;
            }
        }

        pollfd[POLLFD_FIFO].events = u->sink->thread_info.state == PA_SINK_RUNNING ? POLLOUT : 0;

        /* Hmm, nothing to do. Let's sleep */

        if (pa_asyncmsgq_before_poll(u->asyncmsgq) < 0)
            continue;

/*         pa_log("polling for %u", pollfd[POLLFD_FIFO].events);  */
        r = poll(pollfd, POLLFD_MAX, -1);
/*         pa_log("polling got %u", r > 0 ? pollfd[POLLFD_FIFO].revents : 0);  */

        pa_asyncmsgq_after_poll(u->asyncmsgq);

        if (r < 0) {
            if (errno == EINTR) {
                pollfd[POLLFD_ASYNCQ].revents = 0;
                pollfd[POLLFD_FIFO].revents = 0;
                continue;
            }

            pa_log("poll() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        if (pollfd[POLLFD_FIFO].revents & ~POLLOUT) {
            pa_log("FIFO shutdown.");
            goto fail;
        }

        pa_assert((pollfd[POLLFD_ASYNCQ].revents & ~POLLIN) == 0);
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->core->asyncmsgq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->asyncmsgq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_module*m) {
    struct userdata *u;
    struct stat st;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    char *t;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    pa_memchunk_reset(&u->memchunk);

    pa_assert_se(u->asyncmsgq = pa_asyncmsgq_new(0));
    
    u->filename = pa_xstrdup(pa_modargs_get_value(ma, "file", DEFAULT_FILE_NAME));

    mkfifo(u->filename, 0666);
    if ((u->fd = open(u->filename, O_RDWR|O_NOCTTY)) < 0) {
        pa_log("open('%s'): %s", u->filename, pa_cstrerror(errno));
        goto fail;
    }

    pa_fd_set_cloexec(u->fd, 1);
    pa_make_nonblock_fd(u->fd);

    if (fstat(u->fd, &st) < 0) {
        pa_log("fstat('%s'): %s", u->filename, pa_cstrerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        pa_log("'%s' is not a FIFO.", u->filename);
        goto fail;
    }

    if (!(u->sink = pa_sink_new(m->core, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;
    
    pa_sink_set_module(u->sink, m);
    pa_sink_set_asyncmsgq(u->sink, u->asyncmsgq);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Unix FIFO sink '%s'", u->filename));
    pa_xfree(t);

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    
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

    if (u->memchunk.memblock)
       pa_memblock_unref(u->memchunk.memblock);

    if (u->filename) {
        unlink(u->filename);
        pa_xfree(u->filename);
    }

    if (u->fd >= 0)
        close(u->fd);

    pa_xfree(u);
}
