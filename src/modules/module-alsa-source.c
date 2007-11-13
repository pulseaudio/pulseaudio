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

#include <asoundlib.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/core-error.h>
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
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>

#include "alsa-util.h"
#include "module-alsa-source-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("ALSA Source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "source_name=<name for the source> "
        "device=<ALSA device> "
        "device_id=<ALSA device id> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "channel_map=<channel map> "
        "mmap=<enable memory mapping?>");

#define DEFAULT_DEVICE "default"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    snd_pcm_t *pcm_handle;

    pa_alsa_fdlist *mixer_fdl;
    snd_mixer_t *mixer_handle;
    snd_mixer_elem_t *mixer_elem;
    long hw_volume_max, hw_volume_min;

    size_t frame_size, fragment_size, hwbuf_size;
    unsigned nfragments;

    char *device_name;

    pa_bool_t use_mmap;

    pa_rtpoll_item *alsa_rtpoll_item;

    snd_mixer_selem_channel_id_t mixer_map[SND_MIXER_SCHN_LAST];
};

static const char* const valid_modargs[] = {
    "device",
    "device_id",
    "source_name",
    "channels",
    "rate",
    "format",
    "fragments",
    "fragment_size",
    "channel_map",
    "mmap",
    NULL
};

static int mmap_read(struct userdata *u) {
    int work_done = 0;

    pa_assert(u);
    pa_source_assert_ref(u->source);

    for (;;) {
        snd_pcm_sframes_t n;
        int err;
        const snd_pcm_channel_area_t *areas;
        snd_pcm_uframes_t offset, frames;
        pa_memchunk chunk;
        void *p;

        if ((n = snd_pcm_avail_update(u->pcm_handle)) < 0) {

            if (n == -EPIPE)
                pa_log_debug("snd_pcm_avail_update: Buffer underrun!");

            if ((err = snd_pcm_recover(u->pcm_handle, n, 1)) == 0)
                continue;

            if (err == -EAGAIN)
                return work_done;

            pa_log("snd_pcm_avail_update: %s", snd_strerror(err));
            return -1;
        }

/*         pa_log("Got request for %i samples", (int) n); */

        if (n <= 0)
            return work_done;

        frames = n;

        if ((err = snd_pcm_mmap_begin(u->pcm_handle, &areas, &offset, &frames)) < 0) {

            if (err == -EPIPE)
                pa_log_debug("snd_pcm_mmap_begin: Buffer underrun!");

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

        pa_source_post(u->source, &chunk);

        /* FIXME: Maybe we can do something to keep this memory block
         * a little bit longer around? */
        pa_memblock_unref_fixed(chunk.memblock);

        if ((err = snd_pcm_mmap_commit(u->pcm_handle, offset, frames)) < 0) {

            if (err == -EPIPE)
                pa_log_debug("snd_pcm_mmap_commit: Buffer underrun!");

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

static int unix_read(struct userdata *u) {
    snd_pcm_status_t *status;
    int work_done = 0;

    snd_pcm_status_alloca(&status);

    pa_assert(u);
    pa_source_assert_ref(u->source);

    for (;;) {
        void *p;
        snd_pcm_sframes_t t, k;
        ssize_t l;
        int err;
        pa_memchunk chunk;

        if ((err = snd_pcm_status(u->pcm_handle, status)) < 0) {
            pa_log("Failed to query DSP status data: %s", snd_strerror(err));
            return -1;
        }

        if (snd_pcm_status_get_avail_max(status)*u->frame_size >= u->hwbuf_size)
            pa_log_debug("Buffer overrun!");

        l = snd_pcm_status_get_avail(status) * u->frame_size;

        if (l <= 0)
            return work_done;

        chunk.memblock = pa_memblock_new(u->core->mempool, (size_t) -1);

        k = pa_memblock_get_length(chunk.memblock);

        if (k > l)
            k = l;

        k = (k/u->frame_size)*u->frame_size;

        p = pa_memblock_acquire(chunk.memblock);
        t = snd_pcm_readi(u->pcm_handle, (uint8_t*) p, k / u->frame_size);
        pa_memblock_release(chunk.memblock);

/*                     pa_log("wrote %i bytes of %u (%u)", t*u->frame_size, u->memchunk.length, l);   */

        pa_assert(t != 0);

        if (t < 0) {
            pa_memblock_unref(chunk.memblock);

            if ((t = snd_pcm_recover(u->pcm_handle, t, 1)) == 0)
                continue;

            if (t == -EAGAIN) {
                pa_log_debug("EAGAIN");
                return work_done;
            } else {
                pa_log("Failed to read data from DSP: %s", snd_strerror(t));
                return -1;
            }
        }

        chunk.index = 0;
        chunk.length = t * u->frame_size;

        pa_source_post(u->source, &chunk);
        pa_memblock_unref(chunk.memblock);

        work_done = 1;

        if (t * u->frame_size >= (unsigned) l)
            return work_done;
    }
}

static pa_usec_t source_get_latency(struct userdata *u) {
    pa_usec_t r = 0;
    snd_pcm_status_t *status;
    snd_pcm_sframes_t frames = 0;
    int err;

    snd_pcm_status_alloca(&status);

    pa_assert(u);
    pa_assert(u->pcm_handle);

    if ((err = snd_pcm_status(u->pcm_handle, status)) < 0)
        pa_log("Failed to get delay: %s", snd_strerror(err));
    else
        frames = snd_pcm_status_get_delay(status);

    if (frames > 0)
        r = pa_bytes_to_usec(frames * u->frame_size, &u->source->sample_spec);

    return r;
}

static int build_pollfd(struct userdata *u) {
    int err;
    struct pollfd *pollfd;
    int n;

    pa_assert(u);
    pa_assert(u->pcm_handle);

    if ((n = snd_pcm_poll_descriptors_count(u->pcm_handle)) < 0) {
        pa_log("snd_pcm_poll_descriptors_count() failed: %s", snd_strerror(n));
        return -1;
    }

    if (u->alsa_rtpoll_item)
        pa_rtpoll_item_free(u->alsa_rtpoll_item);

    u->alsa_rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, n);
    pollfd = pa_rtpoll_item_get_pollfd(u->alsa_rtpoll_item, NULL);

    if ((err = snd_pcm_poll_descriptors(u->pcm_handle, pollfd, n)) < 0) {
        pa_log("snd_pcm_poll_descriptors() failed: %s", snd_strerror(err));
        return -1;
    }

    return 0;
}

static int suspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->pcm_handle);

    /* Let's suspend */
    snd_pcm_close(u->pcm_handle);
    u->pcm_handle = NULL;

    if (u->alsa_rtpoll_item) {
        pa_rtpoll_item_free(u->alsa_rtpoll_item);
        u->alsa_rtpoll_item = NULL;
    }

    pa_log_info("Device suspended...");

    return 0;
}

static int unsuspend(struct userdata *u) {
    pa_sample_spec ss;
    int err;
    pa_bool_t b;
    unsigned nfrags;
    snd_pcm_uframes_t period_size;

    pa_assert(u);
    pa_assert(!u->pcm_handle);

    pa_log_info("Trying resume...");

    snd_config_update_free_global();
    if ((err = snd_pcm_open(&u->pcm_handle, u->device_name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
        pa_log("Error opening PCM device %s: %s", u->device_name, snd_strerror(err));
        goto fail;
    }

    ss = u->source->sample_spec;
    nfrags = u->nfragments;
    period_size = u->fragment_size / u->frame_size;
    b = u->use_mmap;

    if ((err = pa_alsa_set_hw_params(u->pcm_handle, &ss, &nfrags, &period_size, &b, TRUE)) < 0) {
        pa_log("Failed to set hardware parameters: %s", snd_strerror(err));
        goto fail;
    }

    if (b != u->use_mmap) {
        pa_log_warn("Resume failed, couldn't get original access mode.");
        goto fail;
    }

    if (!pa_sample_spec_equal(&ss, &u->source->sample_spec)) {
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

    snd_pcm_start(u->pcm_handle);

    /* FIXME: We need to reload the volume somehow */

    pa_log_info("Resumed successfully...");

    return 0;

fail:
    if (u->pcm_handle) {
        snd_pcm_close(u->pcm_handle);
        u->pcm_handle = NULL;
    }

    return -1;
}

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (u->pcm_handle)
                r = source_get_latency(u);

            *((pa_usec_t*) data) = r;

            return 0;
        }

        case PA_SOURCE_MESSAGE_SET_STATE:

            switch ((pa_source_state_t) PA_PTR_TO_UINT(data)) {

                case PA_SOURCE_SUSPENDED:
                    pa_assert(PA_SOURCE_OPENED(u->source->thread_info.state));

                    if (suspend(u) < 0)
                        return -1;

                    break;

                case PA_SOURCE_IDLE:
                case PA_SOURCE_RUNNING:

                    if (u->source->thread_info.state == PA_SOURCE_INIT) {
                        if (build_pollfd(u) < 0)
                            return -1;

                        snd_pcm_start(u->pcm_handle);
                    }

                    if (u->source->thread_info.state == PA_SOURCE_SUSPENDED) {
                        if (unsuspend(u) < 0)
                            return -1;
                    }

                    break;

                case PA_SOURCE_UNLINKED:
                case PA_SOURCE_INIT:
                    ;
            }

            break;
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int mixer_callback(snd_mixer_elem_t *elem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(elem);

    pa_assert(u);
    pa_assert(u->mixer_handle);

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    if (mask & SND_CTL_EVENT_MASK_VALUE) {
        pa_source_get_volume(u->source);
        pa_source_get_mute(u->source);
    }

    return 0;
}

static int source_get_volume_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    int err;
    int i;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    for (i = 0; i < s->sample_spec.channels; i++) {
        long set_vol, vol;

        pa_assert(snd_mixer_selem_has_capture_channel(u->mixer_elem, u->mixer_map[i]));

        if ((err = snd_mixer_selem_get_capture_volume(u->mixer_elem, u->mixer_map[i], &vol)) < 0)
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

static int source_set_volume_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    int err;
    int i;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    for (i = 0; i < s->sample_spec.channels; i++) {
        long alsa_vol;
        pa_volume_t vol;

        pa_assert(snd_mixer_selem_has_capture_channel(u->mixer_elem, u->mixer_map[i]));

        vol = s->volume.values[i];

        if (vol > PA_VOLUME_NORM)
            vol = PA_VOLUME_NORM;

        alsa_vol = (long) roundf(((float) vol * (u->hw_volume_max - u->hw_volume_min)) / PA_VOLUME_NORM) + u->hw_volume_min;

        if ((err = snd_mixer_selem_set_capture_volume(u->mixer_elem, u->mixer_map[i], alsa_vol)) < 0)
            goto fail;
    }

    return 0;

fail:
    pa_log_error("Unable to set volume: %s", snd_strerror(err));

    s->get_volume = NULL;
    s->set_volume = NULL;
    return -1;
}

static int source_get_mute_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    int err, sw;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    if ((err = snd_mixer_selem_get_capture_switch(u->mixer_elem, 0, &sw)) < 0) {
        pa_log_error("Unable to get switch: %s", snd_strerror(err));

        s->get_mute = NULL;
        s->set_mute = NULL;
        return -1;
    }

    s->muted = !sw;

    return 0;
}

static int source_set_mute_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    int err;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    if ((err = snd_mixer_selem_set_capture_switch_all(u->mixer_elem, !s->muted)) < 0) {
        pa_log_error("Unable to set switch: %s", snd_strerror(err));

        s->get_mute = NULL;
        s->set_mute = NULL;
        return -1;
    }

    return 0;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    for (;;) {
        int ret;

        /* Read some data and pass it to the sources */
        if (PA_SOURCE_OPENED(u->source->thread_info.state)) {

            if (u->use_mmap) {
                if (mmap_read(u) < 0)
                    goto fail;

            } else {
                if (unix_read(u) < 0)
                    goto fail;
            }
        }

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll, 1)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        /* Tell ALSA about this and process its response */
        if (PA_SOURCE_OPENED(u->source->thread_info.state)) {
            struct pollfd *pollfd;
            unsigned short revents = 0;
            int err;
            unsigned n;

            pollfd = pa_rtpoll_item_get_pollfd(u->alsa_rtpoll_item, &n);

            if ((err = snd_pcm_poll_descriptors_revents(u->pcm_handle, pollfd, n, &revents)) < 0) {
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

                /* Try to recover from this error */

                switch (snd_pcm_state(u->pcm_handle)) {

                    case SND_PCM_STATE_XRUN:
                        if ((err = snd_pcm_recover(u->pcm_handle, -EPIPE, 1)) != 0) {
                            pa_log_warn("Could not recover from POLLERR|POLLNVAL|POLLHUP and XRUN: %s", snd_strerror(err));
                            goto fail;
                        }
                        break;

                    case SND_PCM_STATE_SUSPENDED:
                        if ((err = snd_pcm_recover(u->pcm_handle, -ESTRPIPE, 1)) != 0) {
                            pa_log_warn("Could not recover from POLLERR|POLLNVAL|POLLHUP and SUSPENDED: %s", snd_strerror(err));
                            goto fail;
                        }
                        break;

                    default:

                        snd_pcm_drop(u->pcm_handle);

                        if ((err = snd_pcm_prepare(u->pcm_handle)) < 0) {
                            pa_log_warn("Could not recover from POLLERR|POLLNVAL|POLLHUP with snd_pcm_prepare(): %s", snd_strerror(err));
                            goto fail;
                        }
                        break;
                }
            }
        }
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_module*m) {

    pa_modargs *ma = NULL;
    struct userdata *u = NULL;
    const char *dev_id;
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
    pa_bool_t use_mmap = TRUE, b;

    snd_pcm_info_alloca(&pcm_info);

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_ALSA) < 0) {
        pa_log("Failed to parse sample specification");
        goto fail;
    }

    frame_size = pa_frame_size(&ss);

    nfrags = m->core->default_n_fragments;
    frag_size = pa_usec_to_bytes(m->core->default_fragment_size_msec*1000, &ss);
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
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->use_mmap = use_mmap;
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop);
    u->rtpoll = pa_rtpoll_new();
    u->alsa_rtpoll_item = NULL;
    pa_rtpoll_item_new_asyncmsgq(u->rtpoll, PA_RTPOLL_EARLY, u->thread_mq.inq);

    snd_config_update_free_global();

    b = use_mmap;

    if ((dev_id = pa_modargs_get_value(ma, "device_id", NULL))) {

        if (!(u->pcm_handle = pa_alsa_open_by_device_id(
                      dev_id,
                      &u->device_name,
                      &ss, &map,
                      SND_PCM_STREAM_CAPTURE,
                      &nfrags, &period_size,
                      &b)))
            goto fail;

    } else {

        if (!(u->pcm_handle = pa_alsa_open_by_device_string(
                      pa_modargs_get_value(ma, "device", DEFAULT_DEVICE),
                      &u->device_name,
                      &ss, &map,
                      SND_PCM_STREAM_CAPTURE,
                      &nfrags, &period_size,
                      &b)))
            goto fail;
    }

    pa_assert(u->device_name);
    pa_log_info("Successfully opened device %s.", u->device_name);

    if (use_mmap && !b) {
        pa_log_info("Device doesn't support mmap(), falling back to UNIX read/write mode.");
        u->use_mmap = use_mmap = b;
    }

    if (u->use_mmap)
        pa_log_info("Successfully enabled mmap() mode.");

    if ((err = snd_pcm_info(u->pcm_handle, pcm_info)) < 0) {
        pa_log("Error fetching PCM info: %s", snd_strerror(err));
        goto fail;
    }

    if ((err = pa_alsa_set_sw_params(u->pcm_handle)) < 0) {
        pa_log("Failed to set software parameters: %s", snd_strerror(err));
        goto fail;
    }

    /* ALSA might tweak the sample spec, so recalculate the frame size */
    frame_size = pa_frame_size(&ss);

    if ((err = snd_mixer_open(&u->mixer_handle, 0)) < 0)
        pa_log("Error opening mixer: %s", snd_strerror(err));
    else {
        pa_bool_t found = FALSE;

        if (pa_alsa_prepare_mixer(u->mixer_handle, u->device_name) >= 0)
            found = TRUE;
        else {
            char *md = pa_sprintf_malloc("hw:%s", dev_id);

            if (strcmp(u->device_name, md))
                if (pa_alsa_prepare_mixer(u->mixer_handle, md) >= 0)
                    found = TRUE;

            pa_xfree(md);
        }

        if (found)
            if (!(u->mixer_elem = pa_alsa_find_elem(u->mixer_handle, "Capture", "Mic")))
                found = FALSE;

        if (!found) {
            snd_mixer_close(u->mixer_handle);
            u->mixer_handle = NULL;
        }
    }

    if ((name = pa_modargs_get_value(ma, "source_name", NULL)))
        namereg_fail = 1;
    else {
        name = name_buf = pa_sprintf_malloc("alsa_input.%s", u->device_name);
        namereg_fail = 0;
    }

    u->source = pa_source_new(m->core, __FILE__, name, namereg_fail, &ss, &map);
    pa_xfree(name_buf);

    if (!u->source) {
        pa_log("Failed to create source object");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg;
    u->source->userdata = u;

    pa_source_set_module(u->source, m);
    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    pa_source_set_description(u->source, t = pa_sprintf_malloc(
                                      "ALSA PCM on %s (%s)%s",
                                      u->device_name,
                                      snd_pcm_info_get_name(pcm_info),
                                      use_mmap ? " via DMA" : ""));
    pa_xfree(t);

    u->source->flags = PA_SOURCE_HARDWARE|PA_SOURCE_LATENCY;

    u->frame_size = frame_size;
    u->fragment_size = frag_size = period_size * frame_size;
    u->nfragments = nfrags;
    u->hwbuf_size = u->fragment_size * nfrags;

    pa_log_info("Using %u fragments of size %lu bytes.", nfrags, (long unsigned) u->fragment_size);

    if (u->mixer_handle) {
        pa_assert(u->mixer_elem);

        if (snd_mixer_selem_has_capture_volume(u->mixer_elem))
            if (pa_alsa_calc_mixer_map(u->mixer_elem, &map, u->mixer_map, FALSE) >= 0) {
                u->source->get_volume = source_get_volume_cb;
                u->source->set_volume = source_set_volume_cb;
                snd_mixer_selem_get_capture_volume_range(u->mixer_elem, &u->hw_volume_min, &u->hw_volume_max);
                u->source->flags |= PA_SOURCE_HW_VOLUME_CTRL;
            }

        if (snd_mixer_selem_has_capture_switch(u->mixer_elem)) {
            u->source->get_mute = source_get_mute_cb;
            u->source->set_mute = source_set_mute_cb;
            u->source->flags |= PA_SOURCE_HW_VOLUME_CTRL;
        }

        u->mixer_fdl = pa_alsa_fdlist_new();

        if (pa_alsa_fdlist_set_mixer(u->mixer_fdl, u->mixer_handle, m->core->mainloop) < 0) {
            pa_log("Failed to initialize file descriptor monitoring");
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
    if (u->source->get_volume)
        u->source->get_volume(u->source);
    if (u->source->get_mute)
        u->source->get_mute(u->source);

    pa_source_put(u->source);

    pa_modargs_free(ma);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->source)
        pa_source_unref(u->source);

    if (u->alsa_rtpoll_item)
        pa_rtpoll_item_free(u->alsa_rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->mixer_fdl)
        pa_alsa_fdlist_free(u->mixer_fdl);

    if (u->mixer_handle)
        snd_mixer_close(u->mixer_handle);

    if (u->pcm_handle) {
        snd_pcm_drop(u->pcm_handle);
        snd_pcm_close(u->pcm_handle);
    }

    pa_xfree(u->device_name);
    pa_xfree(u);

    snd_config_update_free_global();
}
