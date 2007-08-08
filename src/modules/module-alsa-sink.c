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

#include <stdio.h>

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#else
#include "poll.h"
#endif

#include <asoundlib.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/core.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread.h>
#include <pulsecore/core-error.h>

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
        "channel_map=<channel map> "
        "mmap=<enable memory mapping?>")

#define DEFAULT_DEVICE "default"

#define DEFAULT_NFRAGS 4
#define DEFAULT_FRAGSIZE_MSEC 25

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_thread *thread;
    pa_asyncmsgq *asyncmsgq;
    
    snd_pcm_t *pcm_handle;

    pa_alsa_fdlist *mixer_fdl;
    snd_mixer_t *mixer_handle;
    snd_mixer_elem_t *mixer_elem;
    long hw_volume_max, hw_volume_min;

    size_t frame_size, fragment_size, hwbuf_size;
    unsigned nfragments;
    pa_memchunk memchunk;

    char *device_name;

    int use_mmap;

    int first;

    struct pollfd *pollfd;
    int n_alsa_fds;
};

enum {
    POLLFD_ASYNCQ,
    POLLFD_ALSA_BASE
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
    "mmap",
    NULL
};

static int mmap_write(struct userdata *u) {
    snd_pcm_sframes_t n;
    int err;
    const snd_pcm_channel_area_t *areas;
    snd_pcm_uframes_t offset, frames;
    int work_done = 0;
    
    pa_assert(u);
    pa_assert(u->sink);

    for (;;) {
        pa_memchunk chunk;
        void *p;
        
        if ((n = snd_pcm_avail_update(u->pcm_handle)) < 0) {

            if (n == -EPIPE) {
                pa_log_debug("snd_pcm_avail_update: Buffer underrun!");
                u->first = 1;
            }
            
            if ((err = snd_pcm_recover(u->pcm_handle, n, 1)) == 0)
                continue;

            if (err == -EAGAIN)
                return work_done;
            
            pa_log("snd_pcm_avail_update: %s", snd_strerror(n));
            return -1;
        }

/*         pa_log("Got request for %i samples", (int) n); */
        
        if (n <= 0)
            return work_done;

        frames = n;
        
        if ((err = snd_pcm_mmap_begin(u->pcm_handle, &areas, &offset, &frames)) < 0) {
            
            if (err == -EPIPE) {
                pa_log_debug("snd_pcm_mmap_begin: Buffer underrun!");
                u->first = 1;
            }

            if ((err = snd_pcm_recover(u->pcm_handle, err, 1)) == 0)
                continue;

            if (err == -EAGAIN)
                return work_done;

            pa_log("Failed to write data to DSP: %s", snd_strerror(err));
            return -1;
        }

        /* Check these are multiples of 8 bit */
        pa_assert((areas[0].first & 7) == 0);
        pa_assert((areas[0].step & 7)== 0);

        /* We assume a single interleaved memory buffer */
        pa_assert((areas[0].first >> 3) == 0);
        pa_assert((areas[0].step >> 3) == u->frame_size);

        p = (uint8_t*) areas[0].addr + (offset * u->frame_size);
        
        chunk.memblock = pa_memblock_new_fixed(u->core->mempool, p, frames * u->frame_size, 1);
        chunk.length = pa_memblock_get_length(chunk.memblock);
        chunk.index = 0;

        pa_sink_render_into_full(u->sink, &chunk);

        /* FIXME: Maybe we can do something to keep this memory block
         * a little bit longer around? */
        pa_memblock_unref_fixed(chunk.memblock);

        if ((err = snd_pcm_mmap_commit(u->pcm_handle, offset, frames)) < 0) {

            if (err == -EPIPE) {
                pa_log_debug("snd_pcm_mmap_commit: Buffer underrun!");
                u->first = 1;
            }
            
            if ((err = snd_pcm_recover(u->pcm_handle, err, 1)) == 0)
                continue;
            
            if (err == -EAGAIN)
                return work_done;
            
            pa_log("Failed to write data to DSP: %s", snd_strerror(err));
            return -1;
        }

        work_done = 1;

/*         pa_log("wrote %i samples", (int) frames); */
    }
}

static pa_usec_t sink_get_latency(struct userdata *u) {
    pa_usec_t r = 0;
    snd_pcm_sframes_t frames = 0;
    int err;
    
    pa_assert(u);

    snd_pcm_avail_update(u->pcm_handle);

    if ((err = snd_pcm_delay(u->pcm_handle, &frames)) < 0) {
        pa_log("Failed to get delay: %s", snd_strerror(err));
        return 0;
    }

    if (frames > 0)
        r = pa_bytes_to_usec(frames * u->frame_size, &u->sink->sample_spec);
    
    if (u->memchunk.memblock)
        r += pa_bytes_to_usec(u->memchunk.length, &u->sink->sample_spec);

    return r;
}

static int build_pollfd(struct userdata *u) {
    int err;
    
    pa_assert(u);
    pa_assert(u->pcm_handle);

    if ((u->n_alsa_fds = snd_pcm_poll_descriptors_count(u->pcm_handle)) < 0) {
        pa_log("snd_pcm_poll_descriptors_count() failed: %s", snd_strerror(u->n_alsa_fds));
        return -1;
    }

    pa_xfree(u->pollfd);
    u->pollfd = pa_xnew0(struct pollfd, POLLFD_ALSA_BASE + u->n_alsa_fds);

    u->pollfd[POLLFD_ASYNCQ].fd = pa_asyncmsgq_get_fd(u->asyncmsgq);
    u->pollfd[POLLFD_ASYNCQ].events = POLLIN;

    if ((err = snd_pcm_poll_descriptors(u->pcm_handle, u->pollfd+POLLFD_ALSA_BASE, u->n_alsa_fds)) < 0) {
        pa_log("snd_pcm_poll_descriptors() failed: %s", snd_strerror(err));
        return -1;
    }
    
    return 0;
}

static int suspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->pcm_handle);

    snd_pcm_drain(u->pcm_handle);    /* Let's suspend */
    snd_pcm_close(u->pcm_handle);
    u->pcm_handle = NULL;

    pa_log_debug("Device suspended...");
    
    return 0;
}

static int unsuspend(struct userdata *u) {
    pa_sample_spec ss;
    int err, b;
    unsigned nfrags;
    snd_pcm_uframes_t period_size;

    pa_assert(u);
    pa_assert(!u->pcm_handle);

    pa_log_debug("Trying resume...");

    snd_config_update_free_global();
    if ((err = snd_pcm_open(&u->pcm_handle, u->device_name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
        pa_log("Error opening PCM device %s: %s", u->device_name, snd_strerror(err));
        goto fail;
    }

    ss = u->sink->sample_spec;
    nfrags = u->nfragments;
    period_size = u->fragment_size / u->frame_size;
    b = u->use_mmap;
    
    if ((err = pa_alsa_set_hw_params(u->pcm_handle, &ss, &nfrags, &period_size, &b)) < 0) {
        pa_log("Failed to set hardware parameters: %s", snd_strerror(err));
        goto fail;
    }

    if (b != u->use_mmap) {
        pa_log_warn("Resume failed, couldn't get original access mode.");
        goto fail;
    }

    if (!pa_sample_spec_equal(&ss, &u->sink->sample_spec)) {
        pa_log_warn("Resume failed, couldn't restore original sample settings.");
        goto fail;
    }

    if (nfrags != u->nfragments || period_size*u->frame_size != u->fragment_size) {
        pa_log_warn("Resume failed, couldn't restore original fragment settings.");
        goto fail;
    }

    if ((err = pa_alsa_set_sw_params(u->pcm_handle)) < 0) {
        pa_log("Failed to set software parameters: %s", snd_strerror(err));
        goto fail;
    }

    if (build_pollfd(u) < 0)
        goto fail;
    
    /* FIXME: We need to reload the volume somehow */
    
    u->first = 1;
                
    pa_log_debug("Resumed successfully...");

    return 0;

fail:
    snd_pcm_close(u->pcm_handle);
    u->pcm_handle = NULL;

    return -1;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (u->pcm_handle)
                r = sink_get_latency(u);

            *((pa_usec_t*) data) = r;

            break;
        }

        case PA_SINK_MESSAGE_SET_STATE:

            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {
                
                case PA_SINK_SUSPENDED:
                    pa_assert(PA_SINK_OPENED(u->sink->thread_info.state));

                    if (suspend(u) < 0)
                        return -1;
                    
                    break;

                case PA_SINK_IDLE:
                case PA_SINK_RUNNING:

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                        if (unsuspend(u) < 0)
                            return -1;
                    }
                    
                    break;

                case PA_SINK_DISCONNECTED:
                    ;
            }
            
            break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int mixer_callback(snd_mixer_elem_t *elem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(elem);

    pa_assert(u);
    pa_assert(u->mixer_handle);

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    if (mask & SND_CTL_EVENT_MASK_VALUE) {
        pa_sink_get_volume(u->sink);
        pa_sink_get_mute(u->sink);
    }

    return 0;
}

static int sink_get_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;
    int i;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    for (i = 0; i < s->sample_spec.channels; i++) {
        long set_vol, vol;

        pa_assert(snd_mixer_selem_has_playback_channel(u->mixer_elem, i));

        if ((err = snd_mixer_selem_get_playback_volume(u->mixer_elem, i, &vol)) < 0)
            goto fail;

        set_vol = (long) roundf(((float) s->volume.values[i] * (u->hw_volume_max - u->hw_volume_min)) / PA_VOLUME_NORM) + u->hw_volume_min;

        /* Try to avoid superfluous volume changes */
        if (set_vol != vol)
            s->volume.values[i] = (pa_volume_t) roundf(((float) (vol - u->hw_volume_min) * PA_VOLUME_NORM) / (u->hw_volume_max - u->hw_volume_min));
    }

    return 0;

fail:
    pa_log_error("Unable to read volume: %s", snd_strerror(err));
    
    s->get_volume = NULL;
    s->set_volume = NULL;
    return -1;
}

static int sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;
    int i;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    for (i = 0; i < s->sample_spec.channels; i++) {
        long alsa_vol;
        pa_volume_t vol;

        pa_assert(snd_mixer_selem_has_playback_channel(u->mixer_elem, i));

        vol = s->volume.values[i];

        if (vol > PA_VOLUME_NORM)
            vol = PA_VOLUME_NORM;

        alsa_vol = (long) roundf(((float) vol * (u->hw_volume_max - u->hw_volume_min)) / PA_VOLUME_NORM) + u->hw_volume_min;

        if ((err = snd_mixer_selem_set_playback_volume(u->mixer_elem, i, alsa_vol)) < 0)
            goto fail;
    }

    return 0;

fail:
    pa_log_error("Unable to set volume: %s", snd_strerror(err));
    
    s->get_volume = NULL;
    s->set_volume = NULL;
    return -1;
}

static int sink_get_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err, sw;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    if ((err = snd_mixer_selem_get_playback_switch(u->mixer_elem, 0, &sw)) < 0) {
        pa_log_error("Unable to get switch: %s", snd_strerror(err));

        s->get_mute = NULL;
        s->set_mute = NULL;
        return -1;
    }

    s->muted = !sw;

    return 0;
}

static int sink_set_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    if ((err = snd_mixer_selem_set_playback_switch_all(u->mixer_elem, !s->muted)) < 0) {
        pa_log_error("Unable to set switch: %s", snd_strerror(err));
        
        s->get_mute = NULL;
        s->set_mute = NULL;
        return -1;
    }

    return 0;
}

static void thread_func(void *userdata) {

    struct userdata *u = userdata;
    unsigned short revents = 0;
    snd_pcm_status_t *status;
    int err;
    
    pa_assert(u);
    snd_pcm_status_alloca(&status);

    pa_log_debug("Thread starting up");

    if (build_pollfd(u) < 0)
        goto fail;

    for (;;) {
        pa_msgobject *object;
        int code;
        void *data;
        pa_memchunk chunk;
        int64_t offset;
        int r;

/*         pa_log("loop");      */
        
        /* Check whether there is a message for us to process */
        if (pa_asyncmsgq_get(u->asyncmsgq, &object, &code, &data, &offset, &chunk, 0) == 0) {
            int ret;

/*             pa_log("processing msg");  */

            if (!object && code == PA_MESSAGE_SHUTDOWN) {
                pa_asyncmsgq_done(u->asyncmsgq, 0);
                goto finish;
            }

            ret = pa_asyncmsgq_dispatch(object, code, data, offset, &chunk);
            pa_asyncmsgq_done(u->asyncmsgq, ret);
            continue;
        } 

/*         pa_log("loop2"); */

        /* Render some data and write it to the dsp */

        if (PA_SINK_OPENED(u->sink->thread_info.state) && ((revents & POLLOUT) || u->first == 1)) {
            int work_done = 0;
            pa_assert(u->pcm_handle);

            if (u->use_mmap) {

                if ((work_done = mmap_write(u)) < 0)
                    goto fail;

            } else {
                
                for (;;) {
                    void *p;
                    snd_pcm_sframes_t t;
                    ssize_t l;
                
                    if ((err = snd_pcm_status(u->pcm_handle, status)) < 0) {
                        pa_log("Failed to query DSP status data: %s", snd_strerror(t));
                        goto fail;
                    }

                    if (snd_pcm_status_get_avail_max(status)*u->frame_size >= u->hwbuf_size)
                        pa_log_debug("Buffer underrun!");
                    
                    l = snd_pcm_status_get_avail(status) * u->frame_size;

/*                     pa_log("%u bytes to write", l); */
                    
                    if (l <= 0)
                        break;
                    
                    if (u->memchunk.length <= 0)
                        pa_sink_render(u->sink, l, &u->memchunk);
                    
                    pa_assert(u->memchunk.length > 0);
                    
                    p = pa_memblock_acquire(u->memchunk.memblock);
                    t = snd_pcm_writei(u->pcm_handle, (const uint8_t*) p + u->memchunk.index, u->memchunk.length / u->frame_size);
                    pa_memblock_release(u->memchunk.memblock);
                    
/*                     pa_log("wrote %i bytes of %u (%u)", t*u->frame_size, u->memchunk.length, l);   */
                    
                    pa_assert(t != 0);
                    
                    if (t < 0) {

                        if ((t = snd_pcm_recover(u->pcm_handle, t, 1)) == 0)
                            continue;
                        
                        if (t == -EAGAIN) {
                            pa_log_debug("EAGAIN");
                            break;
                        } else {
                            pa_log("Failed to write data to DSP: %s", snd_strerror(t));
                            goto fail;
                        }
                    }
                        
                    u->memchunk.index += t * u->frame_size;
                    u->memchunk.length -= t * u->frame_size;
                    
                    if (u->memchunk.length <= 0) {
                        pa_memblock_unref(u->memchunk.memblock);
                        pa_memchunk_reset(&u->memchunk);
                    }

                    work_done = 1;

                    if (t * u->frame_size >= (unsigned) l)
                        break;
                } 
            }

            revents &= ~POLLOUT;
            
            if (work_done) {

                if (u->first) {
                    pa_log_info("Starting playback.");
                    snd_pcm_start(u->pcm_handle);
                    u->first = 0;
                }
            
                continue;
            }
        }

        /* Hmm, nothing to do. Let's sleep */
        if (pa_asyncmsgq_before_poll(u->asyncmsgq) < 0)
            continue;

/*         pa_log("polling for %i", POLLFD_ALSA_BASE + (PA_SINK_OPENED(u->sink->thread_info.state) ? u->n_alsa_fds : 0));    */
        r = poll(u->pollfd, POLLFD_ALSA_BASE + (PA_SINK_OPENED(u->sink->thread_info.state) ? u->n_alsa_fds : 0), -1);
/*         pa_log("poll end"); */

        pa_asyncmsgq_after_poll(u->asyncmsgq);

        if (r < 0) {
            if (errno == EINTR) {
                u->pollfd[POLLFD_ASYNCQ].revents = 0;
                revents = 0;
                continue;
            }

            pa_log("poll() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        pa_assert(r > 0);

        if (PA_SINK_OPENED(u->sink->thread_info.state)) {
            if ((err = snd_pcm_poll_descriptors_revents(u->pcm_handle, u->pollfd + POLLFD_ALSA_BASE, u->n_alsa_fds, &revents)) < 0) {
                pa_log("snd_pcm_poll_descriptors_revents() failed: %s", snd_strerror(err));
                goto fail;
            }

            if (revents & (POLLERR|POLLNVAL|POLLHUP)) {
                if (revents & POLLERR)
                    pa_log_warn("Got POLLERR from ALSA");
                if (revents & POLLNVAL)
                    pa_log_warn("Got POLLNVAL from ALSA"); 
                if (revents & POLLHUP)
                    pa_log_warn("Got POLLHUP from ALSA");

                goto fail;
            }
/*             pa_log("got alsa event"); */
        } else
            revents = 0;
        
        pa_assert((u->pollfd[POLLFD_ASYNCQ].revents & ~POLLIN) == 0);
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->core->asyncmsgq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->asyncmsgq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_core *c, pa_module*m) {
    
    pa_modargs *ma = NULL;
    int ret = -1;
    struct userdata *u = NULL;
    const char *dev;
    pa_sample_spec ss;
    pa_channel_map map;
    uint32_t nfrags, frag_size;
    snd_pcm_uframes_t period_size;
    size_t frame_size;
    snd_pcm_info_t *pcm_info = NULL;
    int err;
    char *t;
    const char *name;
    char *name_buf = NULL;
    int namereg_fail;
    int use_mmap = 1, b;

    pa_assert(c);
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_ALSA) < 0) {
        pa_log("Failed to parse sample specification and channel map");
        goto fail;
    }

    frame_size = pa_frame_size(&ss);

    nfrags = DEFAULT_NFRAGS;
    frag_size = pa_usec_to_bytes(DEFAULT_FRAGSIZE_MSEC*1000, &ss);
    if (frag_size <= 0)
        frag_size = frame_size;

    if (pa_modargs_get_value_u32(ma, "fragments", &nfrags) < 0 || pa_modargs_get_value_u32(ma, "fragment_size", &frag_size) < 0) {
        pa_log("Failed to parse buffer metrics");
        goto fail;
    }
    period_size = frag_size/frame_size;

    if (pa_modargs_get_value_boolean(ma, "mmap", &use_mmap) < 0) {
        pa_log("Failed to parse mmap argument.");
        goto fail;
    }
        
    u = pa_xnew0(struct userdata, 1);
    u->core = c;
    u->module = m;
    m->userdata = u;
    u->use_mmap = use_mmap;
    u->first = 1;
    u->n_alsa_fds = 0;
    u->pollfd = NULL;
    pa_assert_se(u->asyncmsgq = pa_asyncmsgq_new(0));

    snd_config_update_free_global();
    if ((err = snd_pcm_open(&u->pcm_handle, dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
        pa_log("Error opening PCM device %s: %s", dev, snd_strerror(err));
        goto fail;
    }

    u->device_name = pa_xstrdup(dev);

    if ((err = snd_pcm_info_malloc(&pcm_info)) < 0 ||
        (err = snd_pcm_info(u->pcm_handle, pcm_info)) < 0) {
        pa_log("Error fetching PCM info: %s", snd_strerror(err));
        goto fail;
    }

    b = use_mmap;
    if ((err = pa_alsa_set_hw_params(u->pcm_handle, &ss, &nfrags, &period_size, &b)) < 0) {
        pa_log("Failed to set hardware parameters: %s", snd_strerror(err));
        goto fail;
    }

    if (use_mmap && !b) {
        pa_log_info("Device doesn't support mmap(), falling back to UNIX read/write mode.");
        u->use_mmap = use_mmap = b;
    }

    if (u->use_mmap)
        pa_log_info("Successfully enabled mmap() mode.");

    if ((err = pa_alsa_set_sw_params(u->pcm_handle)) < 0) {
        pa_log("Failed to set software parameters: %s", snd_strerror(err));
        goto fail;
    }

    /* ALSA might tweak the sample spec, so recalculate the frame size */
    frame_size = pa_frame_size(&ss);

    if (ss.channels != map.channels)
        /* Seems ALSA didn't like the channel number, so let's fix the channel map */
        pa_channel_map_init_auto(&map, ss.channels, PA_CHANNEL_MAP_ALSA);

    if ((err = snd_mixer_open(&u->mixer_handle, 0)) < 0)
        pa_log_warn("Error opening mixer: %s", snd_strerror(err));
    else {

        if ((pa_alsa_prepare_mixer(u->mixer_handle, dev) < 0) ||
            !(u->mixer_elem = pa_alsa_find_elem(u->mixer_handle, "Master", "PCM"))) {
            
            snd_mixer_close(u->mixer_handle);
            u->mixer_handle = NULL;
        }
    }

    if ((name = pa_modargs_get_value(ma, "sink_name", NULL)))
        namereg_fail = 1;
    else {
        name = name_buf = pa_sprintf_malloc("alsa_output.%s", dev);
        namereg_fail = 0;
    }

    u->sink = pa_sink_new(c, __FILE__, name, namereg_fail, &ss, &map);
    pa_xfree(name_buf);
    
    if (!u->sink) {
        pa_log("Failed to create sink object");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;

    pa_sink_set_module(u->sink, m);
    pa_sink_set_asyncmsgq(u->sink, u->asyncmsgq);
    pa_sink_set_description(u->sink, t = pa_sprintf_malloc(
                                    "ALSA PCM on %s (%s)",
                                    dev,
                                    snd_pcm_info_get_name(pcm_info)));
    pa_xfree(t);
    
    u->sink->is_hardware = 1;

    u->frame_size = frame_size;
    u->fragment_size = frag_size = period_size * frame_size;
    u->nfragments = nfrags;
    u->hwbuf_size = u->fragment_size * nfrags;

    pa_log_info("Using %u fragments of size %lu bytes.", nfrags, (long unsigned) u->fragment_size);

    pa_memchunk_reset(&u->memchunk);

    if (u->mixer_handle) {
        /* Initialize mixer code */
        
        pa_assert(u->mixer_elem);
        
        if (snd_mixer_selem_has_playback_volume(u->mixer_elem)) {
            int i;

            for (i = 0;i < ss.channels; i++) {
                if (!snd_mixer_selem_has_playback_channel(u->mixer_elem, i))
                    break;
            }

            if (i == ss.channels) {
                u->sink->get_volume = sink_get_volume_cb;
                u->sink->set_volume = sink_set_volume_cb;
                snd_mixer_selem_get_playback_volume_range(u->mixer_elem, &u->hw_volume_min, &u->hw_volume_max);
            }
        }
        
        if (snd_mixer_selem_has_playback_switch(u->mixer_elem)) {
            u->sink->get_mute = sink_get_mute_cb;
            u->sink->set_mute = sink_set_mute_cb;
        }

        u->mixer_fdl = pa_alsa_fdlist_new();

        if (pa_alsa_fdlist_set_mixer(u->mixer_fdl, u->mixer_handle, c->mainloop) < 0) {
            pa_log("failed to initialise file descriptor monitoring");
            goto fail;
        }

        snd_mixer_elem_set_callback(u->mixer_elem, mixer_callback);
        snd_mixer_elem_set_callback_private(u->mixer_elem, u);
    } else
        u->mixer_fdl = NULL;

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }
    
    /* Get initial mixer settings */
    if (u->sink->get_volume)
        u->sink->get_volume(u->sink);
    if (u->sink->get_mute)
        u->sink->get_mute(u->sink);

    ret = 0;

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
    
    pa_assert(c);
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_disconnect(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->asyncmsgq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    if (u->asyncmsgq)
        pa_asyncmsgq_free(u->asyncmsgq);

    if (u->sink)
        pa_sink_unref(u->sink);
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->mixer_fdl)
        pa_alsa_fdlist_free(u->mixer_fdl);
    
    if (u->mixer_handle)
        snd_mixer_close(u->mixer_handle);

    if (u->pcm_handle) {
        snd_pcm_drop(u->pcm_handle);
        snd_pcm_close(u->pcm_handle);
    }

    pa_xfree(u->pollfd);
    pa_xfree(u->device_name);
    pa_xfree(u);
    
    snd_config_update_free_global();
}

