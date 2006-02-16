/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
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
#include <sys/mman.h>

#include <polypcore/iochannel.h>
#include <polypcore/sink.h>
#include <polypcore/source.h>
#include <polypcore/module.h>
#include <polypcore/oss-util.h>
#include <polypcore/sample-util.h>
#include <polypcore/util.h>
#include <polypcore/modargs.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>

#include "module-oss-mmap-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("OSS Sink/Source (mmap)")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("sink_name=<name for the sink> source_name=<name for the source> device=<OSS device> record=<enable source?> playback=<enable sink?> format=<sample format> channels=<number of channels> rate=<sample rate> fragments=<number of fragments> fragment_size=<fragment size>")

struct userdata {
    pa_sink *sink;
    pa_source *source;
    pa_core *core;
    pa_sample_spec sample_spec;

    size_t in_fragment_size, out_fragment_size, in_fragments, out_fragments, out_fill;

    int fd;

    void *in_mmap, *out_mmap;
    size_t in_mmap_length, out_mmap_length;

    pa_io_event *io_event;

    pa_memblock **in_memblocks, **out_memblocks;
    unsigned out_current, in_current;
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
    NULL
};

#define DEFAULT_SINK_NAME "oss_output"
#define DEFAULT_SOURCE_NAME "oss_input"
#define DEFAULT_DEVICE "/dev/dsp"

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->sink ? pa_idxset_size(u->sink->inputs) : 0) +
                      (u->sink ? pa_idxset_size(u->sink->monitor_source->outputs) : 0) +
                      (u->source ? pa_idxset_size(u->source->outputs) : 0));
}

static void out_fill_memblocks(struct userdata *u, unsigned n) {
    assert(u && u->out_memblocks);
    
    while (n > 0) {
        pa_memchunk chunk;
        
        if (u->out_memblocks[u->out_current])
            pa_memblock_unref_fixed(u->out_memblocks[u->out_current]);
            
        chunk.memblock = u->out_memblocks[u->out_current] = pa_memblock_new_fixed((uint8_t*)u->out_mmap+u->out_fragment_size*u->out_current, u->out_fragment_size, 1, u->core->memblock_stat);
        assert(chunk.memblock);
        chunk.length = chunk.memblock->length;
        chunk.index = 0;
        
        pa_sink_render_into_full(u->sink, &chunk);
            
        u->out_current++;
        while (u->out_current >= u->out_fragments)
            u->out_current -= u->out_fragments;
        
        n--;
    }
}

static void do_write(struct userdata *u) {
    struct count_info info;
    assert(u && u->sink);

    update_usage(u);
    
    if (ioctl(u->fd, SNDCTL_DSP_GETOPTR, &info) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_GETOPTR: %s\n", strerror(errno));
        return;
    }

    u->out_fill = (u->out_fragment_size * u->out_fragments) - (info.ptr % u->out_fragment_size);

    if (!info.blocks)
        return;
    
    out_fill_memblocks(u, info.blocks);
}

static void in_post_memblocks(struct userdata *u, unsigned n) {
    assert(u && u->in_memblocks);
    
    while (n > 0) {
        pa_memchunk chunk;
        
        if (!u->in_memblocks[u->in_current]) {
            chunk.memblock = u->in_memblocks[u->in_current] = pa_memblock_new_fixed((uint8_t*) u->in_mmap+u->in_fragment_size*u->in_current, u->in_fragment_size, 1, u->core->memblock_stat);
            chunk.length = chunk.memblock->length;
            chunk.index = 0;
            
            pa_source_post(u->source, &chunk);
        }

        u->in_current++;
        while (u->in_current >= u->in_fragments)
            u->in_current -= u->in_fragments;
        
        n--;
    }
}

static void in_clear_memblocks(struct userdata*u, unsigned n) {
    unsigned i = u->in_current;
    assert(u && u->in_memblocks);

    if (n > u->in_fragments)
        n = u->in_fragments;
    
    while (n > 0) {
        if (u->in_memblocks[i]) {
            pa_memblock_unref_fixed(u->in_memblocks[i]);
            u->in_memblocks[i] = NULL;
        }

        i++;
        while (i >= u->in_fragments)
            i -= u->in_fragments;

        n--;
    }
}

static void do_read(struct userdata *u) {
    struct count_info info;
    assert(u && u->source);

    update_usage(u);
    
    if (ioctl(u->fd, SNDCTL_DSP_GETIPTR, &info) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_GETIPTR: %s\n", strerror(errno));
        return;
    }

    if (!info.blocks)
        return;
    
    in_post_memblocks(u, info.blocks);
    in_clear_memblocks(u, u->in_fragments/2);
}

static void io_callback(pa_mainloop_api *m, pa_io_event *e, PA_GCC_UNUSED int fd, pa_io_event_flags_t f, void *userdata) {
    struct userdata *u = userdata;
    assert (u && u->core->mainloop == m && u->io_event == e);

    if (f & PA_IO_EVENT_INPUT)
        do_read(u);
    if (f & PA_IO_EVENT_OUTPUT)
        do_write(u);
}

static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    assert(s && u);

    do_write(u);
    return pa_bytes_to_usec(u->out_fill, &s->sample_spec);
}

int pa__init(pa_core *c, pa_module*m) {
    struct audio_buf_info info;
    struct userdata *u = NULL;
    const char *p;
    int nfrags, frag_size;
    int mode, caps;
    int enable_bits = 0, zero = 0;
    int playback = 1, record = 1;
    pa_modargs *ma = NULL;
    assert(c && m);

    m->userdata = u = pa_xmalloc0(sizeof(struct userdata));
    u->module = m;
    u->fd = -1;
    u->core = c;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments.\n");
        goto fail;
    }
    
    if (pa_modargs_get_value_boolean(ma, "record", &record) < 0 || pa_modargs_get_value_boolean(ma, "playback", &playback) < 0) {
        pa_log(__FILE__": record= and playback= expect numeric arguments.\n");
        goto fail;
    }

    if (!playback && !record) {
        pa_log(__FILE__": neither playback nor record enabled for device.\n");
        goto fail;
    }

    mode = (playback&&record) ? O_RDWR : (playback ? O_WRONLY : (record ? O_RDONLY : 0));

    nfrags = 12;
    frag_size = 1024;
    if (pa_modargs_get_value_s32(ma, "fragments", &nfrags) < 0 || pa_modargs_get_value_s32(ma, "fragment_size", &frag_size) < 0) {
        pa_log(__FILE__": failed to parse fragments arguments\n");
        goto fail;
    }

    u->sample_spec = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &u->sample_spec) < 0) {
        pa_log(__FILE__": failed to parse sample specification\n");
        goto fail;
    }

    if ((u->fd = pa_oss_open(p = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), &mode, &caps)) < 0)
        goto fail;

    if (!(caps & DSP_CAP_MMAP) || !(caps & DSP_CAP_REALTIME) || !(caps & DSP_CAP_TRIGGER)) {
        pa_log(__FILE__": OSS device not mmap capable.\n");
        goto fail;
    }

    pa_log(__FILE__": device opened in %s mode.\n", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));

    if (nfrags >= 2 && frag_size >= 1)
        if (pa_oss_set_fragments(u->fd, nfrags, frag_size) < 0)
            goto fail;
    
    if (pa_oss_auto_format(u->fd, &u->sample_spec) < 0)
        goto fail;

    if (mode != O_WRONLY) {
        if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) < 0) {
            pa_log(__FILE__": SNDCTL_DSP_GETISPACE: %s\n", strerror(errno));
            goto fail;
        }

        pa_log_info(__FILE__": input -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        u->in_mmap_length = (u->in_fragment_size = info.fragsize) * (u->in_fragments = info.fragstotal);

        if ((u->in_mmap = mmap(NULL, u->in_mmap_length, PROT_READ, MAP_SHARED, u->fd, 0)) == MAP_FAILED) {
            if (mode == O_RDWR) {
                pa_log(__FILE__": mmap failed for input. Changing to O_WRONLY mode.\n");
                mode = O_WRONLY;
            } else {
                pa_log(__FILE__": mmap(): %s\n", strerror(errno));
                goto fail;
            }
        } else {
        
            u->source = pa_source_new(c, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &u->sample_spec, NULL);
            assert(u->source);
            u->source->userdata = u;
            pa_source_set_owner(u->source, m);
            u->source->description = pa_sprintf_malloc("Open Sound System PCM/mmap() on '%s'", p);
            
            u->in_memblocks = pa_xmalloc0(sizeof(pa_memblock *)*u->in_fragments);
            
            enable_bits |= PCM_ENABLE_INPUT;
        }
    }

    if (mode != O_RDONLY) {
        if (ioctl(u->fd, SNDCTL_DSP_GETOSPACE, &info) < 0) {
            pa_log(__FILE__": SNDCTL_DSP_GETOSPACE: %s\n", strerror(errno));
            goto fail;
        }
        
        pa_log_info(__FILE__": output -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        u->out_mmap_length = (u->out_fragment_size = info.fragsize) * (u->out_fragments = info.fragstotal);

        if ((u->out_mmap = mmap(NULL, u->out_mmap_length, PROT_WRITE, MAP_SHARED, u->fd, 0))  == MAP_FAILED) {
            if (mode == O_RDWR) {
                pa_log(__FILE__": mmap filed for output. Changing to O_RDONLY mode.\n");
                mode = O_RDONLY;
            } else {
                pa_log(__FILE__": mmap(): %s\n", strerror(errno));
                goto fail;
            }
        } else {
            pa_silence_memory(u->out_mmap, u->out_mmap_length, &u->sample_spec);
            
            u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &u->sample_spec, NULL);
            assert(u->sink);
            u->sink->get_latency = sink_get_latency_cb;
            u->sink->userdata = u;
            pa_sink_set_owner(u->sink, m);
            u->sink->description = pa_sprintf_malloc("Open Sound System PCM/mmap() on '%s'", p);
            
            u->out_memblocks = pa_xmalloc0(sizeof(struct memblock *)*u->out_fragments);
            
            enable_bits |= PCM_ENABLE_OUTPUT;
        }
    }

    zero = 0;
    if (ioctl(u->fd, SNDCTL_DSP_SETTRIGGER, &zero) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_SETTRIGGER: %s\n", strerror(errno));
        goto fail;
    }
    
    if (ioctl(u->fd, SNDCTL_DSP_SETTRIGGER, &enable_bits) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_SETTRIGGER: %s\n", strerror(errno));
        goto fail;
    }
        
    assert(u->source || u->sink);

    u->io_event = c->mainloop->io_new(c->mainloop, u->fd, (u->source ? PA_IO_EVENT_INPUT : 0) | (u->sink ? PA_IO_EVENT_OUTPUT : 0), io_callback, u);
    assert(u->io_event);

    pa_modargs_free(ma);
    
    return 0;

fail:
    pa__done(c, m);

    if (ma)
        pa_modargs_free(ma);
    
    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;

    if (u->out_memblocks) {
        unsigned i;
        for (i = 0; i < u->out_fragments; i++)
            if (u->out_memblocks[i])
                pa_memblock_unref_fixed(u->out_memblocks[i]);
        pa_xfree(u->out_memblocks);
    }

    if (u->in_memblocks) {
        unsigned i;
        for (i = 0; i < u->in_fragments; i++)
            if (u->in_memblocks[i])
                pa_memblock_unref_fixed(u->in_memblocks[i]);
        pa_xfree(u->in_memblocks);
    }
    
    if (u->in_mmap && u->in_mmap != MAP_FAILED)
        munmap(u->in_mmap, u->in_mmap_length);
    
    if (u->out_mmap && u->out_mmap != MAP_FAILED)
        munmap(u->out_mmap, u->out_mmap_length);
    
    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->source) {
        pa_source_disconnect(u->source);
        pa_source_unref(u->source);
    }

    if (u->io_event)
        u->core->mainloop->io_free(u->io_event);

    if (u->fd >= 0)
        close(u->fd);

    pa_xfree(u);
}
