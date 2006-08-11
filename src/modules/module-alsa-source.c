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

#include <pulsecore/core-error.h>
#include <pulsecore/core.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/log.h>

#include "alsa-util.h"
#include "module-alsa-source-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("ALSA Source")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "source_name=<name for the source> "
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
    pa_source *source;
    struct pa_alsa_fdlist *pcm_fdl;
    struct pa_alsa_fdlist *mixer_fdl;
    long hw_volume_max, hw_volume_min;

    size_t frame_size, fragment_size;
    pa_memchunk memchunk;
    pa_module *module;
};

static const char* const valid_modargs[] = {
    "device",
    "source_name",
    "channels",
    "rate",
    "format",
    "fragments",
    "fragment_size",
    "channel_map",
    NULL
};

#define DEFAULT_SOURCE_NAME "alsa_input"
#define DEFAULT_DEVICE "default"

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->source ? pa_idxset_size(u->source->outputs) : 0));
}

static void clear_up(struct userdata *u) {
    assert(u);
    
    if (u->source) {
        pa_source_disconnect(u->source);
        pa_source_unref(u->source);
        u->source = NULL;
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

    pa_log_info(__FILE__": *** ALSA-XRUN (capture) ***");
    
    if ((ret = snd_pcm_prepare(u->pcm_handle)) < 0) {
        pa_log(__FILE__": snd_pcm_prepare() failed: %s", snd_strerror(-ret));

        clear_up(u);
        pa_module_unload_request(u->module);

        return -1;
    }

    return 0;
}

static void do_read(struct userdata *u) {
    assert(u);

    update_usage(u);
    
    for (;;) {
        pa_memchunk post_memchunk;
        snd_pcm_sframes_t frames;
        size_t l;
        
        if (!u->memchunk.memblock) {
            u->memchunk.memblock = pa_memblock_new(u->memchunk.length = u->fragment_size, u->source->core->memblock_stat);
            u->memchunk.index = 0;
        }
            
        assert(u->memchunk.memblock);
        assert(u->memchunk.length);
        assert(u->memchunk.memblock->data);
        assert(u->memchunk.memblock->length);
        assert(u->memchunk.length % u->frame_size == 0);

        if ((frames = snd_pcm_readi(u->pcm_handle, (uint8_t*) u->memchunk.memblock->data + u->memchunk.index, u->memchunk.length / u->frame_size)) < 0) {
            if (frames == -EAGAIN)
                return;
            
            if (frames == -EPIPE) {
                if (xrun_recovery(u) < 0)
                    return;
                
                continue;
            }

            pa_log(__FILE__": snd_pcm_readi() failed: %s", snd_strerror(-frames));

            clear_up(u);
            pa_module_unload_request(u->module);
            return;
        }

        l = frames * u->frame_size;
        
        post_memchunk = u->memchunk;
        post_memchunk.length = l;

        pa_source_post(u->source, &post_memchunk);

        u->memchunk.index += l;
        u->memchunk.length -= l;
        
        if (u->memchunk.length == 0) {
            pa_memblock_unref(u->memchunk.memblock);
            u->memchunk.memblock = NULL;
            u->memchunk.index = u->memchunk.length = 0;
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

    do_read(u);
}

static int mixer_callback(snd_mixer_elem_t *elem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(elem);

    assert(u && u->mixer_handle);

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    if (mask & SND_CTL_EVENT_MASK_VALUE) {
        if (u->source->get_hw_volume)
            u->source->get_hw_volume(u->source);
        if (u->source->get_hw_mute)
            u->source->get_hw_mute(u->source);
        
        pa_subscription_post(u->source->core,
            PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE,
            u->source->index);
    }

    return 0;
}

static pa_usec_t source_get_latency_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    snd_pcm_sframes_t frames;
    assert(s && u && u->source);

    if (snd_pcm_delay(u->pcm_handle, &frames) < 0) {
        pa_log(__FILE__": failed to get delay");
        s->get_latency = NULL;
        return 0;
    }

    return pa_bytes_to_usec(frames * u->frame_size, &s->sample_spec);
}

static int source_get_hw_volume_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    long vol;
    int err;
    int i;

    assert(u && u->mixer_elem);

    for (i = 0;i < s->hw_volume.channels;i++) {
        long set_vol;
        
        assert(snd_mixer_selem_has_capture_channel(u->mixer_elem, i));
        
        if ((err = snd_mixer_selem_get_capture_volume(u->mixer_elem, i, &vol)) < 0)
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

static int source_set_hw_volume_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    int err;
    pa_volume_t vol;
    int i;

    assert(u && u->mixer_elem);

    for (i = 0;i < s->hw_volume.channels;i++) {
        assert(snd_mixer_selem_has_capture_channel(u->mixer_elem, i));

        vol = s->hw_volume.values[i];

        if (vol > PA_VOLUME_NORM)
            vol = PA_VOLUME_NORM;

        vol = (long) roundf(((float) vol * (u->hw_volume_max - u->hw_volume_min)) / PA_VOLUME_NORM) + u->hw_volume_min;

        if ((err = snd_mixer_selem_set_capture_volume(u->mixer_elem, i, vol)) < 0)
            goto fail;
    }

    return 0;

fail:
    pa_log_error(__FILE__": Unable to set volume: %s", snd_strerror(err));
    s->get_hw_volume = NULL;
    s->set_hw_volume = NULL;
    return -1;
}

static int source_get_hw_mute_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    int err, sw;

    assert(u && u->mixer_elem);

    err = snd_mixer_selem_get_capture_switch(u->mixer_elem, 0, &sw);
    if (err) {
        pa_log_error(__FILE__": Unable to get switch: %s", snd_strerror(err));
        s->get_hw_mute = NULL;
        s->set_hw_mute = NULL;
        return -1;
    }

    s->hw_muted = !sw;

    return 0;
}

static int source_set_hw_mute_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    int err;

    assert(u && u->mixer_elem);

    err = snd_mixer_selem_set_capture_switch_all(u->mixer_elem, !s->hw_muted);
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
    unsigned periods, fragsize;
    snd_pcm_uframes_t period_size;
    size_t frame_size;
    snd_pcm_info_t *pcm_info = NULL;
    int err;
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_ALSA) < 0) {
        pa_log(__FILE__": failed to parse sample specification");
        goto fail;
    }

    frame_size = pa_frame_size(&ss);

    /* Fix latency to 100ms */
    periods = 12;
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
    if ((err = snd_pcm_open(&u->pcm_handle, dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
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
        !(u->mixer_elem = pa_alsa_find_elem(u->mixer_handle, "Capture", "Mic"))) {
        snd_mixer_close(u->mixer_handle);
        u->mixer_handle = NULL;
    }

    if (!(u->source = pa_source_new(c, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss, &map))) {
        pa_log(__FILE__": Failed to create source object");
        goto fail;
    }

    u->source->is_hardware = 1;
    u->source->userdata = u;
    u->source->get_latency = source_get_latency_cb;
    if (u->mixer_handle) {
        assert(u->mixer_elem);
        if (snd_mixer_selem_has_capture_volume(u->mixer_elem)) {
            int i;

            for (i = 0;i < ss.channels;i++) {
                if (!snd_mixer_selem_has_capture_channel(u->mixer_elem, i))
                    break;
            }

            if (i == ss.channels) {
                u->source->get_hw_volume = source_get_hw_volume_cb;
                u->source->set_hw_volume = source_set_hw_volume_cb;
                snd_mixer_selem_get_capture_volume_range(
                    u->mixer_elem, &u->hw_volume_min, &u->hw_volume_max);
            }
        }
        if (snd_mixer_selem_has_capture_switch(u->mixer_elem)) {
            u->source->get_hw_mute = source_get_hw_mute_cb;
            u->source->set_hw_mute = source_set_hw_mute_cb;
        }
    }
    pa_source_set_owner(u->source, m);
    u->source->description = pa_sprintf_malloc("Advanced Linux Sound Architecture PCM on '%s' (%s)", dev, snd_pcm_info_get_name(pcm_info));

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

    pa_log_info(__FILE__": using %u fragments of size %lu bytes.", periods, (long unsigned) u->fragment_size);

    u->memchunk.memblock = NULL;
    u->memchunk.index = u->memchunk.length = 0;

    snd_pcm_start(u->pcm_handle);
    
    ret = 0;

    /* Get initial mixer settings */
    if (u->source->get_hw_volume)
        u->source->get_hw_volume(u->source);
    if (u->source->get_hw_mute)
        u->source->get_hw_mute(u->source);

finish:
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
    
    pa_xfree(u);
}

