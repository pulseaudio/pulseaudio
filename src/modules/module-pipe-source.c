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

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>

#include "module-pipe-source-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("UNIX pipe source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "source_name=<name for the source> "
        "file=<path of the FIFO> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map>");

#define DEFAULT_FILE_NAME "/tmp/music.input"
#define DEFAULT_SOURCE_NAME "fifo_input"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    char *filename;
    int fd;

    pa_memchunk memchunk;

    pa_rtpoll_item *rtpoll_item;
};

static const char* const valid_modargs[] = {
    "file",
    "rate",
    "channels",
    "format",
    "source_name",
    "channel_map",
    NULL
};

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    int read_type = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    for (;;) {
        int ret;
        struct pollfd *pollfd;

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

        /* Try to read some data and pass it on to the source driver */
        if (u->source->thread_info.state == PA_SOURCE_RUNNING && pollfd->revents) {
            ssize_t l;
            void *p;

            if (!u->memchunk.memblock) {
                u->memchunk.memblock = pa_memblock_new(u->core->mempool, PIPE_BUF);
                u->memchunk.index = u->memchunk.length = 0;
            }

            pa_assert(pa_memblock_get_length(u->memchunk.memblock) > u->memchunk.index);

            p = pa_memblock_acquire(u->memchunk.memblock);
            l = pa_read(u->fd, (uint8_t*) p + u->memchunk.index, pa_memblock_get_length(u->memchunk.memblock) - u->memchunk.index, &read_type);
            pa_memblock_release(u->memchunk.memblock);

            pa_assert(l != 0); /* EOF cannot happen, since we opened the fifo for both reading and writing */

            if (l < 0) {

                if (errno == EINTR)
                    continue;
                else if (errno != EAGAIN) {
                    pa_log("Faile to read data from FIFO: %s", pa_cstrerror(errno));
                    goto fail;
                }

            } else {

                u->memchunk.length = l;
                pa_source_post(u->source, &u->memchunk);
                u->memchunk.index += l;

                if (u->memchunk.index >= pa_memblock_get_length(u->memchunk.memblock)) {
                    pa_memblock_unref(u->memchunk.memblock);
                    pa_memchunk_reset(&u->memchunk);
                }

                pollfd->revents = 0;
            }
        }

        /* Hmm, nothing to do. Let's sleep */
        pollfd->events = u->source->thread_info.state == PA_SOURCE_RUNNING ? POLLIN : 0;

        if ((ret = pa_rtpoll_run(u->rtpoll, 1)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
        if (pollfd->revents & ~POLLIN) {
            pa_log("FIFO shutdown.");
            goto fail;
        }
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

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
    struct pollfd *pollfd;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("invalid sample format specification or channel map");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    pa_memchunk_reset(&u->memchunk);
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop);
    u->rtpoll = pa_rtpoll_new();
    pa_rtpoll_item_new_asyncmsgq(u->rtpoll, PA_RTPOLL_EARLY, u->thread_mq.inq);

    u->filename = pa_xstrdup(pa_modargs_get_value(ma, "file", DEFAULT_FILE_NAME));

    mkfifo(u->filename, 0666);
    if ((u->fd = open(u->filename, O_RDWR|O_NOCTTY)) < 0) {
        pa_log("open('%s'): %s", u->filename, pa_cstrerror(errno));
        goto fail;
    }

    pa_make_fd_cloexec(u->fd);
    pa_make_fd_nonblock(u->fd);

    if (fstat(u->fd, &st) < 0) {
        pa_log("fstat('%s'): %s",u->filename, pa_cstrerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        pa_log("'%s' is not a FIFO.", u->filename);
        goto fail;
    }

    if (!(u->source = pa_source_new(m->core, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss, &map))) {
        pa_log("Failed to create source.");
        goto fail;
    }

    u->source->userdata = u;
    u->source->flags = 0;

    pa_source_set_module(u->source, m);
    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    pa_source_set_description(u->source, t = pa_sprintf_malloc("Unix FIFO source '%s'", u->filename));
    pa_xfree(t);

    u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    pollfd->fd = u->fd;
    pollfd->events = pollfd->revents = 0;

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_source_put(u->source);

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

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->source)
        pa_source_unref(u->source);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->rtpoll_item)
        pa_rtpoll_item_free(u->rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->filename) {
        unlink(u->filename);
        pa_xfree(u->filename);
    }

    if (u->fd >= 0)
        pa_assert_se(pa_close(u->fd) == 0);

    pa_xfree(u);
}
