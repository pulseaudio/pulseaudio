/* $Id: module-esound-sink.c 2043 2007-11-09 18:25:40Z lennart $ */

/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

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
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>

#ifdef HAVE_LINUX_SOCKIOS_H
#include <linux/sockios.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/core-error.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/socket-client.h>
#include <pulsecore/authkey.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/thread.h>
#include <pulsecore/time-smoother.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/socket-util.h>

#include "module-raop-sink-symdef.h"
#include "rtp.h"
#include "sdp.h"
#include "sap.h"
#include "raop_client.h"

PA_MODULE_AUTHOR("Colin Guthrie");
PA_MODULE_DESCRIPTION("RAOP Sink (Apple Airtunes)");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "server=<address> cookie=<filename>  "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate>");

#define DEFAULT_SINK_NAME "airtunes"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    pa_thread *thread;

    pa_memchunk raw_memchunk;
    pa_memchunk encoded_memchunk;

    void *write_data;
    size_t write_length, write_index;

    void *read_data;
    size_t read_length, read_index;

    pa_usec_t latency;

    /*esd_format_t format;*/
    int32_t rate;

    pa_smoother *smoother;
    int fd;

    int64_t offset;
    int64_t encoding_overhead;
    int32_t next_encoding_overhead;
    double encoding_ratio;

    pa_raop_client *raop;

    size_t block_size;
};

static const char* const valid_modargs[] = {
    "server",
    "rate",
    "format",
    "channels",
    "sink_name",
    NULL
};

enum {
    SINK_MESSAGE_PASS_SOCKET = PA_SINK_MESSAGE_MAX
};

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_SET_STATE:

            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {

                case PA_SINK_SUSPENDED:
                    pa_assert(PA_SINK_OPENED(u->sink->thread_info.state));

                    pa_smoother_pause(u->smoother, pa_rtclock_usec());
                    break;

                case PA_SINK_IDLE:
                case PA_SINK_RUNNING:

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED)
                        pa_smoother_resume(u->smoother, pa_rtclock_usec());

                    break;

                case PA_SINK_UNLINKED:
                case PA_SINK_INIT:
                    ;
            }

            break;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t w, r;

            r = pa_smoother_get(u->smoother, pa_rtclock_usec());
            w = pa_bytes_to_usec((u->offset - u->encoding_overhead + (u->encoded_memchunk.length / u->encoding_ratio)), &u->sink->sample_spec);

            *((pa_usec_t*) data) = w > r ? w - r : 0;
            break;
        }

        case SINK_MESSAGE_PASS_SOCKET: {
            struct pollfd *pollfd;

            pa_assert(!u->rtpoll_item);

            u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
            pollfd->fd = u->fd;
            pollfd->events = pollfd->revents = 0;

            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    int write_type = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    pa_smoother_set_time_offset(u->smoother, pa_rtclock_usec());

    for (;;) {
        int ret;

        if (u->rtpoll_item) {
            struct pollfd *pollfd;
            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

            /* Render some data and write it to the fifo */
            if (PA_SINK_OPENED(u->sink->thread_info.state) && pollfd->revents) {
                pa_usec_t usec;
                int64_t n;

                for (;;) {
                    ssize_t l;
                    void *p;

                    if (u->raw_memchunk.length <= 0) {
                        /* Grab unencoded data */
                        pa_sink_render(u->sink, u->block_size, &u->raw_memchunk);
                    }
                    pa_assert(u->raw_memchunk.length > 0);

                    if (u->encoded_memchunk.length <= 0) {
                        /* Encode it */
                        size_t rl = u->raw_memchunk.length;
                        u->encoding_overhead += u->next_encoding_overhead;
                        pa_raop_client_encode_sample(u->raop, &u->raw_memchunk, &u->encoded_memchunk);
                        u->next_encoding_overhead = (u->encoded_memchunk.length - (rl - u->raw_memchunk.length));
                        u->encoding_ratio = u->encoded_memchunk.length / (rl - u->raw_memchunk.length);
                    }
                    pa_assert(u->encoded_memchunk.length > 0);

                    p = pa_memblock_acquire(u->encoded_memchunk.memblock);
                    l = pa_write(u->fd, (uint8_t*) p + u->encoded_memchunk.index, u->encoded_memchunk.length, &write_type);
                    pa_memblock_release(u->encoded_memchunk.memblock);

                    pa_assert(l != 0);

                    if (l < 0) {

                        if (errno == EINTR)
                            continue;
                        else if (errno == EAGAIN) {

                            /* OK, we filled all socket buffers up
                             * now. */
                            goto filled_up;

                        } else {
                            pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
                            goto fail;
                        }

                    } else {
                        u->offset += l;

                        u->encoded_memchunk.index += l;
                        u->encoded_memchunk.length -= l;

                        pollfd->revents = 0;

                        if (u->encoded_memchunk.length > 0)

                            /* OK, we wrote less that we asked for,
                             * hence we can assume that the socket
                             * buffers are full now */
                            goto filled_up;
                    }
                }

            filled_up:

                /* At this spot we know that the socket buffers are
                 * fully filled up. This is the best time to estimate
                 * the playback position of the server */

                n = u->offset;

#ifdef SIOCOUTQ
                {
                    int l;
                    if (ioctl(u->fd, SIOCOUTQ, &l) >= 0 && l > 0)
                        n -= l;
                }
#endif

                usec = pa_bytes_to_usec(n, &u->sink->sample_spec);

                if (usec > u->latency)
                    usec -= u->latency;
                else
                    usec = 0;

                pa_smoother_put(u->smoother, pa_rtclock_usec(), usec);
            }

            /* Hmm, nothing to do. Let's sleep */
            pollfd->events = PA_SINK_OPENED(u->sink->thread_info.state)  ? POLLOUT : 0;
        }

        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        if (u->rtpoll_item) {
            struct pollfd* pollfd;

            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

            if (pollfd->revents & ~POLLOUT) {
                pa_log("FIFO shutdown.");
                goto fail;
            }
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

static void on_connection(PA_GCC_UNUSED int fd, void*userdata) {
    struct userdata *u = userdata;
    pa_assert(u);

    pa_assert(u->fd < 0);
    u->fd = fd;

    pa_log_debug("Connection authenticated, handing fd to IO thread...");

    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_PASS_SOCKET, NULL, 0, NULL, NULL);
}

static void on_close(void*userdata) {
    struct userdata *u = userdata;
    pa_assert(u);

    pa_log_debug("Control connection closed.");
    pa_module_unload_request(u->module);
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    const char *p;
    pa_sample_spec ss;
    pa_modargs *ma = NULL;
    char *t;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log("invalid sample format specification");
        goto fail;
    }

    if ((/*ss.format != PA_SAMPLE_U8 &&*/ ss.format != PA_SAMPLE_S16NE) ||
        (ss.channels > 2)) {
        pa_log("sample type support is limited to mono/stereo and U8 or S16NE sample data");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->fd = -1;
    u->smoother = pa_smoother_new(PA_USEC_PER_SEC, PA_USEC_PER_SEC*2, TRUE);
    pa_memchunk_reset(&u->raw_memchunk);
    pa_memchunk_reset(&u->encoded_memchunk);
    u->offset = 0;
    u->encoding_overhead = 0;
    u->next_encoding_overhead = 0;
    u->encoding_ratio = 1.0;

    pa_thread_mq_init(&u->thread_mq, m->core->mainloop);
    u->rtpoll = pa_rtpoll_new();
    pa_rtpoll_item_new_asyncmsgq(u->rtpoll, PA_RTPOLL_EARLY, u->thread_mq.inq);
    u->rtpoll_item = NULL;

    /*u->format =
        (ss.format == PA_SAMPLE_U8 ? ESD_BITS8 : ESD_BITS16) |
        (ss.channels == 2 ? ESD_STEREO : ESD_MONO);*/
    u->rate = ss.rate;
    u->block_size = pa_usec_to_bytes(PA_USEC_PER_SEC/20, &ss);

    u->read_data = u->write_data = NULL;
    u->read_index = u->write_index = u->read_length = u->write_length = 0;

    /*u->state = STATE_AUTH;*/
    u->latency = 0;

    if (!(u->sink = pa_sink_new(m->core, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, NULL))) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;
    u->sink->flags = PA_SINK_LATENCY|PA_SINK_NETWORK;

    pa_sink_set_module(u->sink, m);
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    if (!(p = pa_modargs_get_value(ma, "server", NULL))) {
        pa_log("No server argument given.");
        goto fail;
    }

    if (!(u->raop = pa_raop_client_new(u->core, p))) {
        pa_log("Failed to connect to server.");
        goto fail;
    }

    pa_raop_client_set_callback(u->raop, on_connection, u);
    pa_raop_client_set_closed_callback(u->raop, on_close, u);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Airtunes sink '%s'", p));
    pa_xfree(t);


    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);

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
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll_item)
        pa_rtpoll_item_free(u->rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->raw_memchunk.memblock)
        pa_memblock_unref(u->raw_memchunk.memblock);

    if (u->encoded_memchunk.memblock)
        pa_memblock_unref(u->encoded_memchunk.memblock);

    if (u->raop)
        pa_raop_client_free(u->raop);

    pa_xfree(u->read_data);
    pa_xfree(u->write_data);

    if (u->smoother)
        pa_smoother_free(u->smoother);

    if (u->fd >= 0)
        pa_close(u->fd);

    pa_xfree(u);
}
