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

#include "module-pipe-source-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("UNIX pipe source")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "source_name=<name for the source> "
        "file=<path of the FIFO> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map>")

#define DEFAULT_FILE_NAME "/tmp/music.input"
#define DEFAULT_SOURCE_NAME "fifo_input"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;
    pa_thread *thread;
    pa_asyncmsgq *asyncmsgq;
    
    char *filename;
    int fd;

    pa_memchunk memchunk;
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
    enum {
        POLLFD_ASYNCQ,
        POLLFD_FIFO,
        POLLFD_MAX,
    };
    
    struct userdata *u = userdata;
    struct pollfd pollfd[POLLFD_MAX];
    int read_type = 0;

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

        /* Try to read some data and pass it on to the source driver */

        if (u->source->thread_info.state == PA_SOURCE_RUNNING && pollfd[POLLFD_FIFO].revents) {
            void *p;
            ssize_t l;

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

                pollfd[POLLFD_FIFO].revents = 0;
                continue;
            }
        }

        pollfd[POLLFD_FIFO].events = u->source->thread_info.state == PA_SOURCE_RUNNING ? POLLIN : 0;

        /* Hmm, nothing to do. Let's sleep */

        if (pa_asyncmsgq_before_poll(u->asyncmsgq) < 0)
            continue;

/*         pa_log("polling for %i", pollfd[POLLFD_FIFO].events); */
        r = poll(pollfd, POLLFD_MAX, -1);
/*         pa_log("polling got %i (r=%i) %i", r > 0 ? pollfd[POLLFD_FIFO].revents : 0, r, r > 0 ? pollfd[POLLFD_ASYNCQ].revents: 0); */

        pa_asyncmsgq_after_poll(u->asyncmsgq);

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

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u;
    struct stat st;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    char *t;

    pa_assert(c);
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments.");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("invalid sample format specification or channel map");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = c;
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
        pa_log("fstat('%s'): %s",u->filename, pa_cstrerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        pa_log("'%s' is not a FIFO.", u->filename);
        goto fail;
    }

    if (!(u->source = pa_source_new(c, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss, &map))) {
        pa_log("Failed to create source.");
        goto fail;
    }

    u->source->userdata = u;
    
    pa_source_set_module(u->source, m);
    pa_source_set_asyncmsgq(u->source, u->asyncmsgq);
    pa_source_set_description(u->source, t = pa_sprintf_malloc("Unix FIFO source '%s'", u->filename));
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

    if (u->source)
        pa_source_disconnect(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->asyncmsgq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    if (u->asyncmsgq)
        pa_asyncmsgq_free(u->asyncmsgq);

    if (u->source)
        pa_source_unref(u->source);
    
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
