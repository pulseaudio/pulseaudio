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

#include <assert.h>
#include <stdio.h>
#include <sys/poll.h>

#include <asoundlib.h>

#include "module.h"
#include "core.h"
#include "memchunk.h"
#include "sink.h"
#include "modargs.h"
#include "util.h"
#include "sample-util.h"
#include "alsa-util.h"

struct userdata {
    snd_pcm_t *pcm_handle;
    struct pa_sink *sink;
    void **io_sources;
    unsigned n_io_sources;

    size_t frame_size, fragment_size;
    struct pa_memchunk memchunk, silence;
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
#define DEFAULT_DEVICE "plughw:0,0"

static void xrun_recovery(struct userdata *u) {
    assert(u);

    fprintf(stderr, "*** ALSA-XRUN (playback) ***\n");
    
    if (snd_pcm_prepare(u->pcm_handle) < 0)
        fprintf(stderr, "snd_pcm_prepare() failed\n");
}

static void do_write(struct userdata *u) {
    assert(u);

    for (;;) {
        struct pa_memchunk *memchunk = NULL;
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

        if ((frames = snd_pcm_writei(u->pcm_handle, memchunk->memblock->data + memchunk->index, memchunk->length / u->frame_size)) < 0) {
            if (frames == -EAGAIN)
                return;

            if (frames == -EPIPE) {
                xrun_recovery(u);
                continue;
            }

            fprintf(stderr, "snd_pcm_writei() failed\n");
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

static void io_callback(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    struct userdata *u = userdata;
    assert(u && a && id);

    if (snd_pcm_state(u->pcm_handle) == SND_PCM_STATE_XRUN)
        xrun_recovery(u);

    do_write(u);
}

static uint32_t sink_get_latency_cb(struct pa_sink *s) {
    struct userdata *u = s->userdata;
    snd_pcm_sframes_t frames;
    assert(s && u && u->sink);

    if (snd_pcm_delay(u->pcm_handle, &frames) < 0) {
        fprintf(stderr, __FILE__": failed to get delay\n");
        s->get_latency = NULL;
        return 0;
    }

    if (frames < 0)
        frames = 0;
    
    return pa_samples_usec(frames * u->frame_size, &s->sample_spec);
}

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct pa_modargs *ma = NULL;
    int ret = -1;
    struct userdata *u = NULL;
    const char *dev;
    struct pa_sample_spec ss;
    unsigned periods, fragsize;
    snd_pcm_uframes_t buffer_size;
    size_t frame_size;
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        fprintf(stderr, __FILE__": failed to parse module arguments\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        fprintf(stderr, __FILE__": failed to parse sample specification\n");
        goto fail;
    }
    frame_size = pa_sample_size(&ss);
    
    periods = 12;
    fragsize = 1024;
    if (pa_modargs_get_value_u32(ma, "fragments", &periods) < 0 || pa_modargs_get_value_u32(ma, "fragment_size", &fragsize) < 0) {
        fprintf(stderr, __FILE__": failed to parse buffer metrics\n");
        goto fail;
    }
    buffer_size = fragsize/frame_size*periods;
    
    u = malloc(sizeof(struct userdata));
    assert(u);
    memset(u, 0, sizeof(struct userdata));
    m->userdata = u;
    
    if (snd_pcm_open(&u->pcm_handle, dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
        fprintf(stderr, __FILE__": Error opening PCM device %s\n", dev);
        goto fail;
    }

    if (pa_alsa_set_hw_params(u->pcm_handle, &ss, &periods, &buffer_size) < 0) {
        fprintf(stderr, __FILE__": Failed to set hardware parameters\n");
        goto fail;
    }

    u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss);
    assert(u->sink);

    u->sink->get_latency = sink_get_latency_cb;
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_sprintf_malloc("Advanced Linux Sound Architecture PCM on '%s'", dev);

    if (pa_create_io_sources(u->pcm_handle, c->mainloop, &u->io_sources, &u->n_io_sources, io_callback, u) < 0) {
        fprintf(stderr, __FILE__": failed to obtain file descriptors\n");
        goto fail;
    }
    
    u->frame_size = frame_size;
    u->fragment_size = buffer_size*u->frame_size/periods;

    fprintf(stderr, __FILE__": using %u fragments of size %u bytes.\n", periods, u->fragment_size);

    u->silence.memblock = pa_memblock_new(u->silence.length = u->fragment_size);
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
        pa_module_done(c, m);

    goto finish;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if ((u = m->userdata)) {
        if (u->sink)
            pa_sink_free(u->sink);

        if (u->io_sources)
            pa_free_io_sources(c->mainloop, u->io_sources, u->n_io_sources);
        
        if (u->pcm_handle) {
            snd_pcm_drop(u->pcm_handle);
            snd_pcm_close(u->pcm_handle);
        }

        if (u->memchunk.memblock)
            pa_memblock_unref(u->memchunk.memblock);
        if (u->silence.memblock)
            pa_memblock_unref(u->silence.memblock);
        
        free(u);
    }
}

