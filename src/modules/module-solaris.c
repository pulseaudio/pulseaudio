/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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
#include <assert.h>
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
    pa_sink *sink;
    pa_source *source;
    pa_iochannel *io;
    pa_core *core;
    pa_time_event *timer;
    pa_usec_t poll_timeout;
    pa_signal_event *sig;

    pa_memchunk memchunk;

    unsigned int page_size;

    uint32_t frame_size;
    uint32_t buffer_size;
    unsigned int written_bytes, read_bytes;
    int sink_underflow;

    int fd;
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

#define CHUNK_SIZE 2048

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->sink ? pa_sink_used_by(u->sink) : 0) +
                      (u->source ? pa_source_used_by(u->source) : 0));
}

static void do_write(struct userdata *u) {
    audio_info_t info;
    int err;
    size_t len;
    ssize_t r;

    assert(u);

    /* We cannot check pa_iochannel_is_writable() because of our buffer hack */
    if (!u->sink)
        return;

    update_usage(u);

    err = ioctl(u->fd, AUDIO_GETINFO, &info);
    assert(err >= 0);

    /*
     * Since we cannot modify the size of the output buffer we fake it
     * by not filling it more than u->buffer_size.
     */
    len = u->buffer_size;
    len -= u->written_bytes - (info.play.samples * u->frame_size);

    /* The sample counter can sometimes go backwards :( */
    if (len > u->buffer_size)
        len = 0;

    if (!u->sink_underflow && (len == u->buffer_size))
        pa_log_debug("Solaris buffer underflow!");

    len -= len % u->frame_size;

    if (len == 0)
        return;

    if (!u->memchunk.length) {
        if (pa_sink_render(u->sink, len, &u->memchunk) < 0) {
            u->sink_underflow = 1;
            return;
        }
    }

    u->sink_underflow = 0;

    assert(u->memchunk.memblock);
    assert(u->memchunk.memblock->data);
    assert(u->memchunk.length);

    if (u->memchunk.length < len) {
        len = u->memchunk.length;
        len -= len % u->frame_size;
        assert(len);
    }

    if ((r = pa_iochannel_write(u->io,
        (uint8_t*) u->memchunk.memblock->data + u->memchunk.index, len)) < 0) {
        pa_log("write() failed: %s", pa_cstrerror(errno));
        return;
    }

    assert(r % u->frame_size == 0);

    u->memchunk.index += r;
    u->memchunk.length -= r;

    if (u->memchunk.length <= 0) {
        pa_memblock_unref(u->memchunk.memblock);
        u->memchunk.memblock = NULL;
    }

    u->written_bytes += r;
}

static void do_read(struct userdata *u) {
    pa_memchunk memchunk;
    int err;
    size_t l;
    ssize_t r;
    assert(u);

    if (!u->source || !pa_iochannel_is_readable(u->io))
        return;

    update_usage(u);

    err = ioctl(u->fd, I_NREAD, &l);
    assert(err >= 0);

    /* This is to make sure it fits in the memory pool. Also, a page
       should be the most efficient transfer size. */
    if (l > u->page_size)
        l = u->page_size;

    memchunk.memblock = pa_memblock_new(u->core->mempool, l);
    assert(memchunk.memblock);
    if ((r = pa_iochannel_read(u->io, memchunk.memblock->data, memchunk.memblock->length)) < 0) {
        pa_memblock_unref(memchunk.memblock);
        if (errno != EAGAIN)
            pa_log("read() failed: %s", pa_cstrerror(errno));
        return;
    }

    assert(r <= (ssize_t) memchunk.memblock->length);
    memchunk.length = memchunk.memblock->length = r;
    memchunk.index = 0;

    pa_source_post(u->source, &memchunk);
    pa_memblock_unref(memchunk.memblock);

    u->read_bytes += r;
}

static void io_callback(pa_iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
    do_read(u);
}

static void timer_cb(pa_mainloop_api*a, pa_time_event *e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval ntv;

    assert(u);

    do_write(u);

    pa_gettimeofday(&ntv);
    pa_timeval_add(&ntv, u->poll_timeout);

    a->time_restart(e, &ntv);
}

static void sig_callback(pa_mainloop_api *api, pa_signal_event*e, int sig, void *userdata) {
    struct userdata *u = userdata;
    pa_cvolume old_vol;

    assert(u);

    if (u->sink) {
        assert(u->sink->get_hw_volume);
        memcpy(&old_vol, &u->sink->hw_volume, sizeof(pa_cvolume));
        if (u->sink->get_hw_volume(u->sink) < 0)
            return;
        if (memcmp(&old_vol, &u->sink->hw_volume, sizeof(pa_cvolume)) != 0) {
            pa_subscription_post(u->sink->core,
                PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE,
                u->sink->index);
        }
    }

    if (u->source) {
        assert(u->source->get_hw_volume);
        memcpy(&old_vol, &u->source->hw_volume, sizeof(pa_cvolume));
        if (u->source->get_hw_volume(u->source) < 0)
            return;
        if (memcmp(&old_vol, &u->source->hw_volume, sizeof(pa_cvolume)) != 0) {
            pa_subscription_post(u->source->core,
                PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE,
                u->source->index);
        }
    }
}

static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    pa_usec_t r = 0;
    audio_info_t info;
    int err;
    struct userdata *u = s->userdata;
    assert(s && u && u->sink);

    err = ioctl(u->fd, AUDIO_GETINFO, &info);
    assert(err >= 0);

    r += pa_bytes_to_usec(u->written_bytes, &s->sample_spec);
    r -= pa_bytes_to_usec(info.play.samples * u->frame_size, &s->sample_spec);

    if (u->memchunk.memblock)
        r += pa_bytes_to_usec(u->memchunk.length, &s->sample_spec);

    return r;
}

static pa_usec_t source_get_latency_cb(pa_source *s) {
    pa_usec_t r = 0;
    struct userdata *u = s->userdata;
    audio_info_t info;
    int err;
    assert(s && u && u->source);

    err = ioctl(u->fd, AUDIO_GETINFO, &info);
    assert(err >= 0);

    r += pa_bytes_to_usec(info.record.samples * u->frame_size, &s->sample_spec);
    r -= pa_bytes_to_usec(u->read_bytes, &s->sample_spec);

    return r;
}

static int sink_get_hw_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    audio_info_t info;
    int err;

    err = ioctl(u->fd, AUDIO_GETINFO, &info);
    assert(err >= 0);

    pa_cvolume_set(&s->hw_volume, s->hw_volume.channels,
        info.play.gain * PA_VOLUME_NORM / AUDIO_MAX_GAIN);

    return 0;
}

static int sink_set_hw_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    audio_info_t info;

    AUDIO_INITINFO(&info);

    info.play.gain = pa_cvolume_avg(&s->hw_volume) * AUDIO_MAX_GAIN / PA_VOLUME_NORM;
    assert(info.play.gain <= AUDIO_MAX_GAIN);

    if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0) {
        if (errno == EINVAL)
            pa_log("AUDIO_SETINFO: Unsupported volume.");
        else
            pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

static int sink_get_hw_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    audio_info_t info;
    int err;

    err = ioctl(u->fd, AUDIO_GETINFO, &info);
    assert(err >= 0);

    s->hw_muted = !!info.output_muted;

    return 0;
}

static int sink_set_hw_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    audio_info_t info;

    AUDIO_INITINFO(&info);

    info.output_muted = !!s->hw_muted;

    if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0) {
        pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

static int source_get_hw_volume_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    audio_info_t info;
    int err;

    err = ioctl(u->fd, AUDIO_GETINFO, &info);
    assert(err >= 0);

    pa_cvolume_set(&s->hw_volume, s->hw_volume.channels,
        info.record.gain * PA_VOLUME_NORM / AUDIO_MAX_GAIN);

    return 0;
}

static int source_set_hw_volume_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    audio_info_t info;

    AUDIO_INITINFO(&info);

    info.record.gain = pa_cvolume_avg(&s->hw_volume) * AUDIO_MAX_GAIN / PA_VOLUME_NORM;
    assert(info.record.gain <= AUDIO_MAX_GAIN);

    if (ioctl(u->fd, AUDIO_SETINFO, &info) < 0) {
        if (errno == EINVAL)
            pa_log("AUDIO_SETINFO: Unsupported volume.");
        else
            pa_log("AUDIO_SETINFO: %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
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

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    const char *p;
    int fd = -1;
    int buffer_size;
    int mode;
    int record = 1, playback = 1;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    struct timeval tv;
    char *t;
    assert(c && m);

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

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("failed to parse sample specification");
        goto fail;
    }

    if ((fd = open(p = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), mode | O_NONBLOCK)) < 0)
        goto fail;

    pa_log_info("device opened in %s mode.", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));

    if (pa_solaris_auto_format(fd, mode, &ss) < 0)
        goto fail;

    if ((mode != O_WRONLY) && (buffer_size >= 1))
        if (pa_solaris_set_buffer(fd, buffer_size) < 0)
            goto fail;

    u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;

    if (mode != O_WRONLY) {
        u->source = pa_source_new(c, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss, &map);
        assert(u->source);
        u->source->userdata = u;
        u->source->get_latency = source_get_latency_cb;
        u->source->get_hw_volume = source_get_hw_volume_cb;
        u->source->set_hw_volume = source_set_hw_volume_cb;
        pa_source_set_owner(u->source, m);
        pa_source_set_description(u->source, t = pa_sprintf_malloc("Solaris PCM on '%s'", p));
        pa_xfree(t);
        u->source->is_hardware = 1;
    } else
        u->source = NULL;

    if (mode != O_RDONLY) {
        u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, &map);
        assert(u->sink);
        u->sink->get_latency = sink_get_latency_cb;
        u->sink->get_hw_volume = sink_get_hw_volume_cb;
        u->sink->set_hw_volume = sink_set_hw_volume_cb;
        u->sink->get_hw_mute = sink_get_hw_mute_cb;
        u->sink->set_hw_mute = sink_set_hw_mute_cb;
        u->sink->userdata = u;
        pa_sink_set_owner(u->sink, m);
        pa_sink_set_description(u->sink, t = pa_sprintf_malloc("Solaris PCM on '%s'", p));
        pa_xfree(t);
        u->sink->is_hardware = 1;
    } else
        u->sink = NULL;

    assert(u->source || u->sink);

    u->io = pa_iochannel_new(c->mainloop, u->source ? fd : -1, u->sink ? fd : 0);
    assert(u->io);
    pa_iochannel_set_callback(u->io, io_callback, u);
    u->fd = fd;

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;

    /* We use this to get a reasonable chunk size */
    u->page_size = sysconf(_SC_PAGESIZE);

    u->frame_size = pa_frame_size(&ss);
    u->buffer_size = buffer_size;

    u->written_bytes = 0;
    u->read_bytes = 0;

    u->sink_underflow = 1;

    u->module = m;
    m->userdata = u;

    u->poll_timeout = pa_bytes_to_usec(u->buffer_size / 10, &ss);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, u->poll_timeout);

    u->timer = c->mainloop->time_new(c->mainloop, &tv, timer_cb, u);
    assert(u->timer);

    u->sig = pa_signal_new(SIGPOLL, sig_callback, u);
    assert(u->sig);
    ioctl(u->fd, I_SETSIG, S_MSG);

    pa_modargs_free(ma);

    /* Read mixer settings */
    if (u->source)
        source_get_hw_volume_cb(u->source);
    if (u->sink) {
        sink_get_hw_volume_cb(u->sink);
        sink_get_hw_mute_cb(u->sink);
    }

    return 0;

fail:
    if (fd >= 0)
        close(fd);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    if (u->timer)
        c->mainloop->time_free(u->timer);
    ioctl(u->fd, I_SETSIG, 0);
    pa_signal_free(u->sig);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->source) {
        pa_source_disconnect(u->source);
        pa_source_unref(u->source);
    }

    pa_iochannel_free(u->io);
    pa_xfree(u);
}
