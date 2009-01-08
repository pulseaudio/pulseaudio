/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering
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

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/util.h>
#include <pulse/timeval.h>

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
#include <pulsecore/rtclock.h>
#include <pulsecore/time-smoother.h>

#include "alsa-util.h"
#include "module-alsa-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("ALSA Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "device=<ALSA device> "
        "device_id=<ALSA card index> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "mmap=<enable memory mapping?> "
        "tsched=<enable system timer based scheduling mode?> "
        "tsched_buffer_size=<buffer size when using timer based scheduling> "
        "tsched_buffer_watermark=<lower fill watermark>");

static const char* const valid_modargs[] = {
    "sink_name",
    "device",
    "device_id",
    "format",
    "rate",
    "channels",
    "channel_map",
    "fragments",
    "fragment_size",
    "mmap",
    "tsched",
    "tsched_buffer_size",
    "tsched_buffer_watermark",
    NULL
};

#define DEFAULT_DEVICE "default"
#define DEFAULT_TSCHED_BUFFER_USEC (2*PA_USEC_PER_SEC)            /* 2s */
#define DEFAULT_TSCHED_WATERMARK_USEC (20*PA_USEC_PER_MSEC)       /* 20ms */
#define TSCHED_MIN_SLEEP_USEC (3*PA_USEC_PER_MSEC)                /* 3ms */
#define TSCHED_MIN_WAKEUP_USEC (3*PA_USEC_PER_MSEC)               /* 3ms */

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    snd_pcm_t *pcm_handle;

    pa_alsa_fdlist *mixer_fdl;
    snd_mixer_t *mixer_handle;
    snd_mixer_elem_t *mixer_elem;
    long hw_volume_max, hw_volume_min;
    long hw_dB_max, hw_dB_min;
    pa_bool_t hw_dB_supported;
    pa_bool_t mixer_seperate_channels;
    pa_cvolume hardware_volume;

    size_t frame_size, fragment_size, hwbuf_size, tsched_watermark;
    unsigned nfragments;
    pa_memchunk memchunk;

    char *device_name;

    pa_bool_t use_mmap, use_tsched;

    pa_bool_t first, after_rewind;

    pa_rtpoll_item *alsa_rtpoll_item;

    snd_mixer_selem_channel_id_t mixer_map[SND_MIXER_SCHN_LAST];

    pa_smoother *smoother;
    int64_t frame_index;
    uint64_t since_start;

    snd_pcm_sframes_t hwbuf_unused_frames;
};

static void fix_tsched_watermark(struct userdata *u) {
    size_t max_use;
    size_t min_sleep, min_wakeup;
    pa_assert(u);

    max_use = u->hwbuf_size - (size_t) u->hwbuf_unused_frames * u->frame_size;

    min_sleep = pa_usec_to_bytes(TSCHED_MIN_SLEEP_USEC, &u->sink->sample_spec);
    min_wakeup = pa_usec_to_bytes(TSCHED_MIN_WAKEUP_USEC, &u->sink->sample_spec);

    if (min_sleep > max_use/2)
        min_sleep = pa_frame_align(max_use/2, &u->sink->sample_spec);
    if (min_sleep < u->frame_size)
        min_sleep = u->frame_size;

    if (min_wakeup > max_use/2)
        min_wakeup = pa_frame_align(max_use/2, &u->sink->sample_spec);
    if (min_wakeup < u->frame_size)
        min_wakeup = u->frame_size;

    if (u->tsched_watermark > max_use-min_sleep)
        u->tsched_watermark = max_use-min_sleep;

    if (u->tsched_watermark < min_wakeup)
        u->tsched_watermark = min_wakeup;
}

static void hw_sleep_time(struct userdata *u, pa_usec_t *sleep_usec, pa_usec_t*process_usec) {
    pa_usec_t usec, wm;

    pa_assert(sleep_usec);
    pa_assert(process_usec);

    pa_assert(u);

    usec = pa_sink_get_requested_latency_within_thread(u->sink);

    if (usec == (pa_usec_t) -1)
        usec = pa_bytes_to_usec(u->hwbuf_size, &u->sink->sample_spec);

/*     pa_log_debug("hw buffer time: %u ms", (unsigned) (usec / PA_USEC_PER_MSEC)); */

    wm = pa_bytes_to_usec(u->tsched_watermark, &u->sink->sample_spec);

    if (usec >= wm) {
        *sleep_usec = usec - wm;
        *process_usec = wm;
    } else
        *process_usec = *sleep_usec = usec / 2;

/*     pa_log_debug("after watermark: %u ms", (unsigned) (*sleep_usec / PA_USEC_PER_MSEC)); */
}

static int try_recover(struct userdata *u, const char *call, int err) {
    pa_assert(u);
    pa_assert(call);
    pa_assert(err < 0);

    pa_log_debug("%s: %s", call, snd_strerror(err));

    pa_assert(err != -EAGAIN);

    if (err == -EPIPE)
        pa_log_debug("%s: Buffer underrun!", call);

    if ((err = snd_pcm_recover(u->pcm_handle, err, 1)) == 0) {
        u->first = TRUE;
        u->since_start = 0;
        return 0;
    }

    pa_log("%s: %s", call, snd_strerror(err));
    return -1;
}

static size_t check_left_to_play(struct userdata *u, snd_pcm_sframes_t n) {
    size_t left_to_play;

    if ((size_t) n*u->frame_size < u->hwbuf_size)
        left_to_play = u->hwbuf_size - ((size_t) n*u->frame_size);
    else
        left_to_play = 0;

    if (left_to_play > 0) {
/*         pa_log_debug("%0.2f ms left to play", (double) pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) / PA_USEC_PER_MSEC); */
    } else if (!u->first && !u->after_rewind) {
        pa_log_info("Underrun!");

        if (u->use_tsched) {
            size_t old_watermark = u->tsched_watermark;

            u->tsched_watermark *= 2;
            fix_tsched_watermark(u);

            if (old_watermark != u->tsched_watermark)
                pa_log_notice("Increasing wakeup watermark to %0.2f ms",
                              (double) pa_bytes_to_usec(u->tsched_watermark, &u->sink->sample_spec) / PA_USEC_PER_MSEC);
        }
    }

    return left_to_play;
}

static int mmap_write(struct userdata *u, pa_usec_t *sleep_usec, pa_bool_t polled) {
    int work_done = 0;
    pa_usec_t max_sleep_usec = 0, process_usec = 0;
    size_t left_to_play;

    pa_assert(u);
    pa_sink_assert_ref(u->sink);

    if (u->use_tsched)
        hw_sleep_time(u, &max_sleep_usec, &process_usec);

    for (;;) {
        snd_pcm_sframes_t n;
        int r;

        snd_pcm_hwsync(u->pcm_handle);

        /* First we determine how many samples are missing to fill the
         * buffer up to 100% */

        if (PA_UNLIKELY((n = pa_alsa_safe_avail_update(u->pcm_handle, u->hwbuf_size, &u->sink->sample_spec)) < 0)) {

            if ((r = try_recover(u, "snd_pcm_avail_update", (int) n)) == 0)
                continue;

            return r;
        }

        left_to_play = check_left_to_play(u, n);

        if (u->use_tsched)

            /* We won't fill up the playback buffer before at least
            * half the sleep time is over because otherwise we might
            * ask for more data from the clients then they expect. We
            * need to guarantee that clients only have to keep around
            * a single hw buffer length. */

            if (!polled &&
                pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) > process_usec+max_sleep_usec/2)
                break;

        if (PA_UNLIKELY(n <= u->hwbuf_unused_frames)) {

            if (polled)
                pa_log("ALSA woke us up to write new data to the device, but there was actually nothing to write! "
                       "Most likely this is an ALSA driver bug. Please report this issue to the PulseAudio developers.");

            break;
        }

        n -= u->hwbuf_unused_frames;

        polled = FALSE;

/*         pa_log_debug("Filling up"); */

        for (;;) {
            pa_memchunk chunk;
            void *p;
            int err;
            const snd_pcm_channel_area_t *areas;
            snd_pcm_uframes_t offset, frames = (snd_pcm_uframes_t) n;
            snd_pcm_sframes_t sframes;

/*             pa_log_debug("%lu frames to write", (unsigned long) frames); */

            if (PA_UNLIKELY((err = pa_alsa_safe_mmap_begin(u->pcm_handle, &areas, &offset, &frames, u->hwbuf_size, &u->sink->sample_spec)) < 0)) {

                if ((r = try_recover(u, "snd_pcm_mmap_begin", err)) == 0)
                    continue;

                return r;
            }

            /* Make sure that if these memblocks need to be copied they will fit into one slot */
            if (frames > pa_mempool_block_size_max(u->sink->core->mempool)/u->frame_size)
                frames = pa_mempool_block_size_max(u->sink->core->mempool)/u->frame_size;

            /* Check these are multiples of 8 bit */
            pa_assert((areas[0].first & 7) == 0);
            pa_assert((areas[0].step & 7)== 0);

            /* We assume a single interleaved memory buffer */
            pa_assert((areas[0].first >> 3) == 0);
            pa_assert((areas[0].step >> 3) == u->frame_size);

            p = (uint8_t*) areas[0].addr + (offset * u->frame_size);

            chunk.memblock = pa_memblock_new_fixed(u->core->mempool, p, frames * u->frame_size, TRUE);
            chunk.length = pa_memblock_get_length(chunk.memblock);
            chunk.index = 0;

            pa_sink_render_into_full(u->sink, &chunk);

            /* FIXME: Maybe we can do something to keep this memory block
             * a little bit longer around? */
            pa_memblock_unref_fixed(chunk.memblock);

            if (PA_UNLIKELY((sframes = snd_pcm_mmap_commit(u->pcm_handle, offset, frames)) < 0)) {

                if ((r = try_recover(u, "snd_pcm_mmap_commit", (int) sframes)) == 0)
                    continue;

                return r;
            }

            work_done = 1;

            u->frame_index += (int64_t) frames;
            u->since_start += frames * u->frame_size;

/*             pa_log_debug("wrote %lu frames", (unsigned long) frames); */

            if (frames >= (snd_pcm_uframes_t) n)
                break;

            n -= (snd_pcm_sframes_t) frames;
        }
    }

    *sleep_usec = pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) - process_usec;
    return work_done;
}

static int unix_write(struct userdata *u, pa_usec_t *sleep_usec, pa_bool_t polled) {
    int work_done = 0;
    pa_usec_t max_sleep_usec = 0, process_usec = 0;
    size_t left_to_play;

    pa_assert(u);
    pa_sink_assert_ref(u->sink);

    if (u->use_tsched)
        hw_sleep_time(u, &max_sleep_usec, &process_usec);

    for (;;) {
        snd_pcm_sframes_t n;
        int r;

        snd_pcm_hwsync(u->pcm_handle);

        if (PA_UNLIKELY((n = pa_alsa_safe_avail_update(u->pcm_handle, u->hwbuf_size, &u->sink->sample_spec)) < 0)) {

            if ((r = try_recover(u, "snd_pcm_avail_update", (int) n)) == 0)
                continue;

            return r;
        }

        left_to_play = check_left_to_play(u, n);

        if (u->use_tsched)

            /* We won't fill up the playback buffer before at least
            * half the sleep time is over because otherwise we might
            * ask for more data from the clients then they expect. We
            * need to guarantee that clients only have to keep around
            * a single hw buffer length. */

            if (!polled &&
                pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) > process_usec+max_sleep_usec/2)
                break;

        if (PA_UNLIKELY(n <= u->hwbuf_unused_frames)) {

            if (polled)
                pa_log("ALSA woke us up to write new data to the device, but there was actually nothing to write! "
                       "Most likely this is an ALSA driver bug. Please report this issue to the PulseAudio developers.");

            break;
        }

        n -= u->hwbuf_unused_frames;

        polled = FALSE;

        for (;;) {
            snd_pcm_sframes_t frames;
            void *p;

/*         pa_log_debug("%lu frames to write", (unsigned long) frames); */

            if (u->memchunk.length <= 0)
                pa_sink_render(u->sink, (size_t) n * u->frame_size, &u->memchunk);

            pa_assert(u->memchunk.length > 0);

            frames = (snd_pcm_sframes_t) (u->memchunk.length / u->frame_size);

            if (frames > n)
                frames = n;

            p = pa_memblock_acquire(u->memchunk.memblock);
            frames = snd_pcm_writei(u->pcm_handle, (const uint8_t*) p + u->memchunk.index, (snd_pcm_uframes_t) frames);
            pa_memblock_release(u->memchunk.memblock);

            pa_assert(frames != 0);

            if (PA_UNLIKELY(frames < 0)) {

                if ((r = try_recover(u, "snd_pcm_writei", (int) frames)) == 0)
                    continue;

                return r;
            }

            u->memchunk.index += (size_t) frames * u->frame_size;
            u->memchunk.length -= (size_t) frames * u->frame_size;

            if (u->memchunk.length <= 0) {
                pa_memblock_unref(u->memchunk.memblock);
                pa_memchunk_reset(&u->memchunk);
            }

            work_done = 1;

            u->frame_index += frames;
            u->since_start += (size_t) frames * u->frame_size;

/*         pa_log_debug("wrote %lu frames", (unsigned long) frames); */

            if (frames >= n)
                break;

            n -= frames;
        }
    }

    *sleep_usec = pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) - process_usec;
    return work_done;
}

static void update_smoother(struct userdata *u) {
    snd_pcm_sframes_t delay  = 0;
    int64_t frames;
    int err;
    pa_usec_t now1, now2;
/*     struct timeval timestamp; */
    snd_pcm_status_t *status;

    snd_pcm_status_alloca(&status);

    pa_assert(u);
    pa_assert(u->pcm_handle);

    /* Let's update the time smoother */

    snd_pcm_hwsync(u->pcm_handle);
    snd_pcm_avail_update(u->pcm_handle);

/*     if (PA_UNLIKELY((err = snd_pcm_status(u->pcm_handle, status)) < 0)) { */
/*         pa_log("Failed to query DSP status data: %s", snd_strerror(err)); */
/*         return; */
/*     } */

/*     delay = snd_pcm_status_get_delay(status); */

    if (PA_UNLIKELY((err = snd_pcm_delay(u->pcm_handle, &delay)) < 0)) {
        pa_log("Failed to query DSP status data: %s", snd_strerror(err));
        return;
    }

    frames = u->frame_index - delay;

/*     pa_log_debug("frame_index = %llu, delay = %llu, p = %llu", (unsigned long long) u->frame_index, (unsigned long long) delay, (unsigned long long) frames); */

/*     snd_pcm_status_get_tstamp(status, &timestamp); */
/*     pa_rtclock_from_wallclock(&timestamp); */
/*     now1 = pa_timeval_load(&timestamp); */

    now1 = pa_rtclock_usec();
    now2 = pa_bytes_to_usec((uint64_t) frames * u->frame_size, &u->sink->sample_spec);
    pa_smoother_put(u->smoother, now1, now2);
}

static pa_usec_t sink_get_latency(struct userdata *u) {
    pa_usec_t r = 0;
    int64_t delay;
    pa_usec_t now1, now2;

    pa_assert(u);

    now1 = pa_rtclock_usec();
    now2 = pa_smoother_get(u->smoother, now1);

    delay = (int64_t) pa_bytes_to_usec((uint64_t) u->frame_index * u->frame_size, &u->sink->sample_spec) - (int64_t) now2;

    if (delay > 0)
        r = (pa_usec_t) delay;

    if (u->memchunk.memblock)
        r += pa_bytes_to_usec(u->memchunk.length, &u->sink->sample_spec);

    return r;
}

static int build_pollfd(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->pcm_handle);

    if (u->alsa_rtpoll_item)
        pa_rtpoll_item_free(u->alsa_rtpoll_item);

    if (!(u->alsa_rtpoll_item = pa_alsa_build_pollfd(u->pcm_handle, u->rtpoll)))
        return -1;

    return 0;
}

static int suspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->pcm_handle);

    pa_smoother_pause(u->smoother, pa_rtclock_usec());

    /* Let's suspend -- we don't call snd_pcm_drain() here since that might
     * take awfully long with our long buffer sizes today. */
    snd_pcm_close(u->pcm_handle);
    u->pcm_handle = NULL;

    if (u->alsa_rtpoll_item) {
        pa_rtpoll_item_free(u->alsa_rtpoll_item);
        u->alsa_rtpoll_item = NULL;
    }

    pa_log_info("Device suspended...");

    return 0;
}

static int update_sw_params(struct userdata *u) {
    snd_pcm_uframes_t avail_min;
    int err;

    pa_assert(u);

    /* Use the full buffer if noone asked us for anything specific */
    u->hwbuf_unused_frames = 0;

    if (u->use_tsched) {
        pa_usec_t latency;

        if ((latency = pa_sink_get_requested_latency_within_thread(u->sink)) != (pa_usec_t) -1) {
            size_t b;

            pa_log_debug("latency set to %0.2fms", (double) latency / PA_USEC_PER_MSEC);

            b = pa_usec_to_bytes(latency, &u->sink->sample_spec);

            /* We need at least one sample in our buffer */

            if (PA_UNLIKELY(b < u->frame_size))
                b = u->frame_size;

            u->hwbuf_unused_frames = (snd_pcm_sframes_t)
                (PA_LIKELY(b < u->hwbuf_size) ?
                 ((u->hwbuf_size - b) / u->frame_size) : 0);
        }

        fix_tsched_watermark(u);
    }

    pa_log_debug("hwbuf_unused_frames=%lu", (unsigned long) u->hwbuf_unused_frames);

    /* We need at last one frame in the used part of the buffer */
    avail_min = (snd_pcm_uframes_t) u->hwbuf_unused_frames + 1;

    if (u->use_tsched) {
        pa_usec_t sleep_usec, process_usec;

        hw_sleep_time(u, &sleep_usec, &process_usec);
        avail_min += pa_usec_to_bytes(sleep_usec, &u->sink->sample_spec);
    }

    pa_log_debug("setting avail_min=%lu", (unsigned long) avail_min);

    if ((err = pa_alsa_set_sw_params(u->pcm_handle, avail_min)) < 0) {
        pa_log("Failed to set software parameters: %s", snd_strerror(err));
        return err;
    }

    pa_sink_set_max_request(u->sink, u->hwbuf_size - (size_t) u->hwbuf_unused_frames * u->frame_size);

    return 0;
}

static int unsuspend(struct userdata *u) {
    pa_sample_spec ss;
    int err;
    pa_bool_t b, d;
    unsigned nfrags;
    snd_pcm_uframes_t period_size;

    pa_assert(u);
    pa_assert(!u->pcm_handle);

    pa_log_info("Trying resume...");

    snd_config_update_free_global();
    if ((err = snd_pcm_open(&u->pcm_handle, u->device_name, SND_PCM_STREAM_PLAYBACK,
                            /*SND_PCM_NONBLOCK|*/
                            SND_PCM_NO_AUTO_RESAMPLE|
                            SND_PCM_NO_AUTO_CHANNELS|
                            SND_PCM_NO_AUTO_FORMAT)) < 0) {
        pa_log("Error opening PCM device %s: %s", u->device_name, snd_strerror(err));
        goto fail;
    }

    ss = u->sink->sample_spec;
    nfrags = u->nfragments;
    period_size = u->fragment_size / u->frame_size;
    b = u->use_mmap;
    d = u->use_tsched;

    if ((err = pa_alsa_set_hw_params(u->pcm_handle, &ss, &nfrags, &period_size, u->hwbuf_size / u->frame_size, &b, &d, TRUE)) < 0) {
        pa_log("Failed to set hardware parameters: %s", snd_strerror(err));
        goto fail;
    }

    if (b != u->use_mmap || d != u->use_tsched) {
        pa_log_warn("Resume failed, couldn't get original access mode.");
        goto fail;
    }

    if (!pa_sample_spec_equal(&ss, &u->sink->sample_spec)) {
        pa_log_warn("Resume failed, couldn't restore original sample settings.");
        goto fail;
    }

    if (nfrags != u->nfragments || period_size*u->frame_size != u->fragment_size) {
        pa_log_warn("Resume failed, couldn't restore original fragment settings. (Old: %lu*%lu, New %lu*%lu)",
                    (unsigned long) u->nfragments, (unsigned long) u->fragment_size,
                    (unsigned long) nfrags, period_size * u->frame_size);
        goto fail;
    }

    if (update_sw_params(u) < 0)
        goto fail;

    if (build_pollfd(u) < 0)
        goto fail;

    /* FIXME: We need to reload the volume somehow */

    u->first = TRUE;
    u->since_start = 0;

    pa_log_info("Resumed successfully...");

    return 0;

fail:
    if (u->pcm_handle) {
        snd_pcm_close(u->pcm_handle);
        u->pcm_handle = NULL;
    }

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

            return 0;
        }

        case PA_SINK_MESSAGE_SET_STATE:

            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {

                case PA_SINK_SUSPENDED:
                    pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

                    if (suspend(u) < 0)
                        return -1;

                    break;

                case PA_SINK_IDLE:
                case PA_SINK_RUNNING:

                    if (u->sink->thread_info.state == PA_SINK_INIT) {
                        if (build_pollfd(u) < 0)
                            return -1;
                    }

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                        if (unsuspend(u) < 0)
                            return -1;
                    }

                    break;

                case PA_SINK_UNLINKED:
                case PA_SINK_INIT:
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
        pa_sink_get_volume(u->sink, TRUE);
        pa_sink_get_mute(u->sink, TRUE);
    }

    return 0;
}

static pa_volume_t from_alsa_volume(struct userdata *u, long alsa_vol) {

    return (pa_volume_t) round(((double) (alsa_vol - u->hw_volume_min) * PA_VOLUME_NORM) /
                               (double) (u->hw_volume_max - u->hw_volume_min));
}

static long to_alsa_volume(struct userdata *u, pa_volume_t vol) {
    long alsa_vol;

    alsa_vol = (long) round(((double) vol * (double) (u->hw_volume_max - u->hw_volume_min))
                            / PA_VOLUME_NORM) + u->hw_volume_min;

    return PA_CLAMP_UNLIKELY(alsa_vol, u->hw_volume_min, u->hw_volume_max);
}

static int sink_get_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;
    unsigned i;
    pa_cvolume r;
    char t[PA_CVOLUME_SNPRINT_MAX];

    pa_assert(u);
    pa_assert(u->mixer_elem);

    if (u->mixer_seperate_channels) {

        r.channels = s->sample_spec.channels;

        for (i = 0; i < s->sample_spec.channels; i++) {
            long alsa_vol;

            if (u->hw_dB_supported) {

                if ((err = snd_mixer_selem_get_playback_dB(u->mixer_elem, u->mixer_map[i], &alsa_vol)) < 0)
                    goto fail;

#ifdef HAVE_VALGRIND_MEMCHECK_H
                VALGRIND_MAKE_MEM_DEFINED(&alsa_vol, sizeof(alsa_vol));
#endif

                r.values[i] = pa_sw_volume_from_dB((double) (alsa_vol - u->hw_dB_max) / 100.0);
            } else {

                if ((err = snd_mixer_selem_get_playback_volume(u->mixer_elem, u->mixer_map[i], &alsa_vol)) < 0)
                    goto fail;

                r.values[i] = from_alsa_volume(u, alsa_vol);
            }
        }

    } else {
        long alsa_vol;

        if (u->hw_dB_supported) {

            if ((err = snd_mixer_selem_get_playback_dB(u->mixer_elem, SND_MIXER_SCHN_MONO, &alsa_vol)) < 0)
                goto fail;

#ifdef HAVE_VALGRIND_MEMCHECK_H
            VALGRIND_MAKE_MEM_DEFINED(&alsa_vol, sizeof(alsa_vol));
#endif

            pa_cvolume_set(&r, s->sample_spec.channels, pa_sw_volume_from_dB((double) (alsa_vol - u->hw_dB_max) / 100.0));

        } else {

            if ((err = snd_mixer_selem_get_playback_volume(u->mixer_elem, SND_MIXER_SCHN_MONO, &alsa_vol)) < 0)
                goto fail;

            pa_cvolume_set(&r, s->sample_spec.channels, from_alsa_volume(u, alsa_vol));
        }
    }

    pa_log_debug("Read hardware volume: %s", pa_cvolume_snprint(t, sizeof(t), &r));

    if (!pa_cvolume_equal(&u->hardware_volume, &r)) {

        u->hardware_volume = s->volume = r;

        if (u->hw_dB_supported) {
            pa_cvolume reset;

            /* Hmm, so the hardware volume changed, let's reset our software volume */

            pa_cvolume_reset(&reset, s->sample_spec.channels);
            pa_sink_set_soft_volume(s, &reset);
        }
    }

    return 0;

fail:
    pa_log_error("Unable to read volume: %s", snd_strerror(err));

    return -1;
}

static int sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err;
    unsigned i;
    pa_cvolume r;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    if (u->mixer_seperate_channels) {

        r.channels = s->sample_spec.channels;

        for (i = 0; i < s->sample_spec.channels; i++) {
            long alsa_vol;
            pa_volume_t vol;

            vol = s->volume.values[i];

            if (u->hw_dB_supported) {

                alsa_vol = (long) (pa_sw_volume_to_dB(vol) * 100);
                alsa_vol += u->hw_dB_max;
                alsa_vol = PA_CLAMP_UNLIKELY(alsa_vol, u->hw_dB_min, u->hw_dB_max);

                if ((err = snd_mixer_selem_set_playback_dB(u->mixer_elem, u->mixer_map[i], alsa_vol, 1)) < 0)
                    goto fail;

                if ((err = snd_mixer_selem_get_playback_dB(u->mixer_elem, u->mixer_map[i], &alsa_vol)) < 0)
                    goto fail;

                r.values[i] = pa_sw_volume_from_dB((double) (alsa_vol - u->hw_dB_max) / 100.0);

            } else {
                alsa_vol = to_alsa_volume(u, vol);

                if ((err = snd_mixer_selem_set_playback_volume(u->mixer_elem, u->mixer_map[i], alsa_vol)) < 0)
                    goto fail;

                if ((err = snd_mixer_selem_get_playback_volume(u->mixer_elem, u->mixer_map[i], &alsa_vol)) < 0)
                    goto fail;

                r.values[i] = from_alsa_volume(u, alsa_vol);
            }
        }

    } else {
        pa_volume_t vol;
        long alsa_vol;

        vol = pa_cvolume_max(&s->volume);

        if (u->hw_dB_supported) {
            alsa_vol = (long) (pa_sw_volume_to_dB(vol) * 100);
            alsa_vol += u->hw_dB_max;
            alsa_vol = PA_CLAMP_UNLIKELY(alsa_vol, u->hw_dB_min, u->hw_dB_max);

            if ((err = snd_mixer_selem_set_playback_dB_all(u->mixer_elem, alsa_vol, 1)) < 0)
                goto fail;

            if ((err = snd_mixer_selem_get_playback_dB(u->mixer_elem, SND_MIXER_SCHN_MONO, &alsa_vol)) < 0)
                goto fail;

            pa_cvolume_set(&r, s->volume.channels, pa_sw_volume_from_dB((double) (alsa_vol - u->hw_dB_max) / 100.0));

        } else {
            alsa_vol = to_alsa_volume(u, vol);

            if ((err = snd_mixer_selem_set_playback_volume_all(u->mixer_elem, alsa_vol)) < 0)
                goto fail;

            if ((err = snd_mixer_selem_get_playback_volume(u->mixer_elem, SND_MIXER_SCHN_MONO, &alsa_vol)) < 0)
                goto fail;

            pa_cvolume_set(&r, s->sample_spec.channels, from_alsa_volume(u, alsa_vol));
        }
    }

    u->hardware_volume = r;

    if (u->hw_dB_supported) {
        char t[PA_CVOLUME_SNPRINT_MAX];

        /* Match exactly what the user requested by software */

        pa_sw_cvolume_divide(&r, &s->volume, &r);
        pa_sink_set_soft_volume(s, &r);

        pa_log_debug("Requested volume: %s", pa_cvolume_snprint(t, sizeof(t), &s->volume));
        pa_log_debug("Got hardware volume: %s", pa_cvolume_snprint(t, sizeof(t), &u->hardware_volume));
        pa_log_debug("Calculated software volume: %s", pa_cvolume_snprint(t, sizeof(t), &r));

    } else

        /* We can't match exactly what the user requested, hence let's
         * at least tell the user about it */

        s->volume = r;

    return 0;

fail:
    pa_log_error("Unable to set volume: %s", snd_strerror(err));

    return -1;
}

static int sink_get_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    int err, sw;

    pa_assert(u);
    pa_assert(u->mixer_elem);

    if ((err = snd_mixer_selem_get_playback_switch(u->mixer_elem, 0, &sw)) < 0) {
        pa_log_error("Unable to get switch: %s", snd_strerror(err));
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
        return -1;
    }

    return 0;
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    snd_pcm_sframes_t before;
    pa_assert(u);

    if (!u->pcm_handle)
        return;

    before = u->hwbuf_unused_frames;
    update_sw_params(u);

    /* Let's check whether we now use only a smaller part of the
    buffer then before. If so, we need to make sure that subsequent
    rewinds are relative to the new maxium fill level and not to the
    current fill level. Thus, let's do a full rewind once, to clear
    things up. */

    if (u->hwbuf_unused_frames > before) {
        pa_log_debug("Requesting rewind due to latency change.");
        pa_sink_request_rewind(s, (size_t) -1);
    }
}

static int process_rewind(struct userdata *u) {
    snd_pcm_sframes_t unused;
    size_t rewind_nbytes, unused_nbytes, limit_nbytes;
    pa_assert(u);

    /* Figure out how much we shall rewind and reset the counter */
    rewind_nbytes = u->sink->thread_info.rewind_nbytes;
    u->sink->thread_info.rewind_nbytes = 0;

    if (rewind_nbytes <= 0)
        goto finish;

    pa_assert(rewind_nbytes > 0);
    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    snd_pcm_hwsync(u->pcm_handle);
    if ((unused = snd_pcm_avail_update(u->pcm_handle)) < 0) {
        pa_log("snd_pcm_avail_update() failed: %s", snd_strerror((int) unused));
        return -1;
    }

    unused_nbytes = u->tsched_watermark + (size_t) unused * u->frame_size;

    if (u->hwbuf_size > unused_nbytes)
        limit_nbytes = u->hwbuf_size - unused_nbytes;
    else
        limit_nbytes = 0;

    if (rewind_nbytes > limit_nbytes)
        rewind_nbytes = limit_nbytes;

    if (rewind_nbytes > 0) {
        snd_pcm_sframes_t in_frames, out_frames;

        pa_log_debug("Limited to %lu bytes.", (unsigned long) rewind_nbytes);

        in_frames = (snd_pcm_sframes_t) (rewind_nbytes / u->frame_size);
        pa_log_debug("before: %lu", (unsigned long) in_frames);
        if ((out_frames = snd_pcm_rewind(u->pcm_handle, (snd_pcm_uframes_t) in_frames)) < 0) {
            pa_log("snd_pcm_rewind() failed: %s", snd_strerror((int) out_frames));
            return -1;
        }
        pa_log_debug("after: %lu", (unsigned long) out_frames);

        rewind_nbytes = (size_t) out_frames * u->frame_size;

        if (rewind_nbytes <= 0)
            pa_log_info("Tried rewind, but was apparently not possible.");
        else {
            u->frame_index -= out_frames;
            pa_log_debug("Rewound %lu bytes.", (unsigned long) rewind_nbytes);
            pa_sink_process_rewind(u->sink, rewind_nbytes);

            u->after_rewind = TRUE;
            return 0;
        }
    } else
        pa_log_debug("Mhmm, actually there is nothing to rewind.");

finish:

    pa_sink_process_rewind(u->sink, 0);

    return 0;

}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    unsigned short revents = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    for (;;) {
        int ret;

/*         pa_log_debug("loop"); */

        /* Render some data and write it to the dsp */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            int work_done;
            pa_usec_t sleep_usec = 0;

            if (u->sink->thread_info.rewind_requested)
                if (process_rewind(u) < 0)
                        goto fail;

            if (u->use_mmap)
                work_done = mmap_write(u, &sleep_usec, revents & POLLOUT);
            else
                work_done = unix_write(u, &sleep_usec, revents & POLLOUT);

            if (work_done < 0)
                goto fail;

/*             pa_log_debug("work_done = %i", work_done); */

            if (work_done) {

                if (u->first) {
                    pa_log_info("Starting playback.");
                    snd_pcm_start(u->pcm_handle);

                    pa_smoother_resume(u->smoother, pa_rtclock_usec());
                }

                update_smoother(u);
            }

            if (u->use_tsched) {
                pa_usec_t cusec;

                if (u->since_start <= u->hwbuf_size) {

                    /* USB devices on ALSA seem to hit a buffer
                     * underrun during the first iterations much
                     * quicker then we calculate here, probably due to
                     * the transport latency. To accomodate for that
                     * we artificially decrease the sleep time until
                     * we have filled the buffer at least once
                     * completely.*/

                    /*pa_log_debug("Cutting sleep time for the initial iterations by half.");*/
                    sleep_usec /= 2;
                }

                /* OK, the playback buffer is now full, let's
                 * calculate when to wake up next */
/*                 pa_log_debug("Waking up in %0.2fms (sound card clock).", (double) sleep_usec / PA_USEC_PER_MSEC); */

                /* Convert from the sound card time domain to the
                 * system time domain */
                cusec = pa_smoother_translate(u->smoother, pa_rtclock_usec(), sleep_usec);

/*                 pa_log_debug("Waking up in %0.2fms (system clock).", (double) cusec / PA_USEC_PER_MSEC); */

                /* We don't trust the conversion, so we wake up whatever comes first */
                pa_rtpoll_set_timer_relative(u->rtpoll, PA_MIN(sleep_usec, cusec));
            }

            u->first = FALSE;
            u->after_rewind = FALSE;

        } else if (u->use_tsched)

            /* OK, we're in an invalid state, let's disable our timers */
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        /* Tell ALSA about this and process its response */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            struct pollfd *pollfd;
            int err;
            unsigned n;

            pollfd = pa_rtpoll_item_get_pollfd(u->alsa_rtpoll_item, &n);

            if ((err = snd_pcm_poll_descriptors_revents(u->pcm_handle, pollfd, n, &revents)) < 0) {
                pa_log("snd_pcm_poll_descriptors_revents() failed: %s", snd_strerror(err));
                goto fail;
            }

            if (revents & (POLLIN|POLLERR|POLLNVAL|POLLHUP|POLLPRI)) {
                if (pa_alsa_recover_from_poll(u->pcm_handle, revents) < 0)
                    goto fail;

                u->first = TRUE;
                u->since_start = 0;
            }

            if (revents && u->use_tsched)
                pa_log_debug("Wakeup from ALSA!%s%s", (revents & POLLIN) ? " INPUT" : "", (revents & POLLOUT) ? " OUTPUT" : "");
        } else
            revents = 0;
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
    uint32_t nfrags, hwbuf_size, frag_size, tsched_size, tsched_watermark;
    snd_pcm_uframes_t period_frames, tsched_frames;
    size_t frame_size;
    snd_pcm_info_t *pcm_info = NULL;
    int err;
    const char *name;
    char *name_buf = NULL;
    pa_bool_t namereg_fail;
    pa_bool_t use_mmap = TRUE, b, use_tsched = TRUE, d;
    pa_usec_t usec;
    pa_sink_new_data data;

    snd_pcm_info_alloca(&pcm_info);

    pa_assert(m);

    pa_alsa_redirect_errors_inc();

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_ALSA) < 0) {
        pa_log("Failed to parse sample specification and channel map");
        goto fail;
    }

    frame_size = pa_frame_size(&ss);

    nfrags = m->core->default_n_fragments;
    frag_size = (uint32_t) pa_usec_to_bytes(m->core->default_fragment_size_msec*PA_USEC_PER_MSEC, &ss);
    if (frag_size <= 0)
        frag_size = (uint32_t) frame_size;
    tsched_size = (uint32_t) pa_usec_to_bytes(DEFAULT_TSCHED_BUFFER_USEC, &ss);
    tsched_watermark = (uint32_t) pa_usec_to_bytes(DEFAULT_TSCHED_WATERMARK_USEC, &ss);

    if (pa_modargs_get_value_u32(ma, "fragments", &nfrags) < 0 ||
        pa_modargs_get_value_u32(ma, "fragment_size", &frag_size) < 0 ||
        pa_modargs_get_value_u32(ma, "tsched_buffer_size", &tsched_size) < 0 ||
        pa_modargs_get_value_u32(ma, "tsched_buffer_watermark", &tsched_watermark) < 0) {
        pa_log("Failed to parse buffer metrics");
        goto fail;
    }

    hwbuf_size = frag_size * nfrags;
    period_frames = frag_size/frame_size;
    tsched_frames = tsched_size/frame_size;

    if (pa_modargs_get_value_boolean(ma, "mmap", &use_mmap) < 0) {
        pa_log("Failed to parse mmap argument.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "tsched", &use_tsched) < 0) {
        pa_log("Failed to parse tsched argument.");
        goto fail;
    }

    if (use_tsched && !pa_rtclock_hrtimer()) {
        pa_log_notice("Disabling timer-based scheduling because high-resolution timers are not available from the kernel.");
        use_tsched = FALSE;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->use_mmap = use_mmap;
    u->use_tsched = use_tsched;
    u->first = TRUE;
    u->since_start = 0;
    u->after_rewind = FALSE;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);
    u->alsa_rtpoll_item = NULL;

    u->smoother = pa_smoother_new(DEFAULT_TSCHED_BUFFER_USEC*2, DEFAULT_TSCHED_BUFFER_USEC*2, TRUE, 5);
    usec = pa_rtclock_usec();
    pa_smoother_set_time_offset(u->smoother, usec);
    pa_smoother_pause(u->smoother, usec);

    snd_config_update_free_global();

    b = use_mmap;
    d = use_tsched;

    if ((dev_id = pa_modargs_get_value(ma, "device_id", NULL))) {

        if (!(u->pcm_handle = pa_alsa_open_by_device_id(
                      dev_id,
                      &u->device_name,
                      &ss, &map,
                      SND_PCM_STREAM_PLAYBACK,
                      &nfrags, &period_frames, tsched_frames,
                      &b, &d)))

            goto fail;

    } else {

        if (!(u->pcm_handle = pa_alsa_open_by_device_string(
                      pa_modargs_get_value(ma, "device", DEFAULT_DEVICE),
                      &u->device_name,
                      &ss, &map,
                      SND_PCM_STREAM_PLAYBACK,
                      &nfrags, &period_frames, tsched_frames,
                      &b, &d)))
            goto fail;

    }

    pa_assert(u->device_name);
    pa_log_info("Successfully opened device %s.", u->device_name);

    if (use_mmap && !b) {
        pa_log_info("Device doesn't support mmap(), falling back to UNIX read/write mode.");
        u->use_mmap = use_mmap = FALSE;
    }

    if (use_tsched && (!b || !d)) {
        pa_log_info("Cannot enabled timer-based scheduling, falling back to sound IRQ scheduling.");
        u->use_tsched = use_tsched = FALSE;
    }

    if (u->use_mmap)
        pa_log_info("Successfully enabled mmap() mode.");

    if (u->use_tsched)
        pa_log_info("Successfully enabled timer-based scheduling mode.");

    if ((err = snd_pcm_info(u->pcm_handle, pcm_info)) < 0) {
        pa_log("Error fetching PCM info: %s", snd_strerror(err));
        goto fail;
    }

    /* ALSA might tweak the sample spec, so recalculate the frame size */
    frame_size = pa_frame_size(&ss);

    if ((err = snd_mixer_open(&u->mixer_handle, 0)) < 0)
        pa_log_warn("Error opening mixer: %s", snd_strerror(err));
    else {
        pa_bool_t found = FALSE;

        if (pa_alsa_prepare_mixer(u->mixer_handle, u->device_name) >= 0)
            found = TRUE;
        else {
            snd_pcm_info_t *info;

            snd_pcm_info_alloca(&info);

            if (snd_pcm_info(u->pcm_handle, info) >= 0) {
                char *md;
                int card;

                if ((card = snd_pcm_info_get_card(info)) >= 0) {

                    md = pa_sprintf_malloc("hw:%i", card);

                    if (strcmp(u->device_name, md))
                        if (pa_alsa_prepare_mixer(u->mixer_handle, md) >= 0)
                            found = TRUE;
                    pa_xfree(md);
                }
            }
        }

        if (found)
            if (!(u->mixer_elem = pa_alsa_find_elem(u->mixer_handle, "Master", "PCM")))
                found = FALSE;

        if (!found) {
            snd_mixer_close(u->mixer_handle);
            u->mixer_handle = NULL;
        }
    }

    if ((name = pa_modargs_get_value(ma, "sink_name", NULL)))
        namereg_fail = TRUE;
    else {
        name = name_buf = pa_sprintf_malloc("alsa_output.%s", u->device_name);
        namereg_fail = FALSE;
    }

    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, name);
    data.namereg_fail = namereg_fail;
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);

    pa_alsa_init_proplist(data.proplist, pcm_info);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->device_name);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_BUFFERING_BUFFER_SIZE, "%lu", (unsigned long) (period_frames * frame_size * nfrags));
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_BUFFERING_FRAGMENT_SIZE, "%lu", (unsigned long) (period_frames * frame_size));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_ACCESS_MODE, u->use_tsched ? "mmap+timer" : (u->use_mmap ? "mmap" : "serial"));

    u->sink = pa_sink_new(m->core, &data, PA_SINK_HARDWARE|PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);
    pa_xfree(name_buf);

    if (!u->sink) {
        pa_log("Failed to create sink object");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->frame_size = frame_size;
    u->fragment_size = frag_size = (uint32_t) (period_frames * frame_size);
    u->nfragments = nfrags;
    u->hwbuf_size = u->fragment_size * nfrags;
    u->hwbuf_unused_frames = 0;
    u->tsched_watermark = tsched_watermark;
    u->frame_index = 0;
    u->hw_dB_supported = FALSE;
    u->hw_dB_min = u->hw_dB_max = 0;
    u->hw_volume_min = u->hw_volume_max = 0;
    u->mixer_seperate_channels = FALSE;
    pa_cvolume_mute(&u->hardware_volume, u->sink->sample_spec.channels);

    if (use_tsched)
        fix_tsched_watermark(u);

    u->sink->thread_info.max_rewind = use_tsched ? u->hwbuf_size : 0;
    u->sink->thread_info.max_request = u->hwbuf_size;

    pa_sink_set_latency_range(u->sink,
                              !use_tsched ? pa_bytes_to_usec(u->hwbuf_size, &ss) : (pa_usec_t) -1,
                              pa_bytes_to_usec(u->hwbuf_size, &ss));

    pa_log_info("Using %u fragments of size %lu bytes, buffer time is %0.2fms",
                nfrags, (long unsigned) u->fragment_size,
                (double) pa_bytes_to_usec(u->hwbuf_size, &ss) / PA_USEC_PER_MSEC);

    if (use_tsched)
        pa_log_info("Time scheduling watermark is %0.2fms",
                    (double) pa_bytes_to_usec(u->tsched_watermark, &ss) / PA_USEC_PER_MSEC);

    if (update_sw_params(u) < 0)
        goto fail;

    pa_memchunk_reset(&u->memchunk);

    if (u->mixer_handle) {
        pa_assert(u->mixer_elem);

        if (snd_mixer_selem_has_playback_volume(u->mixer_elem)) {
            pa_bool_t suitable = FALSE;

            if (snd_mixer_selem_get_playback_volume_range(u->mixer_elem, &u->hw_volume_min, &u->hw_volume_max) < 0)
                pa_log_info("Failed to get volume range. Falling back to software volume control.");
            else if (u->hw_volume_min >= u->hw_volume_max)
                pa_log_warn("Your kernel driver is broken: it reports a volume range from %li to %li which makes no sense.", u->hw_volume_min, u->hw_volume_max);
            else {
                pa_log_info("Volume ranges from %li to %li.", u->hw_volume_min, u->hw_volume_max);
                suitable = TRUE;
            }

            if (snd_mixer_selem_get_playback_dB_range(u->mixer_elem, &u->hw_dB_min, &u->hw_dB_max) < 0)
                pa_log_info("Mixer doesn't support dB information.");
            else {
#ifdef HAVE_VALGRIND_MEMCHECK_H
                VALGRIND_MAKE_MEM_DEFINED(&u->hw_dB_min, sizeof(u->hw_dB_min));
                VALGRIND_MAKE_MEM_DEFINED(&u->hw_dB_max, sizeof(u->hw_dB_max));
#endif

                if (u->hw_dB_min >= u->hw_dB_max)
                    pa_log_warn("Your kernel driver is broken: it reports a volume range from %0.2f dB to %0.2f dB which makes no sense.", (double) u->hw_dB_min/100.0, (double) u->hw_dB_max/100.0);
                else {
                    pa_log_info("Volume ranges from %0.2f dB to %0.2f dB.", (double) u->hw_dB_min/100.0, (double) u->hw_dB_max/100.0);
                    u->hw_dB_supported = TRUE;
                }
            }

            if (suitable &&
                !u->hw_dB_supported &&
                u->hw_volume_max - u->hw_volume_min < 3) {

                pa_log_info("Device doesn't do dB volume and has less than 4 volume levels. Falling back to software volume control.");
                suitable = FALSE;
            }

            if (suitable) {
                u->mixer_seperate_channels = pa_alsa_calc_mixer_map(u->mixer_elem, &map, u->mixer_map, TRUE) >= 0;

                u->sink->get_volume = sink_get_volume_cb;
                u->sink->set_volume = sink_set_volume_cb;
                u->sink->flags |= PA_SINK_HW_VOLUME_CTRL | (u->hw_dB_supported ? PA_SINK_DECIBEL_VOLUME : 0);
                pa_log_info("Using hardware volume control. Hardware dB scale %s.", u->hw_dB_supported ? "supported" : "not supported");

            } else
                pa_log_info("Using software volume control.");
        }

        if (snd_mixer_selem_has_playback_switch(u->mixer_elem)) {
            u->sink->get_mute = sink_get_mute_cb;
            u->sink->set_mute = sink_set_mute_cb;
            u->sink->flags |= PA_SINK_HW_MUTE_CTRL;
        } else
            pa_log_info("Using software mute control.");

        u->mixer_fdl = pa_alsa_fdlist_new();

        if (pa_alsa_fdlist_set_mixer(u->mixer_fdl, u->mixer_handle, m->core->mainloop) < 0) {
            pa_log("Failed to initialize file descriptor monitoring");
            goto fail;
        }

        snd_mixer_elem_set_callback(u->mixer_elem, mixer_callback);
        snd_mixer_elem_set_callback_private(u->mixer_elem, u);
    } else
        u->mixer_fdl = NULL;

    pa_alsa_dump(u->pcm_handle);

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    /* Get initial mixer settings */
    if (data.volume_is_set) {
        if (u->sink->set_volume)
            u->sink->set_volume(u->sink);
    } else {
        if (u->sink->get_volume)
            u->sink->get_volume(u->sink);
    }

    if (data.muted_is_set) {
        if (u->sink->set_mute)
            u->sink->set_mute(u->sink);
    } else {
        if (u->sink->get_mute)
            u->sink->get_mute(u->sink);
    }

    pa_sink_put(u->sink);

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

    if (!(u = m->userdata)) {
        pa_alsa_redirect_errors_dec();
        return;
    }

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

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

    if (u->smoother)
        pa_smoother_free(u->smoother);

    pa_xfree(u->device_name);
    pa_xfree(u);

    snd_config_update_free_global();

    pa_alsa_redirect_errors_dec();
}
