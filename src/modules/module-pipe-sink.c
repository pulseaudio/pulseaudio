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
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>

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
    char *filename;
    int fd;
    pa_thread *thread;
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

enum {
    POLLFD_ASYNCQ,
    POLLFD_FIFO,
    POLLFD_MAX,
};

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    int quit = 0;
    struct pollfd pollfd[POLLFD_MAX];
    int running = 1, underrun = 0;
    pa_memchunk memchunk;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    memset(&pollfd, 0, sizeof(pollfd));
    pollfd[POLLFD_ASYNCQ].fd = pa_asyncmsgq_get_fd(u->sink->asyncmsgq, PA_ASYNCQ_POP);
    pollfd[POLLFD_ASYNCQ].events = POLLIN;

    pollfd[POLLFD_FIFO].fd = u->fd;

    memset(&memchunk, 0, sizeof(memchunk));

    for (;;) {
        int code;
        void *object, *data;
        int r;
        struct timeval now;

        /* Check whether there is a message for us to process */
        if (pa_asyncmsgq_get(u->sink->asyncmsgq, &object, &code, &data) == 0) {


            /* Now process these messages our own way */
            if (!object) {
                switch (code) {
                    case PA_SINK_MESSAGE_SHUTDOWN:
                        goto finish;

                    default:
                        pa_sink_process_msg(u->sink->asyncmsgq, object, code, data);
                }

            } else if (object == u->sink) {

                case PA_SINK_MESSAGE_STOP:
                    pa_assert(running);
                    running = 0;
                    break;

                case PA_SINK_MESSAGE_START:
                    pa_assert(!running);
                    running = 1;
                    break;

                case PA_SINK_MESSAGE_GET_LATENCY: {
                    size_t n = 0;
                    int l;

                    if (ioctl(u->fd, TIOCINQ, &l) >= 0 && l > 0)
                        n = (size_t) l;

                    n += memchunk.length;

                    *((pa_usec_t*) data) pa_bytes_to_usec(n, &u->sink->sample_spec);
                    break;
                }

                /* ... */

                default:
                    pa_sink_process_msg(u->sink->asyncmsgq, object, code, data);
            }

            pa_asyncmsgq_done(u->sink->asyncmsgq);
            continue;
        }

        /* Render some data and write it to the fifo */

        if (running && (pollfd[POLLFD_FIFO].revents || underrun)) {

            if (chunk.length <= 0)
                pa_sink_render(u->fd, PIPE_BUF, &chunk);

            underrun = chunk.length <= 0;

            if (!underrun) {
                ssize_t l;

                p = pa_memblock_acquire(u->memchunk.memblock);
                l = pa_write(u->fd, (uint8_t*) p + u->memchunk.index, u->memchunk.length);
                pa_memblock_release(p);

                if (l < 0) {

                    if (errno != EINTR && errno != EAGAIN) {
                        pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
                        goto fail;
                    }

                } else {

                    u->memchunk.index += l;
                    u->memchunk.length -= l;

                    if (u->memchunk.length <= 0) {
                        pa_memblock_unref(u->memchunk.memblock);
                        u->memchunk.memblock = NULL;
                    }
                }

                pollfd[POLLFD_FIFO].revents = 0;
                continue;
            }
        }

        pollfd[POLLFD_FIFO].events = running && !underrun ? POLLOUT : 0;

        /* Hmm, nothing to do. Let's sleep */

        if (pa_asyncmsgq_before_poll(u->sink->asyncmsgq) < 0)
            continue;

        r = poll(&pollfd, 1, 0);
        pa_asyncmsgq_after_poll(u->sink->asyncmsgq);

        if (r < 0) {
            if (errno == EINTR)
                continue;

            pa_log("poll() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        if (pollfd[POLLFD_FIFO].revents & ~POLLIN) {
            pa_log("FIFO shutdown.");
            goto fail;
        }

        pa_assert(pollfd[POLLFD_ASYNCQ].revents & ~POLLIN == 0);
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->core->asyncmsgq, u->core, PA_CORE_MESSAGE_UNLOAD_MODULE, pa_module_ref(u->module), pa_module_unref);
    pa_asyncmsgq_wait_for(PA_SINK_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    struct stat st;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    char *t;

    pa_assert(c);
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = c;
    u->module = m;
    u->filename = pa_xstrdup(pa_modargs_get_value(ma, "file", DEFAULT_FIFO_NAME));
    u->fd = fd;
    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;
    m->userdata = u;

    mkfifo(u->filename, 0666);

    if ((u->fd = open(u->filename, O_RDWR)) < 0) {
        pa_log("open('%s'): %s", p, pa_cstrerror(errno));
        goto fail;
    }

    pa_fd_set_cloexec(u->fd, 1);
    pa_make_nonblock_fd(u->fd);

    if (fstat(u->fd, &st) < 0) {
        pa_log("fstat('%s'): %s", p, pa_cstrerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        pa_log("'%s' is not a FIFO.", p);
        goto fail;
    }

    if (!(u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Unix FIFO sink '%s'", p));
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
