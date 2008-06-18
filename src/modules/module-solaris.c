/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2006-2007 Pierre Ossman <ossman@cendio.se> for Cendio AB

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
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <signal.h>
#include <stropts.h>
#include <sys/conf.h>
#include <sys/audio.h>

#include <pulse/error.h>
#include <pulse/mainloop-signal.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-error.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/thread.h>

#include "module-solaris-symdef.h"

PA_MODULE_AUTHOR("Pierre Ossman")
PA_MODULE_DESCRIPTION("Solaris Sink/Source")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
    "sink_name=<name for the sink> "
    "source_name=<name for the source> "
    "device=<OSS device> record=<enable source?> "
    "playback=<enable sink?> "
    "format=<sample format> "
    "channels=<number of channels> "
    "rate=<sample rate> "
    "buffer_size=<record buffer size> "
    "channel_map=<channel map>")

struct userdata {
    pa_core *core;
    pa_sink *sink;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_signal_event *sig;

    pa_memchunk memchunk;

    unsigned int page_size;

    uint32_t frame_size;
    uint32_t buffer_size;
    unsigned int written_bytes, read_bytes;

    int fd;
    pa_rtpoll_item *rtpoll_item;
    pa_module *module;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "source_name",
    "device",
    "record",
    "playback",
    "buffer_size",
    "format",
    "rate",
    "channels",
    "channel_map",
    NULL
};

#define DEFAULT_SINK_NAME "solaris_output"
#define DEFAULT_SOURCE_NAME "solaris_input"
#define DEFAULT_DEVICE "/dev/audio"

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;
    int err;
    audio_info_t info;

    switch (code) {
    case PA_SINK_MESSAGE_GET_LATENCY: {
        pa_usec_t r = 0;

        if (u->fd >= 0) {

            err = ioctl(u->fd, AUDIO_GETINFO, &info);
            pa_assert(err >= 0);

            r += pa_bytes_to_usec(u->written_bytes, &PA_SINK(o)->sample_spec);
            r -= pa_bytes_to_usec(info.play.samples * u->frame_size, &PA_SINK(o)->sample_spec);

            if (u->memchunk.memblock)
                r += pa_bytes_to_usec(u->memchunk.length, &PA_SINK(o)->sample_spec);
        }

        *((pa_usec_t*) data) = r;

        return 0;
    }

    case PA_SINK_MESSAGE_SET_VOLUME:
        if (u->fd >= 0) {
            AUDIO_INITINFO(&info);

            info.play.gain = pa_cvolume_avg((pa_cvolume*)data) * AUDIO_MAX_GAIN / PA_VOLUME_NORM;
            assert(info.play.gain <= AUDIO_MAX_GAIN);

            if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0) {
                if (errno == EINVAL)
                    pa_log("AUDIO_SETINFO: Unsupported volume.");
                else
                    pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
            } else {
                return 0;
            }
        }
        break;

    case PA_SINK_MESSAGE_GET_VOLUME:
        if (u->fd >= 0) {
            err = ioctl(u->fd, AUDIO_GETINFO, &info);
            assert(err >= 0);

            pa_cvolume_set((pa_cvolume*) data, ((pa_cvolume*) data)->channels,
                info.play.gain * PA_VOLUME_NORM / AUDIO_MAX_GAIN);

            return 0;
        }
        break;

    case PA_SINK_MESSAGE_SET_MUTE:
        if (u->fd >= 0) {
            AUDIO_INITINFO(&info);

            info.output_muted = !!PA_PTR_TO_UINT(data);

            if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0)
                pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
            else
                return 0;
        }
        break;

    case PA_SINK_MESSAGE_GET_MUTE:
        if (u->fd >= 0) {
            err = ioctl(u->fd, AUDIO_GETINFO, &info);
            pa_assert(err >= 0);

            *(int*)data = !!info.output_muted;

            return 0;
        }
        break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;
    int err;
    audio_info_t info;

    switch (code) {
        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (u->fd) {
                err = ioctl(u->fd, AUDIO_GETINFO, &info);
                pa_assert(err >= 0);

                r += pa_bytes_to_usec(info.record.samples * u->frame_size, &PA_SOURCE(o)->sample_spec);
                r -= pa_bytes_to_usec(u->read_bytes, &PA_SOURCE(o)->sample_spec);
            }

            *((pa_usec_t*) data) = r;

            return 0;
        }

        case PA_SOURCE_MESSAGE_SET_VOLUME:
            if (u->fd >= 0) {
                AUDIO_INITINFO(&info);

                info.record.gain = pa_cvolume_avg((pa_cvolume*) data) * AUDIO_MAX_GAIN / PA_VOLUME_NORM;
                assert(info.record.gain <= AUDIO_MAX_GAIN);

                if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0) {
                    if (errno == EINVAL)
                        pa_log("AUDIO_SETINFO: Unsupported volume.");
                    else
                        pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
                } else {
                    return 0;
                }
            }
            break;

        case PA_SOURCE_MESSAGE_GET_VOLUME:
            if (u->fd >= 0) {
                err = ioctl(u->fd, AUDIO_GETINFO, &info);
                pa_assert(err >= 0);

                pa_cvolume_set((pa_cvolume*) data, ((pa_cvolume*) data)->channels,
                    info.record.gain * PA_VOLUME_NORM / AUDIO_MAX_GAIN);

                return 0;
            }
            break;
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static void clear_underflow(struct userdata *u)
{
    audio_info_t info;

    AUDIO_INITINFO(&info);

    info.play.error = 0;

    if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0)
        pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
}

static void clear_overflow(struct userdata *u)
{
    audio_info_t info;

    AUDIO_INITINFO(&info);

    info.record.error = 0;

    if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0)
        pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    unsigned short revents = 0;
    int ret;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    if (u->core->high_priority)
        pa_make_realtime();

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    for (;;) {
        /* Render some data and write it to the dsp */

        if (u->sink && PA_SINK_OPENED(u->sink->thread_info.state)) {
            audio_info_t info;
            int err;
            size_t len;

            err = ioctl(u->fd, AUDIO_GETINFO, &info);
            pa_assert(err >= 0);

            /*
             * Since we cannot modify the size of the output buffer we fake it
             * by not filling it more than u->buffer_size.
             */
            len = u->buffer_size;
            len -= u->written_bytes - (info.play.samples * u->frame_size);

            /* The sample counter can sometimes go backwards :( */
            if (len > u->buffer_size)
                len = 0;

            if (info.play.error) {
                pa_log_debug("Solaris buffer underflow!");
                clear_underflow(u);
            }

            len -= len % u->frame_size;

            while (len) {
                void *p;
                ssize_t r;

                if (!u->memchunk.length)
                    pa_sink_render(u->sink, len, &u->memchunk);

                pa_assert(u->memchunk.length);

                p = pa_memblock_acquire(u->memchunk.memblock);
                r = pa_write(u->fd, (uint8_t*) p + u->memchunk.index, u->memchunk.length, NULL);
                pa_memblock_release(u->memchunk.memblock);

                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    else if (errno != EAGAIN) {
                        pa_log("Failed to read data from DSP: %s", pa_cstrerror(errno));
                        goto fail;
                    }
                } else {
                    pa_assert(r % u->frame_size == 0);

                    u->memchunk.index += r;
                    u->memchunk.length -= r;

                    if (u->memchunk.length <= 0) {
                        pa_memblock_unref(u->memchunk.memblock);
                        pa_memchunk_reset(&u->memchunk);
                    }

                    len -= r;
                    u->written_bytes += r;
                }
            }
        }

        /* Try to read some data and pass it on to the source driver */

        if (u->source && PA_SOURCE_OPENED(u->source->thread_info.state) && ((revents & POLLIN))) {
            pa_memchunk memchunk;
            int err;
            size_t l;
            void *p;
            ssize_t r;
            audio_info_t info;

            err = ioctl(u->fd, AUDIO_GETINFO, &info);
            pa_assert(err >= 0);

            if (info.record.error) {
                pa_log_debug("Solaris buffer overflow!");
                clear_overflow(u);
            }

            err = ioctl(u->fd, I_NREAD, &l);
            pa_assert(err >= 0);

            if (l > 0) {
                /* This is to make sure it fits in the memory pool. Also, a page
                   should be the most efficient transfer size. */
                if (l > u->page_size)
                    l = u->page_size;

                memchunk.memblock = pa_memblock_new(u->core->mempool, l);
                pa_assert(memchunk.memblock);

                p = pa_memblock_acquire(memchunk.memblock);
                r = pa_read(u->fd, p, l, NULL);
                pa_memblock_release(memchunk.memblock);

                if (r < 0) {
                    pa_memblock_unref(memchunk.memblock);
                    if (errno != EAGAIN) {
                        pa_log("Failed to read data from DSP: %s", pa_cstrerror(errno));
                        goto fail;
                    }
                } else {
                    memchunk.index = 0;
                    memchunk.length = r;

                    pa_source_post(u->source, &memchunk);
                    pa_memblock_unref(memchunk.memblock);

                    u->read_bytes += r;

                    revents &= ~POLLIN;
                }
            }
        }

        if (u->fd >= 0) {
            struct pollfd *pollfd;

            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
            pollfd->events =
                ((u->source && PA_SOURCE_OPENED(u->source->thread_info.state)) ? POLLIN : 0);
        }

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll, 1)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        if (u->fd >= 0) {
            struct pollfd *pollfd;

            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

            if (pollfd->revents & ~(POLLOUT|POLLIN)) {
                pa_log("DSP shutdown.");
                goto fail;
            }

            revents = pollfd->revents;
        } else
            revents = 0;
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

static void sig_callback(pa_mainloop_api *api, pa_signal_event*e, int sig, void *userdata) {
    struct userdata *u = userdata;

    assert(u);

    if (u->sink) {
        pa_sink_get_volume(u->sink);
        pa_sink_get_mute(u->sink);
    }

    if (u->source)
        pa_source_get_volume(u->source);
}

static int pa_solaris_auto_format(int fd, int mode, pa_sample_spec *ss) {
    audio_info_t info;

    AUDIO_INITINFO(&info);

    if (mode != O_RDONLY) {
        info.play.sample_rate = ss->rate;
        info.play.channels = ss->channels;
        switch (ss->format) {
        case PA_SAMPLE_U8:
            info.play.precision = 8;
            info.play.encoding = AUDIO_ENCODING_LINEAR;
            break;
        case PA_SAMPLE_ALAW:
            info.play.precision = 8;
            info.play.encoding = AUDIO_ENCODING_ALAW;
            break;
        case PA_SAMPLE_ULAW:
            info.play.precision = 8;
            info.play.encoding = AUDIO_ENCODING_ULAW;
            break;
        case PA_SAMPLE_S16NE:
            info.play.precision = 16;
            info.play.encoding = AUDIO_ENCODING_LINEAR;
            break;
        default:
            return -1;
        }
    }

    if (mode != O_WRONLY) {
        info.record.sample_rate = ss->rate;
        info.record.channels = ss->channels;
        switch (ss->format) {
        case PA_SAMPLE_U8:
            info.record.precision = 8;
            info.record.encoding = AUDIO_ENCODING_LINEAR;
            break;
        case PA_SAMPLE_ALAW:
            info.record.precision = 8;
            info.record.encoding = AUDIO_ENCODING_ALAW;
            break;
        case PA_SAMPLE_ULAW:
            info.record.precision = 8;
            info.record.encoding = AUDIO_ENCODING_ULAW;
            break;
        case PA_SAMPLE_S16NE:
            info.record.precision = 16;
            info.record.encoding = AUDIO_ENCODING_LINEAR;
            break;
        default:
            return -1;
        }
    }

    if (ioctl(fd, AUDIO_SETINFO, &info) < 0) {
        if (errno == EINVAL)
            pa_log("AUDIO_SETINFO: Unsupported sample format.");
        else
            pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

static int pa_solaris_set_buffer(int fd, int buffer_size) {
    audio_info_t info;

    AUDIO_INITINFO(&info);

    info.play.buffer_size = buffer_size;
    info.record.buffer_size = buffer_size;

    if (ioctl(fd, AUDIO_SETINFO, &info) < 0) {
        if (errno == EINVAL)
            pa_log("AUDIO_SETINFO: Unsupported buffer size.");
        else
            pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    const char *p;
    int fd = -1;
    int buffer_size;
    int mode;
    int record = 1, playback = 1;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    char *t;
    struct pollfd *pollfd;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "record", &record) < 0 || pa_modargs_get_value_boolean(ma, "playback", &playback) < 0) {
        pa_log("record= and playback= expect numeric argument.");
        goto fail;
    }

    if (!playback && !record) {
        pa_log("neither playback nor record enabled for device.");
        goto fail;
    }

    mode = (playback&&record) ? O_RDWR : (playback ? O_WRONLY : (record ? O_RDONLY : 0));

    buffer_size = 16384;
    if (pa_modargs_get_value_s32(ma, "buffer_size", &buffer_size) < 0) {
        pa_log("failed to parse buffer size argument");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("failed to parse sample specification");
        goto fail;
    }

    if ((fd = open(p = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), mode | O_NONBLOCK)) < 0)
        goto fail;

    pa_log_info("device opened in %s mode.", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));

    if (pa_solaris_auto_format(fd, mode, &ss) < 0)
        goto fail;

    if (pa_solaris_set_buffer(fd, buffer_size) < 0)
        goto fail;

    u = pa_xmalloc(sizeof(struct userdata));
    u->core = m->core;

    u->fd = fd;

    pa_memchunk_reset(&u->memchunk);

    /* We use this to get a reasonable chunk size */
    u->page_size = PA_PAGE_SIZE;

    u->frame_size = pa_frame_size(&ss);
    u->buffer_size = buffer_size;

    u->written_bytes = 0;
    u->read_bytes = 0;

    u->module = m;
    m->userdata = u;

    pa_thread_mq_init(&u->thread_mq, m->core->mainloop);

    u->rtpoll = pa_rtpoll_new();
    pa_rtpoll_item_new_asyncmsgq(u->rtpoll, PA_RTPOLL_EARLY, u->thread_mq.inq);

    pa_rtpoll_set_timer_periodic(u->rtpoll, pa_bytes_to_usec(u->buffer_size / 10, &ss));

    u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    pollfd->fd = fd;
    pollfd->events = 0;
    pollfd->revents = 0;

    if (mode != O_WRONLY) {
        u->source = pa_source_new(m->core, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss, &map);
        pa_assert(u->source);

        u->source->userdata = u;
        u->source->parent.process_msg = source_process_msg;

        pa_source_set_module(u->source, m);
        pa_source_set_description(u->source, t = pa_sprintf_malloc("Solaris PCM on '%s'", p));
        pa_xfree(t);
        pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
        pa_source_set_rtpoll(u->source, u->rtpoll);

        u->source->flags = PA_SOURCE_HARDWARE|PA_SOURCE_LATENCY|PA_SOURCE_HW_VOLUME_CTRL;
        u->source->refresh_volume = 1;
    } else
        u->source = NULL;

    if (mode != O_RDONLY) {
        u->sink = pa_sink_new(m->core, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map);
        pa_assert(u->sink);

        u->sink->userdata = u;
        u->sink->parent.process_msg = sink_process_msg;

        pa_sink_set_module(u->sink, m);
        pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Solaris PCM on '%s'", p));
        pa_xfree(t);
        pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
        pa_sink_set_rtpoll(u->sink, u->rtpoll);

        u->sink->flags = PA_SINK_HARDWARE|PA_SINK_LATENCY|PA_SINK_HW_VOLUME_CTRL;
        u->sink->refresh_volume = 1;
        u->sink->refresh_mute = 1;
    } else
        u->sink = NULL;

    pa_assert(u->source || u->sink);

    u->sig = pa_signal_new(SIGPOLL, sig_callback, u);
    pa_assert(u->sig);
    ioctl(u->fd, I_SETSIG, S_MSG);

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    /* Read mixer settings */
    if (u->source)
        pa_asyncmsgq_send(u->thread_mq.inq, PA_MSGOBJECT(u->source), PA_SOURCE_MESSAGE_GET_VOLUME, &u->source->volume, 0, NULL);
    if (u->sink) {
        pa_asyncmsgq_send(u->thread_mq.inq, PA_MSGOBJECT(u->sink), PA_SINK_MESSAGE_GET_VOLUME, &u->sink->volume, 0, NULL);
        pa_asyncmsgq_send(u->thread_mq.inq, PA_MSGOBJECT(u->sink), PA_SINK_MESSAGE_GET_MUTE, &u->sink->muted, 0, NULL);
    }

    if (u->sink)
        pa_sink_put(u->sink);
    if (u->source)
        pa_source_put(u->source);

    pa_modargs_free(ma);

    return 0;

fail:
    if (u)
        pa__done(m);
    else if (fd >= 0)
        close(fd);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    ioctl(u->fd, I_SETSIG, 0);
    pa_signal_free(u->sig);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->source)
        pa_source_unref(u->source);

     if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->rtpoll_item)
        pa_rtpoll_item_free(u->rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->fd >= 0)
        close(u->fd);

    pa_xfree(u);
}
