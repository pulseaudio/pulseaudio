/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/rtclock.h>

#include <pulsecore/core-error.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("UNIX pipe sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "sink_properties=<properties for the sink> "
        "file=<path of the FIFO> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map> "
        "use_system_clock_for_timing=<yes or no> "
);

#define DEFAULT_FILE_NAME "fifo_output"
#define DEFAULT_SINK_NAME "fifo_output"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    char *filename;
    int fd;
    bool do_unlink_fifo;
    size_t buffer_size;
    size_t bytes_dropped;
    bool fifo_error;

    pa_memchunk memchunk;

    pa_rtpoll_item *rtpoll_item;

    int write_type;
    pa_usec_t block_usec;
    pa_usec_t timestamp;

    bool use_system_clock_for_timing;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "file",
    "format",
    "rate",
    "channels",
    "channel_map",
    "use_system_clock_for_timing",
    NULL
};

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
            if (u->use_system_clock_for_timing) {
                pa_usec_t now;
                now = pa_rtclock_now();
                *((int64_t*) data) = (int64_t)u->timestamp - (int64_t)now;
            } else {
                size_t n = 0;

#ifdef FIONREAD
                int l;

                if (ioctl(u->fd, FIONREAD, &l) >= 0 && l > 0)
                    n = (size_t) l;
#endif

                n += u->memchunk.length;

                *((int64_t*) data) = pa_bytes_to_usec(n, &u->sink->sample_spec);
            }
            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    if (u->sink->thread_info.state == PA_SINK_SUSPENDED || u->sink->thread_info.state == PA_SINK_INIT) {
        if (PA_SINK_IS_OPENED(new_state))
            u->timestamp = pa_rtclock_now();
    } else if (u->sink->thread_info.state == PA_SINK_RUNNING || u->sink->thread_info.state == PA_SINK_IDLE) {
        if (new_state == PA_SINK_SUSPENDED) {
            /* Clear potential FIFO error flag */
            u->fifo_error = false;

            /* Continuously dropping data (clear counter on entering suspended state. */
            if (u->bytes_dropped != 0) {
                pa_log_debug("Pipe-sink continuously dropping data - clear statistics (%zu -> 0 bytes dropped)", u->bytes_dropped);
                u->bytes_dropped = 0;
            }
        }
    }

    return 0;
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->block_usec = pa_sink_get_requested_latency_within_thread(s);

    if (u->block_usec == (pa_usec_t) -1)
        u->block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(u->block_usec, &s->sample_spec);
    pa_sink_set_max_request_within_thread(s, nbytes);
}

static ssize_t pipe_sink_write(struct userdata *u, pa_memchunk *pchunk) {
    size_t index, length;
    ssize_t count = 0;
    void *p;

    pa_assert(u);
    pa_assert(pchunk);

    index = pchunk->index;
    length = pchunk->length;
    p = pa_memblock_acquire(pchunk->memblock);

    for (;;) {
        ssize_t l;

        l = pa_write(u->fd, (uint8_t*) p + index, length, &u->write_type);

        pa_assert(l != 0);

        if (l < 0) {
            if (errno == EAGAIN)
                break;
            else if (errno != EINTR) {
                if (!u->fifo_error) {
                    pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
                    u->fifo_error = true;
                }
                count = -1 - count;
                break;
            }
        } else {
            if (u->fifo_error) {
                pa_log_debug("Recovered from FIFO error");
                u->fifo_error = false;
            }
            count += l;
            index += l;
            length -= l;

            if (length <= 0) {
                break;
            }
        }
    }

    pa_memblock_release(pchunk->memblock);

    return count;
}

static void process_render_use_timing(struct userdata *u, pa_usec_t now) {
    size_t dropped = 0;
    size_t consumed = 0;

    pa_assert(u);

    /* Fill the buffer up the latency size */
    while (u->timestamp < now + u->block_usec) {
        ssize_t written = 0;
        pa_memchunk chunk;

        pa_sink_render(u->sink, u->sink->thread_info.max_request, &chunk);

        pa_assert(chunk.length > 0);

        if ((written = pipe_sink_write(u, &chunk)) < 0)
            written = -1 - written;

        pa_memblock_unref(chunk.memblock);

        u->timestamp += pa_bytes_to_usec(chunk.length, &u->sink->sample_spec);

        dropped = chunk.length - written;

        if (u->bytes_dropped != 0 && dropped != chunk.length) {
            pa_log_debug("Pipe-sink continuously dropped %zu bytes", u->bytes_dropped);
            u->bytes_dropped = 0;
        }

        if (u->bytes_dropped == 0 && dropped != 0)
            pa_log_debug("Pipe-sink just dropped %zu bytes", dropped);

        u->bytes_dropped += dropped;

        consumed += chunk.length;

        if (consumed >= u->sink->thread_info.max_request)
            break;
    }
}

static int process_render(struct userdata *u) {
    pa_assert(u);

    if (u->memchunk.length <= 0)
        pa_sink_render(u->sink, u->buffer_size, &u->memchunk);

    pa_assert(u->memchunk.length > 0);

    for (;;) {
        ssize_t l;
        void *p;

        p = pa_memblock_acquire(u->memchunk.memblock);
        l = pa_write(u->fd, (uint8_t*) p + u->memchunk.index, u->memchunk.length, &u->write_type);
        pa_memblock_release(u->memchunk.memblock);

        pa_assert(l != 0);

        if (l < 0) {

            if (errno == EINTR)
                continue;
            else if (errno == EAGAIN)
                return 0;
            else {
                pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
                return -1;
            }

        } else {

            u->memchunk.index += (size_t) l;
            u->memchunk.length -= (size_t) l;

            if (u->memchunk.length <= 0) {
                pa_memblock_unref(u->memchunk.memblock);
                pa_memchunk_reset(&u->memchunk);
            }
        }

        return 0;
    }
}

static void thread_func_use_timing(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread (use timing) starting up");

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    for (;;) {
        pa_usec_t now = 0;
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            now = pa_rtclock_now();

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            pa_sink_process_rewind(u->sink, 0);

        /* Render some data and write it to the fifo */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->timestamp <= now)
                process_render_use_timing(u, now);

            pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread (use timing) shutting down");
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        struct pollfd *pollfd;
        int ret;

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            pa_sink_process_rewind(u->sink, 0);

        /* Render some data and write it to the fifo */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (pollfd->revents) {
                if (process_render(u) < 0)
                    goto fail;

                pollfd->revents = 0;
            }
        }

        /* Hmm, nothing to do. Let's sleep */
        pollfd->events = (short) (u->sink->thread_info.state == PA_SINK_RUNNING ? POLLOUT : 0);

        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

        if (pollfd->revents & ~POLLOUT) {
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

int pa__init(pa_module *m) {
    struct userdata *u;
    struct stat st;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    struct pollfd *pollfd;
    pa_sink_new_data data;
    pa_thread_func_t thread_routine;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    pa_memchunk_reset(&u->memchunk);
    u->rtpoll = pa_rtpoll_new();

    if (pa_modargs_get_value_boolean(ma, "use_system_clock_for_timing", &u->use_system_clock_for_timing) < 0) {
        pa_log("Failed to parse use_system_clock_for_timing argument.");
        goto fail;
    }

    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        pa_log("pa_thread_mq_init() failed.");
        goto fail;
    }

    u->write_type = 0;

    u->filename = pa_runtime_path(pa_modargs_get_value(ma, "file", DEFAULT_FILE_NAME));
    u->do_unlink_fifo = false;

    if (mkfifo(u->filename, 0666) < 0) {
        if (errno != EEXIST) {
            pa_log("mkfifo('%s'): %s", u->filename, pa_cstrerror(errno));
            goto fail;
        }
    } else
        u->do_unlink_fifo = true;

    if ((u->fd = pa_open_cloexec(u->filename, O_RDWR, 0)) < 0) {
        pa_log("open('%s'): %s", u->filename, pa_cstrerror(errno));
        goto fail;
    }

    pa_make_fd_nonblock(u->fd);

    if (fstat(u->fd, &st) < 0) {
        pa_log("fstat('%s'): %s", u->filename, pa_cstrerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        pa_log("'%s' is not a FIFO.", u->filename);
        goto fail;
    }

    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->filename);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Unix FIFO sink %s", u->filename);
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    if (u->use_system_clock_for_timing)
        u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY);
    else
        u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    if (u->use_system_clock_for_timing)
        u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->bytes_dropped = 0;
    u->fifo_error = false;
    u->buffer_size = pa_frame_align(pa_pipe_buf(u->fd), &u->sink->sample_spec);
    if (u->use_system_clock_for_timing) {
        u->block_usec = pa_bytes_to_usec(u->buffer_size, &u->sink->sample_spec);
        pa_sink_set_latency_range(u->sink, 0, u->block_usec);
        thread_routine = thread_func_use_timing;
    } else {
        pa_sink_set_fixed_latency(u->sink, pa_bytes_to_usec(u->buffer_size, &u->sink->sample_spec));
        thread_routine = thread_func;
    }
    pa_sink_set_max_request(u->sink, u->buffer_size);

    u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    pollfd->fd = u->fd;
    pollfd->events = pollfd->revents = 0;

    if (!(u->thread = pa_thread_new("pipe-sink", thread_routine, u))) {
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

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module *m) {
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

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->rtpoll_item)
        pa_rtpoll_item_free(u->rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->filename) {
        if (u->do_unlink_fifo)
            unlink(u->filename);
        pa_xfree(u->filename);
    }

    if (u->fd >= 0)
        pa_assert_se(pa_close(u->fd) == 0);

    pa_xfree(u);
}
