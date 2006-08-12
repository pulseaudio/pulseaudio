/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include <assert.h>
#include <stdio.h>

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#else
#include "poll.h"
#endif

#include <asoundlib.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/log.h>

#include "alsa-util.h"
#include "module-alsa-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("ALSA Sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "device=<ALSA device> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "channel_map=<channel map>")

struct userdata {
    snd_pcm_t *pcm_handle;
    snd_mixer_t *mixer_handle;
    snd_mixer_elem_t *mixer_elem;
    pa_sink *sink;
    struct pa_alsa_fdlist *pcm_fdl;
    struct pa_alsa_fdlist *mixer_fdl;
    long hw_volume_max, hw_volume_min;

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
    "channel_map",
    NULL
};

#define DEFAULT_DEVICE "default"

static void update_usage(struct userdata *u) {
    pa_module_set_used(u->module, u->sink ? pa_sink_used_by(u->sink) : 0);
}

static void clear_up(struct userdata *u) {
    assert(u);
    
    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }
    
    if (u->pcm_fdl)
        pa_alsa_fdlist_free(u->pcm_fdl);
    if (u->mixer_fdl)
        pa_alsa_fdlist_free(u->mixer_fdl);

    u->pcm_fdl = u->mixer_fdl = NULL;

    if (u->mixer_handle) {
        snd_mixer_close(u->mixer_handle);
        u->mixer_handle = NULL;
    }
    
    if (u->pcm_handle) {
        snd_pcm_drop(u->pcm_handle);
        snd_pcm_close(u->pcm_handle);
        u->pcm_handle = NULL;
    }
}

static int xrun_recovery(struct userdata *u) {
    int ret;
    assert(u);

    pa_log_info(__FILE__": *** ALSA-XRUN (playback) ***");
    
    if ((ret = snd_pcm_prepare(u->pcm_handle)) < 0) {
        pa_log(__FILE__": snd_pcm_prepare() failed: %s", snd_strerror(-ret));

        clear_up(u);
        pa_module_unload_request(u->module);
        return -1;
    }

    return ret;
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
                if (xrun_recovery(u) < 0)
                    return;
                
                continue;
            }

            pa_log(__FILE__": snd_pcm_writei() failed: %s", snd_strerror(-frames));

            clear_up(u);
            pa_module_unload_request(u->module);
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

static void fdl_callback(void *userdata) {
    struct userdata *u = userdata;
    assert(u);

    if (snd_pcm_state(u->pcm_handle) == SND_PCM_STATE_XRUN)
        if (xrun_recovery(u) < 0)
            return;

    do_write(u);
}

static int mixer_callback(snd_mixer_elem_t *elem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(elem);

    assert(u && u->mixer_handle);

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    if (mask & SND_CTL_EVENT_MASK_VALUE) {
        if (u->sink->get_hw_volume)
            u->sink->get_hw_volume(u->sink);
        if (u->sink->get_hw_mute)
            u->sink->get_hw_mute(u->sink);
        pa_subscription_post(u->sink->core,
            PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE,
            u->sink->index);
    }

    return 0;
}

static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    pa_usec_t r = 0;
    struct userdata *u = s->userdata;
    snd_pcm_sframes_t frames;
    int err;
    
    assert(s && u && u->sink);

    if ((err = snd_pcm_delay(u->pcm_handle, &frames)) < 0) {
        pa_log(__FILE__": failed to get delay: %s", snd_strerror(err));
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

static int sink_get_hw_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;
    int i;

    assert(u);
    assert(u->mixer_elem);

    for (i = 0; i < s->hw_volume.channels; i++) {
        long set_vol, vol;

        assert(snd_mixer_selem_has_playback_channel(u->mixer_elem, i));

        if ((err = snd_mixer_selem_get_playback_volume(u->mixer_elem, i, &vol)) < 0)
            goto fail;

        set_vol = (long) roundf(((float) s->hw_volume.values[i] * (u->hw_volume_max - u->hw_volume_min)) / PA_VOLUME_NORM) + u->hw_volume_min;

        /* Try to avoid superfluous volume changes */
        if (set_vol != vol)
            s->hw_volume.values[i] = (pa_volume_t) roundf(((float) (vol - u->hw_volume_min) * PA_VOLUME_NORM) / (u->hw_volume_max - u->hw_volume_min));
    }

    return 0;

fail:
    pa_log_error(__FILE__": Unable to read volume: %s", snd_strerror(err));
    s->get_hw_volume = NULL;
    s->set_hw_volume = NULL;
    return -1;
}

static int sink_set_hw_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;
    int i;
    pa_volume_t vol;

    assert(u);
    assert(u->mixer_elem);

    for (i = 0; i < s->hw_volume.channels; i++) {
        long alsa_vol;
        
        assert(snd_mixer_selem_has_playback_channel(u->mixer_elem, i));

        vol = s->hw_volume.values[i];

        if (vol > PA_VOLUME_NORM)
            vol = PA_VOLUME_NORM;
        
        alsa_vol = (long) roundf(((float) vol * (u->hw_volume_max - u->hw_volume_min)) / PA_VOLUME_NORM) + u->hw_volume_min;

        if ((err = snd_mixer_selem_set_playback_volume(u->mixer_elem, i, alsa_vol)) < 0)
            goto fail;
    }

    return 0;

fail:
    pa_log_error(__FILE__": Unable to set volume: %s", snd_strerror(err));
    s->get_hw_volume = NULL;
    s->set_hw_volume = NULL;
    return -1;
}

static int sink_get_hw_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err, sw;

    assert(u && u->mixer_elem);

    err = snd_mixer_selem_get_playback_switch(u->mixer_elem, 0, &sw);
    if (err) {
        pa_log_error(__FILE__": Unable to get switch: %s", snd_strerror(err));
        s->get_hw_mute = NULL;
        s->set_hw_mute = NULL;
        return -1;
    }

    s->hw_muted = !sw;

    return 0;
}

static int sink_set_hw_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;

    assert(u && u->mixer_elem);

    err = snd_mixer_selem_set_playback_switch_all(u->mixer_elem, !s->hw_muted);
    if (err) {
        pa_log_error(__FILE__": Unable to set switch: %s", snd_strerror(err));
        s->get_hw_mute = NULL;
        s->set_hw_mute = NULL;
        return -1;
    }

    return 0;
}

int pa__init(pa_core *c, pa_module*m) {
    pa_modargs *ma = NULL;
    int ret = -1;
    struct userdata *u = NULL;
    const char *dev;
    pa_sample_spec ss;
    pa_channel_map map;
    uint32_t periods, fragsize;
    snd_pcm_uframes_t period_size;
    size_t frame_size;
    snd_pcm_info_t *pcm_info = NULL;
    int err;
    char *t;
    const char *name;
    char *name_buf = NULL;
    int namereg_fail;
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_ALSA) < 0) {
        pa_log(__FILE__": failed to parse sample specification and channel map");
        goto fail;
    }

    frame_size = pa_frame_size(&ss);
    
    /* Fix latency to 100ms */
    periods = 8;
    fragsize = pa_bytes_per_second(&ss)/128;

    if (pa_modargs_get_value_u32(ma, "fragments", &periods) < 0 || pa_modargs_get_value_u32(ma, "fragment_size", &fragsize) < 0) {
        pa_log(__FILE__": failed to parse buffer metrics");
        goto fail;
    }
    period_size = fragsize/frame_size;
    
    u = pa_xnew0(struct userdata, 1);
    m->userdata = u;
    u->module = m;
    
    snd_config_update_free_global();
    if ((err = snd_pcm_open(&u->pcm_handle, dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
        pa_log(__FILE__": Error opening PCM device %s: %s", dev, snd_strerror(err));
        goto fail;
    }

    if ((err = snd_pcm_info_malloc(&pcm_info)) < 0 ||
        (err = snd_pcm_info(u->pcm_handle, pcm_info)) < 0) {
        pa_log(__FILE__": Error fetching PCM info: %s", snd_strerror(err));
        goto fail;
    }

    if ((err = pa_alsa_set_hw_params(u->pcm_handle, &ss, &periods, &period_size)) < 0) {
        pa_log(__FILE__": Failed to set hardware parameters: %s", snd_strerror(err));
        goto fail;
    }

    if (ss.channels != map.channels)
        /* Seems ALSA didn't like the channel number, so let's fix the channel map */
        pa_channel_map_init_auto(&map, ss.channels, PA_CHANNEL_MAP_ALSA);
    
    if ((err = snd_mixer_open(&u->mixer_handle, 0)) < 0) {
        pa_log(__FILE__": Error opening mixer: %s", snd_strerror(err));
        goto fail;
    }

    if ((pa_alsa_prepare_mixer(u->mixer_handle, dev) < 0) ||
        !(u->mixer_elem = pa_alsa_find_elem(u->mixer_handle, "PCM", "Master"))) {
        snd_mixer_close(u->mixer_handle);
        u->mixer_handle = NULL;
    }

    if ((name = pa_modargs_get_value(ma, "sink_name", NULL)))
        namereg_fail = 1;
    else {
        name = name_buf = pa_sprintf_malloc("alsa_output.%s", dev);
        namereg_fail = 0;
    }

    if (!(u->sink = pa_sink_new(c, __FILE__, name, namereg_fail, &ss, &map))) {
        pa_log(__FILE__": Failed to create sink object");
        goto fail;
    }

    u->sink->is_hardware = 1;
    u->sink->get_latency = sink_get_latency_cb;
    if (u->mixer_handle) {
        assert(u->mixer_elem);
        if (snd_mixer_selem_has_playback_volume(u->mixer_elem)) {
            int i;

            for (i = 0;i < ss.channels;i++) {
                if (!snd_mixer_selem_has_playback_channel(u->mixer_elem, i))
                    break;
            }

            if (i == ss.channels) {
                u->sink->get_hw_volume = sink_get_hw_volume_cb;
                u->sink->set_hw_volume = sink_set_hw_volume_cb;
                snd_mixer_selem_get_playback_volume_range(
                    u->mixer_elem, &u->hw_volume_min, &u->hw_volume_max);
            }
        }
        if (snd_mixer_selem_has_playback_switch(u->mixer_elem)) {
            u->sink->get_hw_mute = sink_get_hw_mute_cb;
            u->sink->set_hw_mute = sink_set_hw_mute_cb;
        }
    }
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc("ALSA PCM on %s (%s)", dev, snd_pcm_info_get_name(pcm_info)));
    pa_xfree(t);

    u->pcm_fdl = pa_alsa_fdlist_new();
    assert(u->pcm_fdl);
    if (pa_alsa_fdlist_init_pcm(u->pcm_fdl, u->pcm_handle, c->mainloop, fdl_callback, u) < 0) {
        pa_log(__FILE__": failed to initialise file descriptor monitoring");
        goto fail;
    }

    if (u->mixer_handle) {
        u->mixer_fdl = pa_alsa_fdlist_new();
        assert(u->mixer_fdl);
        if (pa_alsa_fdlist_init_mixer(u->mixer_fdl, u->mixer_handle, c->mainloop) < 0) {
            pa_log(__FILE__": failed to initialise file descriptor monitoring");
            goto fail;
        }
        snd_mixer_elem_set_callback(u->mixer_elem, mixer_callback);
        snd_mixer_elem_set_callback_private(u->mixer_elem, u);
    } else
        u->mixer_fdl = NULL;
    
    u->frame_size = frame_size;
    u->fragment_size = period_size * frame_size;

    pa_log_info(__FILE__": using %u fragments of size %lu bytes.", periods, (long unsigned)u->fragment_size);

    u->silence.memblock = pa_memblock_new(u->silence.length = u->fragment_size, c->memblock_stat);
    assert(u->silence.memblock);
    pa_silence_memblock(u->silence.memblock, &ss);
    u->silence.index = 0;

    u->memchunk.memblock = NULL;
    u->memchunk.index = u->memchunk.length = 0;
    
    ret = 0;

    /* Get initial mixer settings */
    if (u->sink->get_hw_volume)
        u->sink->get_hw_volume(u->sink);
    if (u->sink->get_hw_mute)
        u->sink->get_hw_mute(u->sink);
     
finish:

    pa_xfree(name_buf);
    
     if (ma)
         pa_modargs_free(ma);

    if (pcm_info)
        snd_pcm_info_free(pcm_info);
    
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

    clear_up(u);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
    if (u->silence.memblock)
        pa_memblock_unref(u->silence.memblock);
    
    pa_xfree(u);
}

