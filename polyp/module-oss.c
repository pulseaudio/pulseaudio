/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
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

#include "iochannel.h"
#include "sink.h"
#include "source.h"
#include "module.h"
#include "oss-util.h"
#include "sample-util.h"
#include "util.h"
#include "modargs.h"
#include "xmalloc.h"

struct userdata {
    struct pa_sink *sink;
    struct pa_source *source;
    struct pa_iochannel *io;
    struct pa_core *core;

    struct pa_memchunk memchunk, silence;

    uint32_t in_fragment_size, out_fragment_size, sample_size;

    int fd;
    struct pa_module *module;
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
    NULL
};

#define DEFAULT_SINK_NAME "oss_output"
#define DEFAULT_SOURCE_NAME "oss_input"
#define DEFAULT_DEVICE "/dev/dsp"

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->sink ? pa_idxset_ncontents(u->sink->inputs) : 0) +
                      (u->sink ? pa_idxset_ncontents(u->sink->monitor_source->outputs) : 0) +
                      (u->source ? pa_idxset_ncontents(u->source->outputs) : 0));
}

static void do_write(struct userdata *u) {
    struct pa_memchunk *memchunk;
    ssize_t r;
    assert(u);

    if (!u->sink || !pa_iochannel_is_writable(u->io))
        return;

    update_usage(u);
     
    if (!u->memchunk.length) {
        if (pa_sink_render(u->sink, u->out_fragment_size, &u->memchunk) < 0)
            memchunk = &u->silence;
        else
            memchunk = &u->memchunk;
    }

    assert(memchunk->memblock && memchunk->length);
    
    if ((r = pa_iochannel_write(u->io, memchunk->memblock->data + memchunk->index, memchunk->length)) < 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        return;
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
}

static void do_read(struct userdata *u) {
    struct pa_memchunk memchunk;
    ssize_t r;
    assert(u);
    
    if (!u->source || !pa_iochannel_is_readable(u->io))
        return;

    update_usage(u);

    memchunk.memblock = pa_memblock_new(u->in_fragment_size, u->core->memblock_stat);
    assert(memchunk.memblock);
    if ((r = pa_iochannel_read(u->io, memchunk.memblock->data, memchunk.memblock->length)) < 0) {
        pa_memblock_unref(memchunk.memblock);
        if (errno != EAGAIN)
            fprintf(stderr, "read() failed: %s\n", strerror(errno));
        return;
    }

    assert(r <= (ssize_t) memchunk.memblock->length);
    memchunk.length = memchunk.memblock->length = r;
    memchunk.index = 0;

    pa_source_post(u->source, &memchunk);
    pa_memblock_unref(memchunk.memblock);
}

static void io_callback(struct pa_iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
    do_read(u);
}

static uint32_t sink_get_latency_cb(struct pa_sink *s) {
    int arg;
    struct userdata *u = s->userdata;
    assert(s && u && u->sink);

    if (ioctl(u->fd, SNDCTL_DSP_GETODELAY, &arg) < 0) {
        fprintf(stderr, "module-oss: device doesn't support SNDCTL_DSP_GETODELAY.\n");
        s->get_latency = NULL;
        return 0;
    }

    return pa_bytes_to_usec(arg, &s->sample_spec);
}

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct audio_buf_info info;
    struct userdata *u = NULL;
    const char *p;
    int fd = -1;
    int nfrags, frag_size, in_frag_size, out_frag_size;
    int mode;
    uint32_t record = 1, playback = 1;
    struct pa_sample_spec ss;
    struct pa_modargs *ma = NULL;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        fprintf(stderr, __FILE__": failed to parse module arguments.\n");
        goto fail;
    }
    
    if (pa_modargs_get_value_u32(ma, "record", &record) < 0 || pa_modargs_get_value_u32(ma, "playback", &playback) < 0) {
        fprintf(stderr, __FILE__": record= and playback= expect numeric argument.\n");
        goto fail;
    }

    mode = (playback&&record) ? O_RDWR : (playback ? O_WRONLY : (record ? O_RDONLY : 0));
    if (mode == 0) {
        fprintf(stderr, __FILE__": neither playback nor record enabled for device.\n");
        goto fail;
    }

    nfrags = 12;
    frag_size = 1024;
    if (pa_modargs_get_value_u32(ma, "fragments", &nfrags) < 0 || nfrags < 2 || pa_modargs_get_value_u32(ma, "fragment_size", &frag_size) < 0 || frag_size < 1) {
        fprintf(stderr, __FILE__": failed to parse fragments arguments\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        fprintf(stderr, __FILE__": failed to parse sample specification\n");
        goto fail;
    }
    
    if ((fd = pa_oss_open(p = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), &mode, NULL)) < 0)
        goto fail;

    fprintf(stderr, "module-oss: device opened in %s mode.\n", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));

    if (pa_oss_set_fragments(fd, nfrags, frag_size) < 0)
        goto fail;

    if (pa_oss_auto_format(fd, &ss) < 0)
        goto fail;

    if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &frag_size) < 0) {
        fprintf(stderr, "SNDCTL_DSP_GETBLKSIZE: %s\n", strerror(errno));
        goto fail;
    }
    assert(frag_size);
    in_frag_size = out_frag_size = frag_size;

    if (ioctl(fd, SNDCTL_DSP_GETISPACE, &info) >= 0) {
        fprintf(stderr, "module-oss: input -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        in_frag_size = info.fragsize;
    }

    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) >= 0) {
        fprintf(stderr, "module-oss: output -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        out_frag_size = info.fragsize;
    }

    u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;

    if (mode != O_WRONLY) {
        u->source = pa_source_new(c, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss);
        assert(u->source);
        u->source->userdata = u;
        pa_source_set_owner(u->source, m);
        u->source->description = pa_sprintf_malloc("Open Sound System PCM on '%s'", p);
    } else
        u->source = NULL;

    if (mode != O_RDONLY) {
        u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss);
        assert(u->sink);
        u->sink->get_latency = sink_get_latency_cb;
        u->sink->userdata = u;
        pa_sink_set_owner(u->sink, m);
        u->sink->description = pa_sprintf_malloc("Open Sound System PCM on '%s'", p);
    } else
        u->sink = NULL;

    assert(u->source || u->sink);

    u->io = pa_iochannel_new(c->mainloop, u->source ? fd : -1, u->sink ? fd : 0);
    assert(u->io);
    pa_iochannel_set_callback(u->io, io_callback, u);
    u->fd = fd;

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;
    u->sample_size = pa_frame_size(&ss);

    u->out_fragment_size = out_frag_size;
    u->in_fragment_size = in_frag_size;
    u->silence.memblock = pa_memblock_new(u->silence.length = u->out_fragment_size, u->core->memblock_stat);
    assert(u->silence.memblock);
    pa_silence_memblock(u->silence.memblock, &ss);
    u->silence.index = 0;

    u->module = m;
    m->userdata = u;

    pa_modargs_free(ma);

    return 0;

fail:
    if (fd >= 0)
        close(fd);

    if (ma)
        pa_modargs_free(ma);
    
    return -1;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
    if (u->silence.memblock)
        pa_memblock_unref(u->silence.memblock);

    if (u->sink)
        pa_sink_free(u->sink);
    if (u->source)
        pa_source_free(u->source);
    
    pa_iochannel_free(u->io);
    pa_xfree(u);
}
