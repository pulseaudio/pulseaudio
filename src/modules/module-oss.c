/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
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

#include <sys/soundcard.h>
#include <sys/ioctl.h>
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
#include <pulse/util.h>

#include <pulsecore/core-error.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>

#include "oss-util.h"
#include "module-oss-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("OSS Sink/Source")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "source_name=<name for the source> "
        "device=<OSS device> "
        "record=<enable source?> "
        "playback=<enable sink?> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "channel_map=<channel map>")

struct userdata {
    pa_sink *sink;
    pa_source *source;
    pa_iochannel *io;
    pa_core *core;

    pa_memchunk memchunk, silence;

    uint32_t in_fragment_size, out_fragment_size, sample_size;
    int use_getospace, use_getispace;

    int fd;
    pa_module *module;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "source_name",
    "device",
    "record",
    "playback",
    "fragments",
    "fragment_size",
    "format",
    "rate",
    "channels",
    "channel_map",
    NULL
};

#define DEFAULT_DEVICE "/dev/dsp"

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->sink ? pa_sink_used_by(u->sink) : 0) +
                      (u->source ? pa_source_used_by(u->source) : 0));
}

static void clear_up(struct userdata *u) {
    assert(u);

    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }

    if (u->source) {
        pa_source_disconnect(u->source);
        pa_source_unref(u->source);
        u->source = NULL;
    }

    if (u->io) {
        pa_iochannel_free(u->io);
        u->io = NULL;
    }
}

static void do_write(struct userdata *u) {
    pa_memchunk *memchunk;
    ssize_t r;
    size_t l;
    int loop = 0;

    assert(u);

    if (!u->sink || !pa_iochannel_is_writable(u->io))
        return;

    update_usage(u);

    l = u->out_fragment_size;

    if (u->use_getospace) {
        audio_buf_info info;

        if (ioctl(u->fd, SNDCTL_DSP_GETOSPACE, &info) < 0)
            u->use_getospace = 0;
        else {
            if (info.bytes/l > 0) {
                l = (info.bytes/l)*l;
                loop = 1;
            }
        }
    }

    do {
        memchunk = &u->memchunk;

        if (!memchunk->length)
            if (pa_sink_render(u->sink, l, memchunk) < 0)
                memchunk = &u->silence;

        assert(memchunk->memblock);
        assert(memchunk->memblock->data);
        assert(memchunk->length);

        if ((r = pa_iochannel_write(u->io, (uint8_t*) memchunk->memblock->data + memchunk->index, memchunk->length)) < 0) {

            if (errno != EAGAIN) {
                pa_log("write() failed: %s", pa_cstrerror(errno));

                clear_up(u);
                pa_module_unload_request(u->module);
            }

            break;
        }

        if (memchunk == &u->silence)
            assert(r % u->sample_size == 0);
        else {
            u->memchunk.index += r;
            u->memchunk.length -= r;

            if (u->memchunk.length <= 0) {
                pa_memblock_unref(u->memchunk.memblock);
                u->memchunk.memblock = NULL;
            }
        }

        l = l > (size_t) r ? l - r : 0;
    } while (loop && l > 0);
}

static void do_read(struct userdata *u) {
    pa_memchunk memchunk;
    ssize_t r;
    size_t l;
    int loop = 0;
    assert(u);

    if (!u->source || !pa_iochannel_is_readable(u->io) || !pa_idxset_size(u->source->outputs))
        return;

    update_usage(u);

    l = u->in_fragment_size;

    if (u->use_getispace) {
        audio_buf_info info;

        if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) < 0)
            u->use_getispace = 0;
        else {
            if (info.bytes/l > 0) {
                l = (info.bytes/l)*l;
                loop = 1;
            }
        }
    }

    do {
        memchunk.memblock = pa_memblock_new(u->core->mempool, l);
        assert(memchunk.memblock);
        if ((r = pa_iochannel_read(u->io, memchunk.memblock->data, memchunk.memblock->length)) < 0) {
            pa_memblock_unref(memchunk.memblock);

            if (errno != EAGAIN) {
                pa_log("read() failed: %s", pa_cstrerror(errno));

                clear_up(u);
                pa_module_unload_request(u->module);
            }

            break;
        }

        assert(r <= (ssize_t) memchunk.memblock->length);
        memchunk.length = memchunk.memblock->length = r;
        memchunk.index = 0;

        pa_source_post(u->source, &memchunk);
        pa_memblock_unref(memchunk.memblock);

        l = l > (size_t) r ? l - r : 0;
    } while (loop && l > 0);
}

static void io_callback(PA_GCC_UNUSED pa_iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
    do_read(u);
}

static void source_notify_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    assert(u);
    do_read(u);
}

static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    pa_usec_t r = 0;
    int arg;
    struct userdata *u = s->userdata;
    assert(s && u && u->sink);

    if (ioctl(u->fd, SNDCTL_DSP_GETODELAY, &arg) < 0) {
        pa_log_info("device doesn't support SNDCTL_DSP_GETODELAY: %s", pa_cstrerror(errno));
        s->get_latency = NULL;
        return 0;
    }

    r += pa_bytes_to_usec(arg, &s->sample_spec);

    if (u->memchunk.memblock)
        r += pa_bytes_to_usec(u->memchunk.length, &s->sample_spec);

    return r;
}

static pa_usec_t source_get_latency_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    audio_buf_info info;
    assert(s && u && u->source);

    if (!u->use_getispace)
        return 0;

    if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) < 0) {
        u->use_getispace = 0;
        return 0;
    }

    if (info.bytes <= 0)
        return 0;

    return pa_bytes_to_usec(info.bytes, &s->sample_spec);
}

static int sink_get_hw_volume(pa_sink *s) {
    struct userdata *u = s->userdata;

    if (pa_oss_get_pcm_volume(u->fd, &s->sample_spec, &s->hw_volume) < 0) {
        pa_log_info("device doesn't support reading mixer settings: %s", pa_cstrerror(errno));
        s->get_hw_volume = NULL;
        return -1;
    }

    return 0;
}

static int sink_set_hw_volume(pa_sink *s) {
    struct userdata *u = s->userdata;

    if (pa_oss_set_pcm_volume(u->fd, &s->sample_spec, &s->hw_volume) < 0) {
        pa_log_info("device doesn't support writing mixer settings: %s", pa_cstrerror(errno));
        s->set_hw_volume = NULL;
        return -1;
    }

    return 0;
}

static int source_get_hw_volume(pa_source *s) {
    struct userdata *u = s->userdata;

    if (pa_oss_get_input_volume(u->fd, &s->sample_spec, &s->hw_volume) < 0) {
        pa_log_info("device doesn't support reading mixer settings: %s", pa_cstrerror(errno));
        s->get_hw_volume = NULL;
        return -1;
    }

    return 0;
}

static int source_set_hw_volume(pa_source *s) {
    struct userdata *u = s->userdata;

    if (pa_oss_set_input_volume(u->fd, &s->sample_spec, &s->hw_volume) < 0) {
        pa_log_info("device doesn't support writing mixer settings: %s", pa_cstrerror(errno));
        s->set_hw_volume = NULL;
        return -1;
    }

    return 0;
}

int pa__init(pa_core *c, pa_module*m) {
    struct audio_buf_info info;
    struct userdata *u = NULL;
    const char *p;
    int fd = -1;
    int nfrags, frag_size, in_frag_size, out_frag_size;
    int mode;
    int record = 1, playback = 1;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    char hwdesc[64], *t;
    const char *name;
    char *name_buf = NULL;
    int namereg_fail;

    assert(c);
    assert(m);

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

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_OSS) < 0) {
        pa_log("failed to parse sample specification or channel map");
        goto fail;
    }

    /* Fix latency to 100ms */
    nfrags = 12;
    frag_size = pa_bytes_per_second(&ss)/128;

    if (pa_modargs_get_value_s32(ma, "fragments", &nfrags) < 0 || pa_modargs_get_value_s32(ma, "fragment_size", &frag_size) < 0) {
        pa_log("failed to parse fragments arguments");
        goto fail;
    }

    if ((fd = pa_oss_open(p = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), &mode, NULL)) < 0)
        goto fail;

    if (pa_oss_get_hw_description(p, hwdesc, sizeof(hwdesc)) >= 0)
        pa_log_info("hardware name is '%s'.", hwdesc);
    else
        hwdesc[0] = 0;

    pa_log_info("device opened in %s mode.", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));

    if (nfrags >= 2 && frag_size >= 1)
        if (pa_oss_set_fragments(fd, nfrags, frag_size) < 0)
            goto fail;

    if (pa_oss_auto_format(fd, &ss) < 0)
        goto fail;

    if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &frag_size) < 0) {
        pa_log("SNDCTL_DSP_GETBLKSIZE: %s", pa_cstrerror(errno));
        goto fail;
    }
    assert(frag_size);
    in_frag_size = out_frag_size = frag_size;

    u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;
    u->use_getospace = u->use_getispace = 0;

    if (ioctl(fd, SNDCTL_DSP_GETISPACE, &info) >= 0) {
        pa_log_info("input -- %u fragments of size %u.", info.fragstotal, info.fragsize);
        in_frag_size = info.fragsize;
        u->use_getispace = 1;
    }

    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) >= 0) {
        pa_log_info("output -- %u fragments of size %u.", info.fragstotal, info.fragsize);
        out_frag_size = info.fragsize;
        u->use_getospace = 1;
    }

    if (mode != O_WRONLY) {
        if ((name = pa_modargs_get_value(ma, "source_name", NULL)))
            namereg_fail = 1;
        else {
            name = name_buf = pa_sprintf_malloc("oss_input.%s", pa_path_get_filename(p));
            namereg_fail = 0;
        }

        if (!(u->source = pa_source_new(c, __FILE__, name, namereg_fail, &ss, &map)))
            goto fail;

        u->source->userdata = u;
        u->source->notify = source_notify_cb;
        u->source->get_latency = source_get_latency_cb;
        u->source->get_hw_volume = source_get_hw_volume;
        u->source->set_hw_volume = source_set_hw_volume;
        pa_source_set_owner(u->source, m);
        pa_source_set_description(u->source, t = pa_sprintf_malloc("OSS PCM on %s%s%s%s",
                                                                 p,
                                                                 hwdesc[0] ? " (" : "",
                                                                 hwdesc[0] ? hwdesc : "",
                                                                 hwdesc[0] ? ")" : ""));
        pa_xfree(t);
        u->source->is_hardware = 1;
    } else
        u->source = NULL;

    pa_xfree(name_buf);
    name_buf = NULL;

    if (mode != O_RDONLY) {
        if ((name = pa_modargs_get_value(ma, "sink_name", NULL)))
            namereg_fail = 1;
        else {
            name = name_buf = pa_sprintf_malloc("oss_output.%s", pa_path_get_filename(p));
            namereg_fail = 0;
        }

        if (!(u->sink = pa_sink_new(c, __FILE__, name, namereg_fail, &ss, &map)))
            goto fail;

        u->sink->get_latency = sink_get_latency_cb;
        u->sink->get_hw_volume = sink_get_hw_volume;
        u->sink->set_hw_volume = sink_set_hw_volume;
        u->sink->userdata = u;
        pa_sink_set_owner(u->sink, m);
        pa_sink_set_description(u->sink, t = pa_sprintf_malloc("OSS PCM on %s%s%s%s",
                                                           p,
                                                           hwdesc[0] ? " (" : "",
                                                           hwdesc[0] ? hwdesc : "",
                                                           hwdesc[0] ? ")" : ""));
        pa_xfree(t);
        u->sink->is_hardware = 1;
    } else
        u->sink = NULL;

    pa_xfree(name_buf);
    name_buf = NULL;

    assert(u->source || u->sink);

    u->io = pa_iochannel_new(c->mainloop, u->source ? fd : -1, u->sink ? fd : -1);
    assert(u->io);
    pa_iochannel_set_callback(u->io, io_callback, u);
    u->fd = fd;

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;
    u->sample_size = pa_frame_size(&ss);

    u->out_fragment_size = out_frag_size;
    u->in_fragment_size = in_frag_size;
    u->silence.memblock = pa_memblock_new(u->core->mempool, u->silence.length = u->out_fragment_size);
    assert(u->silence.memblock);
    pa_silence_memblock(u->silence.memblock, &ss);
    u->silence.index = 0;

    u->module = m;
    m->userdata = u;

    pa_modargs_free(ma);

    /*
     * Some crappy drivers do not start the recording until we read something.
     * Without this snippet, poll will never register the fd as ready.
     */
    if (u->source) {
        char *buf = pa_xnew(char, u->sample_size);
        pa_read(u->fd, buf, u->sample_size, NULL);
        pa_xfree(buf);
    }

    /* Read mixer settings */
    if (u->source)
        source_get_hw_volume(u->source);
    if (u->sink)
        sink_get_hw_volume(u->sink);

    return 0;

fail:
    if (fd >= 0)
        close(fd);

    if (ma)
        pa_modargs_free(ma);

    pa_xfree(name_buf);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;

    assert(c);
    assert(m);

    if (!(u = m->userdata))
        return;

    clear_up(u);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
    if (u->silence.memblock)
        pa_memblock_unref(u->silence.memblock);

    pa_xfree(u);
}
