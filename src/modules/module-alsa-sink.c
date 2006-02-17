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

#include <assert.h>
#include <stdio.h>

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#else
#include "poll.h"
#endif

#include <asoundlib.h>

#include <polypcore/core.h>
#include <polypcore/module.h>
#include <polypcore/memchunk.h>
#include <polypcore/sink.h>
#include <polypcore/modargs.h>
#include <polypcore/util.h>
#include <polypcore/sample-util.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>

#include "alsa-util.h"
#include "module-alsa-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("ALSA Sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("sink_name=<name for the sink> device=<ALSA device> format=<sample format> channels=<number of channels> rate=<sample rate> fragments=<number of fragments> fragment_size=<fragment size>")

struct userdata {
    snd_pcm_t *pcm_handle;
    pa_sink *sink;
    pa_io_event **io_events;
    unsigned n_io_events;

    size_t frame_size, fragment_size;
    pa_memchunk memchunk, silence;
    pa_module *module;
};

static const char* const valid_modargs[] = {
    "device",
    "sink_name",
    "format",
    "channels",
    "rate",
    "fragments",
    "fragment_size",
    NULL
};

#define DEFAULT_SINK_NAME "alsa_output"
#define DEFAULT_DEVICE "default"

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->sink ? pa_idxset_size(u->sink->inputs) : 0) +
                      (u->sink ? pa_idxset_size(u->sink->monitor_source->outputs) : 0));
}

static void xrun_recovery(struct userdata *u) {
    assert(u);

    pa_log(__FILE__": *** ALSA-XRUN (playback) ***\n");
    
    if (snd_pcm_prepare(u->pcm_handle) < 0)
        pa_log(__FILE__": snd_pcm_prepare() failed\n");
}

static void do_write(struct userdata *u) {
    assert(u);

    update_usage(u);
    
    for (;;) {
        pa_memchunk *memchunk = NULL;
        snd_pcm_sframes_t frames;
        
        if (u->memchunk.memblock)
            memchunk = &u->memchunk;
        else {
            if (pa_sink_render(u->sink, u->fragment_size, &u->memchunk) < 0)
                memchunk = &u->silence;
            else
                memchunk = &u->memchunk;
        }
            
        assert(memchunk->memblock && memchunk->memblock->data && memchunk->length && memchunk->memblock->length && (memchunk->length % u->frame_size) == 0);

        if ((frames = snd_pcm_writei(u->pcm_handle, (uint8_t*) memchunk->memblock->data + memchunk->index, memchunk->length / u->frame_size)) < 0) {
            if (frames == -EAGAIN)
                return;

            if (frames == -EPIPE) {
                xrun_recovery(u);
                continue;
            }

            pa_log(__FILE__": snd_pcm_writei() failed\n");
            return;
        }

        if (memchunk == &u->memchunk) {
            size_t l = frames * u->frame_size;
            memchunk->index += l;
            memchunk->length -= l;

            if (memchunk->length == 0) {
                pa_memblock_unref(memchunk->memblock);
                memchunk->memblock = NULL;
                memchunk->index = memchunk->length = 0;
            }
        }
        
        break;
    }
}

static void io_callback(pa_mainloop_api*a, pa_io_event *e, PA_GCC_UNUSED int fd, PA_GCC_UNUSED pa_io_event_flags_t f, void *userdata) {
    struct userdata *u = userdata;
    assert(u && a && e);

    if (snd_pcm_state(u->pcm_handle) == SND_PCM_STATE_XRUN)
        xrun_recovery(u);

    do_write(u);
}

static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    pa_usec_t r = 0;
    struct userdata *u = s->userdata;
    snd_pcm_sframes_t frames;
    assert(s && u && u->sink);

    if (snd_pcm_delay(u->pcm_handle, &frames) < 0) {
        pa_log(__FILE__": failed to get delay\n");
        s->get_latency = NULL;
        return 0;
    }

    if (frames < 0)
        frames = 0;
    
    r += pa_bytes_to_usec(frames * u->frame_size, &s->sample_spec);

    if (u->memchunk.memblock)
        r += pa_bytes_to_usec(u->memchunk.length, &s->sample_spec);

    return r;
}

int pa__init(pa_core *c, pa_module*m) {
    pa_modargs *ma = NULL;
    int ret = -1;
    struct userdata *u = NULL;
    const char *dev;
    pa_sample_spec ss;
    uint32_t periods, fragsize;
    snd_pcm_uframes_t period_size;
    size_t frame_size;
    int err;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": failed to parse sample specification\n");
        goto fail;
    }
    frame_size = pa_frame_size(&ss);
    
    periods = 8;
    fragsize = 1024;
    if (pa_modargs_get_value_u32(ma, "fragments", &periods) < 0 || pa_modargs_get_value_u32(ma, "fragment_size", &fragsize) < 0) {
        pa_log(__FILE__": failed to parse buffer metrics\n");
        goto fail;
    }
    period_size = fragsize;
    
    u = pa_xmalloc0(sizeof(struct userdata));
    m->userdata = u;
    u->module = m;
    
    snd_config_update_free_global();
    if ((err = snd_pcm_open(&u->pcm_handle, dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
        pa_log(__FILE__": Error opening PCM device %s: %s\n", dev, snd_strerror(err));
        goto fail;
    }

    if ((err = pa_alsa_set_hw_params(u->pcm_handle, &ss, &periods, &period_size)) < 0) {
        pa_log(__FILE__": Failed to set hardware parameters: %s\n", snd_strerror(err));
        goto fail;
    }

    u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, NULL);
    assert(u->sink);

    u->sink->get_latency = sink_get_latency_cb;
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_sprintf_malloc("Advanced Linux Sound Architecture PCM on '%s'", dev);

    if (pa_create_io_events(u->pcm_handle, c->mainloop, &u->io_events, &u->n_io_events, io_callback, u) < 0) {
        pa_log(__FILE__": failed to obtain file descriptors\n");
        goto fail;
    }
    
    u->frame_size = frame_size;
    u->fragment_size = period_size;

    pa_log_info(__FILE__": using %u fragments of size %u bytes.\n", periods, u->fragment_size);

    u->silence.memblock = pa_memblock_new(u->silence.length = u->fragment_size, c->memblock_stat);
    assert(u->silence.memblock);
    pa_silence_memblock(u->silence.memblock, &ss);
    u->silence.index = 0;

    u->memchunk.memblock = NULL;
    u->memchunk.index = u->memchunk.length = 0;
    
    ret = 0;
     
finish:
     if (ma)
         pa_modargs_free(ma);
    
    return ret;

fail:
    
    if (u)
        pa__done(c, m);

    goto finish;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
    }
    
    if (u->io_events)
        pa_free_io_events(c->mainloop, u->io_events, u->n_io_events);
    
    if (u->pcm_handle) {
        snd_pcm_drop(u->pcm_handle);
        snd_pcm_close(u->pcm_handle);
    }
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
    if (u->silence.memblock)
        pa_memblock_unref(u->silence.memblock);
    
    pa_xfree(u);
}

