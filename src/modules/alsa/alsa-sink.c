/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>
#include <pulse/internal.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/time-smoother.h>

#include <modules/reserve-wrap.h>

#include "alsa-util.h"
#include "alsa-sink.h"

/* #define DEBUG_TIMING */

#define DEFAULT_DEVICE "default"

#define DEFAULT_TSCHED_BUFFER_USEC (2*PA_USEC_PER_SEC)             /* 2s    -- Overall buffer size */
#define DEFAULT_TSCHED_WATERMARK_USEC (20*PA_USEC_PER_MSEC)        /* 20ms  -- Fill up when only this much is left in the buffer */

#define TSCHED_WATERMARK_INC_STEP_USEC (10*PA_USEC_PER_MSEC)       /* 10ms  -- On underrun, increase watermark by this */
#define TSCHED_WATERMARK_DEC_STEP_USEC (5*PA_USEC_PER_MSEC)        /* 5ms   -- When everything's great, decrease watermark by this */
#define TSCHED_WATERMARK_VERIFY_AFTER_USEC (20*PA_USEC_PER_SEC)    /* 20s   -- How long after a drop out recheck if things are good now */
#define TSCHED_WATERMARK_INC_THRESHOLD_USEC (0*PA_USEC_PER_MSEC)   /* 0ms   -- If the buffer level ever below this threshold, increase the watermark */
#define TSCHED_WATERMARK_DEC_THRESHOLD_USEC (100*PA_USEC_PER_MSEC) /* 100ms -- If the buffer level didn't drop below this threshold in the verification time, decrease the watermark */

/* Note that TSCHED_WATERMARK_INC_THRESHOLD_USEC == 0 means that we
 * will increase the watermark only if we hit a real underrun. */

#define TSCHED_MIN_SLEEP_USEC (10*PA_USEC_PER_MSEC)                /* 10ms  -- Sleep at least 10ms on each iteration */
#define TSCHED_MIN_WAKEUP_USEC (4*PA_USEC_PER_MSEC)                /* 4ms   -- Wakeup at least this long before the buffer runs empty*/

#define SMOOTHER_WINDOW_USEC  (10*PA_USEC_PER_SEC)                 /* 10s   -- smoother windows size */
#define SMOOTHER_ADJUST_USEC  (1*PA_USEC_PER_SEC)                  /* 1s    -- smoother adjust time */

#define SMOOTHER_MIN_INTERVAL (2*PA_USEC_PER_MSEC)                 /* 2ms   -- min smoother update interval */
#define SMOOTHER_MAX_INTERVAL (200*PA_USEC_PER_MSEC)               /* 200ms -- max smoother update interval */

#define VOLUME_ACCURACY (PA_VOLUME_NORM/100)  /* don't require volume adjustments to be perfectly correct. don't necessarily extend granularity in software unless the differences get greater than this level */

#define DEFAULT_REWIND_SAFEGUARD_BYTES (256U) /* 1.33ms @48kHz, we'll never rewind less than this */
#define DEFAULT_REWIND_SAFEGUARD_USEC (1330) /* 1.33ms, depending on channels/rate/sample we may rewind more than 256 above */

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    snd_pcm_t *pcm_handle;

    char *paths_dir;
    pa_alsa_fdlist *mixer_fdl;
    pa_alsa_mixer_pdata *mixer_pd;
    snd_mixer_t *mixer_handle;
    pa_alsa_path_set *mixer_path_set;
    pa_alsa_path *mixer_path;

    pa_cvolume hardware_volume;

    unsigned int *rates;

    size_t
        frame_size,
        fragment_size,
        hwbuf_size,
        tsched_watermark,
        tsched_watermark_ref,
        hwbuf_unused,
        min_sleep,
        min_wakeup,
        watermark_inc_step,
        watermark_dec_step,
        watermark_inc_threshold,
        watermark_dec_threshold,
        rewind_safeguard;

    pa_usec_t watermark_dec_not_before;
    pa_usec_t min_latency_ref;

    pa_memchunk memchunk;

    char *device_name;  /* name of the PCM device */
    char *control_device; /* name of the control device */

    pa_bool_t use_mmap:1, use_tsched:1, deferred_volume:1, fixed_latency_range:1;

    pa_bool_t first, after_rewind;

    pa_rtpoll_item *alsa_rtpoll_item;

    pa_smoother *smoother;
    uint64_t write_count;
    uint64_t since_start;
    pa_usec_t smoother_interval;
    pa_usec_t last_smoother_update;

    pa_idxset *formats;

    pa_reserve_wrapper *reserve;
    pa_hook_slot *reserve_slot;
    pa_reserve_monitor_wrapper *monitor;
    pa_hook_slot *monitor_slot;
};

static void userdata_free(struct userdata *u);

/* FIXME: Is there a better way to do this than device names? */
static pa_bool_t is_iec958(struct userdata *u) {
    return (strncmp("iec958", u->device_name, 6) == 0);
}

static pa_bool_t is_hdmi(struct userdata *u) {
    return (strncmp("hdmi", u->device_name, 4) == 0);
}

static pa_hook_result_t reserve_cb(pa_reserve_wrapper *r, void *forced, struct userdata *u) {
    pa_assert(r);
    pa_assert(u);

    if (pa_sink_suspend(u->sink, TRUE, PA_SUSPEND_APPLICATION) < 0)
        return PA_HOOK_CANCEL;

    return PA_HOOK_OK;
}

static void reserve_done(struct userdata *u) {
    pa_assert(u);

    if (u->reserve_slot) {
        pa_hook_slot_free(u->reserve_slot);
        u->reserve_slot = NULL;
    }

    if (u->reserve) {
        pa_reserve_wrapper_unref(u->reserve);
        u->reserve = NULL;
    }
}

static void reserve_update(struct userdata *u) {
    const char *description;
    pa_assert(u);

    if (!u->sink || !u->reserve)
        return;

    if ((description = pa_proplist_gets(u->sink->proplist, PA_PROP_DEVICE_DESCRIPTION)))
        pa_reserve_wrapper_set_application_device_name(u->reserve, description);
}

static int reserve_init(struct userdata *u, const char *dname) {
    char *rname;

    pa_assert(u);
    pa_assert(dname);

    if (u->reserve)
        return 0;

    if (pa_in_system_mode())
        return 0;

    if (!(rname = pa_alsa_get_reserve_name(dname)))
        return 0;

    /* We are resuming, try to lock the device */
    u->reserve = pa_reserve_wrapper_get(u->core, rname);
    pa_xfree(rname);

    if (!(u->reserve))
        return -1;

    reserve_update(u);

    pa_assert(!u->reserve_slot);
    u->reserve_slot = pa_hook_connect(pa_reserve_wrapper_hook(u->reserve), PA_HOOK_NORMAL, (pa_hook_cb_t) reserve_cb, u);

    return 0;
}

static pa_hook_result_t monitor_cb(pa_reserve_monitor_wrapper *w, void* busy, struct userdata *u) {
    pa_bool_t b;

    pa_assert(w);
    pa_assert(u);

    b = PA_PTR_TO_UINT(busy) && !u->reserve;

    pa_sink_suspend(u->sink, b, PA_SUSPEND_APPLICATION);
    return PA_HOOK_OK;
}

static void monitor_done(struct userdata *u) {
    pa_assert(u);

    if (u->monitor_slot) {
        pa_hook_slot_free(u->monitor_slot);
        u->monitor_slot = NULL;
    }

    if (u->monitor) {
        pa_reserve_monitor_wrapper_unref(u->monitor);
        u->monitor = NULL;
    }
}

static int reserve_monitor_init(struct userdata *u, const char *dname) {
    char *rname;

    pa_assert(u);
    pa_assert(dname);

    if (pa_in_system_mode())
        return 0;

    if (!(rname = pa_alsa_get_reserve_name(dname)))
        return 0;

    /* We are resuming, try to lock the device */
    u->monitor = pa_reserve_monitor_wrapper_get(u->core, rname);
    pa_xfree(rname);

    if (!(u->monitor))
        return -1;

    pa_assert(!u->monitor_slot);
    u->monitor_slot = pa_hook_connect(pa_reserve_monitor_wrapper_hook(u->monitor), PA_HOOK_NORMAL, (pa_hook_cb_t) monitor_cb, u);

    return 0;
}

static void fix_min_sleep_wakeup(struct userdata *u) {
    size_t max_use, max_use_2;

    pa_assert(u);
    pa_assert(u->use_tsched);

    max_use = u->hwbuf_size - u->hwbuf_unused;
    max_use_2 = pa_frame_align(max_use/2, &u->sink->sample_spec);

    u->min_sleep = pa_usec_to_bytes(TSCHED_MIN_SLEEP_USEC, &u->sink->sample_spec);
    u->min_sleep = PA_CLAMP(u->min_sleep, u->frame_size, max_use_2);

    u->min_wakeup = pa_usec_to_bytes(TSCHED_MIN_WAKEUP_USEC, &u->sink->sample_spec);
    u->min_wakeup = PA_CLAMP(u->min_wakeup, u->frame_size, max_use_2);
}

static void fix_tsched_watermark(struct userdata *u) {
    size_t max_use;
    pa_assert(u);
    pa_assert(u->use_tsched);

    max_use = u->hwbuf_size - u->hwbuf_unused;

    if (u->tsched_watermark > max_use - u->min_sleep)
        u->tsched_watermark = max_use - u->min_sleep;

    if (u->tsched_watermark < u->min_wakeup)
        u->tsched_watermark = u->min_wakeup;
}

static void increase_watermark(struct userdata *u) {
    size_t old_watermark;
    pa_usec_t old_min_latency, new_min_latency;

    pa_assert(u);
    pa_assert(u->use_tsched);

    /* First, just try to increase the watermark */
    old_watermark = u->tsched_watermark;
    u->tsched_watermark = PA_MIN(u->tsched_watermark * 2, u->tsched_watermark + u->watermark_inc_step);
    fix_tsched_watermark(u);

    if (old_watermark != u->tsched_watermark) {
        pa_log_info("Increasing wakeup watermark to %0.2f ms",
                    (double) pa_bytes_to_usec(u->tsched_watermark, &u->sink->sample_spec) / PA_USEC_PER_MSEC);
        return;
    }

    /* Hmm, we cannot increase the watermark any further, hence let's
       raise the latency, unless doing so was disabled in
       configuration */
    if (u->fixed_latency_range)
        return;

    old_min_latency = u->sink->thread_info.min_latency;
    new_min_latency = PA_MIN(old_min_latency * 2, old_min_latency + TSCHED_WATERMARK_INC_STEP_USEC);
    new_min_latency = PA_MIN(new_min_latency, u->sink->thread_info.max_latency);

    if (old_min_latency != new_min_latency) {
        pa_log_info("Increasing minimal latency to %0.2f ms",
                    (double) new_min_latency / PA_USEC_PER_MSEC);

        pa_sink_set_latency_range_within_thread(u->sink, new_min_latency, u->sink->thread_info.max_latency);
    }

    /* When we reach this we're officialy fucked! */
}

static void decrease_watermark(struct userdata *u) {
    size_t old_watermark;
    pa_usec_t now;

    pa_assert(u);
    pa_assert(u->use_tsched);

    now = pa_rtclock_now();

    if (u->watermark_dec_not_before <= 0)
        goto restart;

    if (u->watermark_dec_not_before > now)
        return;

    old_watermark = u->tsched_watermark;

    if (u->tsched_watermark < u->watermark_dec_step)
        u->tsched_watermark = u->tsched_watermark / 2;
    else
        u->tsched_watermark = PA_MAX(u->tsched_watermark / 2, u->tsched_watermark - u->watermark_dec_step);

    fix_tsched_watermark(u);

    if (old_watermark != u->tsched_watermark)
        pa_log_info("Decreasing wakeup watermark to %0.2f ms",
                    (double) pa_bytes_to_usec(u->tsched_watermark, &u->sink->sample_spec) / PA_USEC_PER_MSEC);

    /* We don't change the latency range*/

restart:
    u->watermark_dec_not_before = now + TSCHED_WATERMARK_VERIFY_AFTER_USEC;
}

static void hw_sleep_time(struct userdata *u, pa_usec_t *sleep_usec, pa_usec_t*process_usec) {
    pa_usec_t usec, wm;

    pa_assert(sleep_usec);
    pa_assert(process_usec);

    pa_assert(u);
    pa_assert(u->use_tsched);

    usec = pa_sink_get_requested_latency_within_thread(u->sink);

    if (usec == (pa_usec_t) -1)
        usec = pa_bytes_to_usec(u->hwbuf_size, &u->sink->sample_spec);

    wm = pa_bytes_to_usec(u->tsched_watermark, &u->sink->sample_spec);

    if (wm > usec)
        wm = usec/2;

    *sleep_usec = usec - wm;
    *process_usec = wm;

#ifdef DEBUG_TIMING
    pa_log_debug("Buffer time: %lu ms; Sleep time: %lu ms; Process time: %lu ms",
                 (unsigned long) (usec / PA_USEC_PER_MSEC),
                 (unsigned long) (*sleep_usec / PA_USEC_PER_MSEC),
                 (unsigned long) (*process_usec / PA_USEC_PER_MSEC));
#endif
}

static int try_recover(struct userdata *u, const char *call, int err) {
    pa_assert(u);
    pa_assert(call);
    pa_assert(err < 0);

    pa_log_debug("%s: %s", call, pa_alsa_strerror(err));

    pa_assert(err != -EAGAIN);

    if (err == -EPIPE)
        pa_log_debug("%s: Buffer underrun!", call);

    if (err == -ESTRPIPE)
        pa_log_debug("%s: System suspended!", call);

    if ((err = snd_pcm_recover(u->pcm_handle, err, 1)) < 0) {
        pa_log("%s: %s", call, pa_alsa_strerror(err));
        return -1;
    }

    u->first = TRUE;
    u->since_start = 0;
    return 0;
}

static size_t check_left_to_play(struct userdata *u, size_t n_bytes, pa_bool_t on_timeout) {
    size_t left_to_play;
    pa_bool_t underrun = FALSE;

    /* We use <= instead of < for this check here because an underrun
     * only happens after the last sample was processed, not already when
     * it is removed from the buffer. This is particularly important
     * when block transfer is used. */

    if (n_bytes <= u->hwbuf_size)
        left_to_play = u->hwbuf_size - n_bytes;
    else {

        /* We got a dropout. What a mess! */
        left_to_play = 0;
        underrun = TRUE;

#if 0
        PA_DEBUG_TRAP;
#endif

        if (!u->first && !u->after_rewind)
            if (pa_log_ratelimit(PA_LOG_INFO))
                pa_log_info("Underrun!");
    }

#ifdef DEBUG_TIMING
    pa_log_debug("%0.2f ms left to play; inc threshold = %0.2f ms; dec threshold = %0.2f ms",
                 (double) pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) / PA_USEC_PER_MSEC,
                 (double) pa_bytes_to_usec(u->watermark_inc_threshold, &u->sink->sample_spec) / PA_USEC_PER_MSEC,
                 (double) pa_bytes_to_usec(u->watermark_dec_threshold, &u->sink->sample_spec) / PA_USEC_PER_MSEC);
#endif

    if (u->use_tsched) {
        pa_bool_t reset_not_before = TRUE;

        if (!u->first && !u->after_rewind) {
            if (underrun || left_to_play < u->watermark_inc_threshold)
                increase_watermark(u);
            else if (left_to_play > u->watermark_dec_threshold) {
                reset_not_before = FALSE;

                /* We decrease the watermark only if have actually
                 * been woken up by a timeout. If something else woke
                 * us up it's too easy to fulfill the deadlines... */

                if (on_timeout)
                    decrease_watermark(u);
            }
        }

        if (reset_not_before)
            u->watermark_dec_not_before = 0;
    }

    return left_to_play;
}

static int mmap_write(struct userdata *u, pa_usec_t *sleep_usec, pa_bool_t polled, pa_bool_t on_timeout) {
    pa_bool_t work_done = FALSE;
    pa_usec_t max_sleep_usec = 0, process_usec = 0;
    size_t left_to_play;
    unsigned j = 0;

    pa_assert(u);
    pa_sink_assert_ref(u->sink);

    if (u->use_tsched)
        hw_sleep_time(u, &max_sleep_usec, &process_usec);

    for (;;) {
        snd_pcm_sframes_t n;
        size_t n_bytes;
        int r;
        pa_bool_t after_avail = TRUE;

        /* First we determine how many samples are missing to fill the
         * buffer up to 100% */

        if (PA_UNLIKELY((n = pa_alsa_safe_avail(u->pcm_handle, u->hwbuf_size, &u->sink->sample_spec)) < 0)) {

            if ((r = try_recover(u, "snd_pcm_avail", (int) n)) == 0)
                continue;

            return r;
        }

        n_bytes = (size_t) n * u->frame_size;

#ifdef DEBUG_TIMING
        pa_log_debug("avail: %lu", (unsigned long) n_bytes);
#endif

        left_to_play = check_left_to_play(u, n_bytes, on_timeout);
        on_timeout = FALSE;

        if (u->use_tsched)

            /* We won't fill up the playback buffer before at least
            * half the sleep time is over because otherwise we might
            * ask for more data from the clients then they expect. We
            * need to guarantee that clients only have to keep around
            * a single hw buffer length. */

            if (!polled &&
                pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) > process_usec+max_sleep_usec/2) {
#ifdef DEBUG_TIMING
                pa_log_debug("Not filling up, because too early.");
#endif
                break;
            }

        if (PA_UNLIKELY(n_bytes <= u->hwbuf_unused)) {

            if (polled)
                PA_ONCE_BEGIN {
                    char *dn = pa_alsa_get_driver_name_by_pcm(u->pcm_handle);
                    pa_log(_("ALSA woke us up to write new data to the device, but there was actually nothing to write!\n"
                             "Most likely this is a bug in the ALSA driver '%s'. Please report this issue to the ALSA developers.\n"
                             "We were woken up with POLLOUT set -- however a subsequent snd_pcm_avail() returned 0 or another value < min_avail."),
                           pa_strnull(dn));
                    pa_xfree(dn);
                } PA_ONCE_END;

#ifdef DEBUG_TIMING
            pa_log_debug("Not filling up, because not necessary.");
#endif
            break;
        }


        if (++j > 10) {
#ifdef DEBUG_TIMING
            pa_log_debug("Not filling up, because already too many iterations.");
#endif

            break;
        }

        n_bytes -= u->hwbuf_unused;
        polled = FALSE;

#ifdef DEBUG_TIMING
        pa_log_debug("Filling up");
#endif

        for (;;) {
            pa_memchunk chunk;
            void *p;
            int err;
            const snd_pcm_channel_area_t *areas;
            snd_pcm_uframes_t offset, frames;
            snd_pcm_sframes_t sframes;

            frames = (snd_pcm_uframes_t) (n_bytes / u->frame_size);
/*             pa_log_debug("%lu frames to write", (unsigned long) frames); */

            if (PA_UNLIKELY((err = pa_alsa_safe_mmap_begin(u->pcm_handle, &areas, &offset, &frames, u->hwbuf_size, &u->sink->sample_spec)) < 0)) {

                if (!after_avail && err == -EAGAIN)
                    break;

                if ((r = try_recover(u, "snd_pcm_mmap_begin", err)) == 0)
                    continue;

                return r;
            }

            /* Make sure that if these memblocks need to be copied they will fit into one slot */
            if (frames > pa_mempool_block_size_max(u->core->mempool)/u->frame_size)
                frames = pa_mempool_block_size_max(u->core->mempool)/u->frame_size;

            if (!after_avail && frames == 0)
                break;

            pa_assert(frames > 0);
            after_avail = FALSE;

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
            pa_memblock_unref_fixed(chunk.memblock);

            if (PA_UNLIKELY((sframes = snd_pcm_mmap_commit(u->pcm_handle, offset, frames)) < 0)) {

                if (!after_avail && (int) sframes == -EAGAIN)
                    break;

                if ((r = try_recover(u, "snd_pcm_mmap_commit", (int) sframes)) == 0)
                    continue;

                return r;
            }

            work_done = TRUE;

            u->write_count += frames * u->frame_size;
            u->since_start += frames * u->frame_size;

#ifdef DEBUG_TIMING
            pa_log_debug("Wrote %lu bytes (of possible %lu bytes)", (unsigned long) (frames * u->frame_size), (unsigned long) n_bytes);
#endif

            if ((size_t) frames * u->frame_size >= n_bytes)
                break;

            n_bytes -= (size_t) frames * u->frame_size;
        }
    }

    if (u->use_tsched) {
        *sleep_usec = pa_bytes_to_usec(left_to_play, &u->sink->sample_spec);
        process_usec = pa_bytes_to_usec(u->tsched_watermark, &u->sink->sample_spec);

        if (*sleep_usec > process_usec)
            *sleep_usec -= process_usec;
        else
            *sleep_usec = 0;
    } else
        *sleep_usec = 0;

    return work_done ? 1 : 0;
}

static int unix_write(struct userdata *u, pa_usec_t *sleep_usec, pa_bool_t polled, pa_bool_t on_timeout) {
    pa_bool_t work_done = FALSE;
    pa_usec_t max_sleep_usec = 0, process_usec = 0;
    size_t left_to_play;
    unsigned j = 0;

    pa_assert(u);
    pa_sink_assert_ref(u->sink);

    if (u->use_tsched)
        hw_sleep_time(u, &max_sleep_usec, &process_usec);

    for (;;) {
        snd_pcm_sframes_t n;
        size_t n_bytes;
        int r;
        pa_bool_t after_avail = TRUE;

        if (PA_UNLIKELY((n = pa_alsa_safe_avail(u->pcm_handle, u->hwbuf_size, &u->sink->sample_spec)) < 0)) {

            if ((r = try_recover(u, "snd_pcm_avail", (int) n)) == 0)
                continue;

            return r;
        }

        n_bytes = (size_t) n * u->frame_size;
        left_to_play = check_left_to_play(u, n_bytes, on_timeout);
        on_timeout = FALSE;

        if (u->use_tsched)

            /* We won't fill up the playback buffer before at least
            * half the sleep time is over because otherwise we might
            * ask for more data from the clients then they expect. We
            * need to guarantee that clients only have to keep around
            * a single hw buffer length. */

            if (!polled &&
                pa_bytes_to_usec(left_to_play, &u->sink->sample_spec) > process_usec+max_sleep_usec/2)
                break;

        if (PA_UNLIKELY(n_bytes <= u->hwbuf_unused)) {

            if (polled)
                PA_ONCE_BEGIN {
                    char *dn = pa_alsa_get_driver_name_by_pcm(u->pcm_handle);
                    pa_log(_("ALSA woke us up to write new data to the device, but there was actually nothing to write!\n"
                             "Most likely this is a bug in the ALSA driver '%s'. Please report this issue to the ALSA developers.\n"
                             "We were woken up with POLLOUT set -- however a subsequent snd_pcm_avail() returned 0 or another value < min_avail."),
                           pa_strnull(dn));
                    pa_xfree(dn);
                } PA_ONCE_END;

            break;
        }

        if (++j > 10) {
#ifdef DEBUG_TIMING
            pa_log_debug("Not filling up, because already too many iterations.");
#endif

            break;
        }

        n_bytes -= u->hwbuf_unused;
        polled = FALSE;

        for (;;) {
            snd_pcm_sframes_t frames;
            void *p;

/*         pa_log_debug("%lu frames to write", (unsigned long) frames); */

            if (u->memchunk.length <= 0)
                pa_sink_render(u->sink, n_bytes, &u->memchunk);

            pa_assert(u->memchunk.length > 0);

            frames = (snd_pcm_sframes_t) (u->memchunk.length / u->frame_size);

            if (frames > (snd_pcm_sframes_t) (n_bytes/u->frame_size))
                frames = (snd_pcm_sframes_t) (n_bytes/u->frame_size);

            p = pa_memblock_acquire(u->memchunk.memblock);
            frames = snd_pcm_writei(u->pcm_handle, (const uint8_t*) p + u->memchunk.index, (snd_pcm_uframes_t) frames);
            pa_memblock_release(u->memchunk.memblock);

            if (PA_UNLIKELY(frames < 0)) {

                if (!after_avail && (int) frames == -EAGAIN)
                    break;

                if ((r = try_recover(u, "snd_pcm_writei", (int) frames)) == 0)
                    continue;

                return r;
            }

            if (!after_avail && frames == 0)
                break;

            pa_assert(frames > 0);
            after_avail = FALSE;

            u->memchunk.index += (size_t) frames * u->frame_size;
            u->memchunk.length -= (size_t) frames * u->frame_size;

            if (u->memchunk.length <= 0) {
                pa_memblock_unref(u->memchunk.memblock);
                pa_memchunk_reset(&u->memchunk);
            }

            work_done = TRUE;

            u->write_count += frames * u->frame_size;
            u->since_start += frames * u->frame_size;

/*         pa_log_debug("wrote %lu frames", (unsigned long) frames); */

            if ((size_t) frames * u->frame_size >= n_bytes)
                break;

            n_bytes -= (size_t) frames * u->frame_size;
        }
    }

    if (u->use_tsched) {
        *sleep_usec = pa_bytes_to_usec(left_to_play, &u->sink->sample_spec);
        process_usec = pa_bytes_to_usec(u->tsched_watermark, &u->sink->sample_spec);

        if (*sleep_usec > process_usec)
            *sleep_usec -= process_usec;
        else
            *sleep_usec = 0;
    } else
        *sleep_usec = 0;

    return work_done ? 1 : 0;
}

static void update_smoother(struct userdata *u) {
    snd_pcm_sframes_t delay = 0;
    int64_t position;
    int err;
    pa_usec_t now1 = 0, now2;
    snd_pcm_status_t *status;

    snd_pcm_status_alloca(&status);

    pa_assert(u);
    pa_assert(u->pcm_handle);

    /* Let's update the time smoother */

    if (PA_UNLIKELY((err = pa_alsa_safe_delay(u->pcm_handle, &delay, u->hwbuf_size, &u->sink->sample_spec, FALSE)) < 0)) {
        pa_log_warn("Failed to query DSP status data: %s", pa_alsa_strerror(err));
        return;
    }

    if (PA_UNLIKELY((err = snd_pcm_status(u->pcm_handle, status)) < 0))
        pa_log_warn("Failed to get timestamp: %s", pa_alsa_strerror(err));
    else {
        snd_htimestamp_t htstamp = { 0, 0 };
        snd_pcm_status_get_htstamp(status, &htstamp);
        now1 = pa_timespec_load(&htstamp);
    }

    /* Hmm, if the timestamp is 0, then it wasn't set and we take the current time */
    if (now1 <= 0)
        now1 = pa_rtclock_now();

    /* check if the time since the last update is bigger than the interval */
    if (u->last_smoother_update > 0)
        if (u->last_smoother_update + u->smoother_interval > now1)
            return;

    position = (int64_t) u->write_count - ((int64_t) delay * (int64_t) u->frame_size);

    if (PA_UNLIKELY(position < 0))
        position = 0;

    now2 = pa_bytes_to_usec((uint64_t) position, &u->sink->sample_spec);

    pa_smoother_put(u->smoother, now1, now2);

    u->last_smoother_update = now1;
    /* exponentially increase the update interval up to the MAX limit */
    u->smoother_interval = PA_MIN (u->smoother_interval * 2, SMOOTHER_MAX_INTERVAL);
}

static pa_usec_t sink_get_latency(struct userdata *u) {
    pa_usec_t r;
    int64_t delay;
    pa_usec_t now1, now2;

    pa_assert(u);

    now1 = pa_rtclock_now();
    now2 = pa_smoother_get(u->smoother, now1);

    delay = (int64_t) pa_bytes_to_usec(u->write_count, &u->sink->sample_spec) - (int64_t) now2;

    r = delay >= 0 ? (pa_usec_t) delay : 0;

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

/* Called from IO context */
static int suspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->pcm_handle);

    pa_smoother_pause(u->smoother, pa_rtclock_now());

    /* Let's suspend -- we don't call snd_pcm_drain() here since that might
     * take awfully long with our long buffer sizes today. */
    snd_pcm_close(u->pcm_handle);
    u->pcm_handle = NULL;

    if (u->alsa_rtpoll_item) {
        pa_rtpoll_item_free(u->alsa_rtpoll_item);
        u->alsa_rtpoll_item = NULL;
    }

    /* We reset max_rewind/max_request here to make sure that while we
     * are suspended the old max_request/max_rewind values set before
     * the suspend can influence the per-stream buffer of newly
     * created streams, without their requirements having any
     * influence on them. */
    pa_sink_set_max_rewind_within_thread(u->sink, 0);
    pa_sink_set_max_request_within_thread(u->sink, 0);

    pa_log_info("Device suspended...");

    return 0;
}

/* Called from IO context */
static int update_sw_params(struct userdata *u) {
    snd_pcm_uframes_t avail_min;
    int err;

    pa_assert(u);

    /* Use the full buffer if no one asked us for anything specific */
    u->hwbuf_unused = 0;

    if (u->use_tsched) {
        pa_usec_t latency;

        if ((latency = pa_sink_get_requested_latency_within_thread(u->sink)) != (pa_usec_t) -1) {
            size_t b;

            pa_log_debug("Latency set to %0.2fms", (double) latency / PA_USEC_PER_MSEC);

            b = pa_usec_to_bytes(latency, &u->sink->sample_spec);

            /* We need at least one sample in our buffer */

            if (PA_UNLIKELY(b < u->frame_size))
                b = u->frame_size;

            u->hwbuf_unused = PA_LIKELY(b < u->hwbuf_size) ? (u->hwbuf_size - b) : 0;
        }

        fix_min_sleep_wakeup(u);
        fix_tsched_watermark(u);
    }

    pa_log_debug("hwbuf_unused=%lu", (unsigned long) u->hwbuf_unused);

    /* We need at last one frame in the used part of the buffer */
    avail_min = (snd_pcm_uframes_t) u->hwbuf_unused / u->frame_size + 1;

    if (u->use_tsched) {
        pa_usec_t sleep_usec, process_usec;

        hw_sleep_time(u, &sleep_usec, &process_usec);
        avail_min += pa_usec_to_bytes(sleep_usec, &u->sink->sample_spec) / u->frame_size;
    }

    pa_log_debug("setting avail_min=%lu", (unsigned long) avail_min);

    if ((err = pa_alsa_set_sw_params(u->pcm_handle, avail_min, !u->use_tsched)) < 0) {
        pa_log("Failed to set software parameters: %s", pa_alsa_strerror(err));
        return err;
    }

    pa_sink_set_max_request_within_thread(u->sink, u->hwbuf_size - u->hwbuf_unused);
     if (pa_alsa_pcm_is_hw(u->pcm_handle))
         pa_sink_set_max_rewind_within_thread(u->sink, u->hwbuf_size);
    else {
        pa_log_info("Disabling rewind_within_thread for device %s", u->device_name);
        pa_sink_set_max_rewind_within_thread(u->sink, 0);
    }

    return 0;
}

/* Called from IO Context on unsuspend or from main thread when creating sink */
static void reset_watermark(struct userdata *u, size_t tsched_watermark, pa_sample_spec *ss,
                            pa_bool_t in_thread)
{
    u->tsched_watermark = pa_usec_to_bytes_round_up(pa_bytes_to_usec_round_up(tsched_watermark, ss),
                                                    &u->sink->sample_spec);

    u->watermark_inc_step = pa_usec_to_bytes(TSCHED_WATERMARK_INC_STEP_USEC, &u->sink->sample_spec);
    u->watermark_dec_step = pa_usec_to_bytes(TSCHED_WATERMARK_DEC_STEP_USEC, &u->sink->sample_spec);

    u->watermark_inc_threshold = pa_usec_to_bytes_round_up(TSCHED_WATERMARK_INC_THRESHOLD_USEC, &u->sink->sample_spec);
    u->watermark_dec_threshold = pa_usec_to_bytes_round_up(TSCHED_WATERMARK_DEC_THRESHOLD_USEC, &u->sink->sample_spec);

    fix_min_sleep_wakeup(u);
    fix_tsched_watermark(u);

    if (in_thread)
        pa_sink_set_latency_range_within_thread(u->sink,
                                                u->min_latency_ref,
                                                pa_bytes_to_usec(u->hwbuf_size, ss));
    else {
        pa_sink_set_latency_range(u->sink,
                                  0,
                                  pa_bytes_to_usec(u->hwbuf_size, ss));

        /* work-around assert in pa_sink_set_latency_within_thead,
           keep track of min_latency and reuse it when
           this routine is called from IO context */
        u->min_latency_ref = u->sink->thread_info.min_latency;
    }

    pa_log_info("Time scheduling watermark is %0.2fms",
                (double) pa_bytes_to_usec(u->tsched_watermark, ss) / PA_USEC_PER_MSEC);
}

/* Called from IO context */
static int unsuspend(struct userdata *u) {
    pa_sample_spec ss;
    int err;
    pa_bool_t b, d;
    snd_pcm_uframes_t period_size, buffer_size;
    char *device_name = NULL;

    pa_assert(u);
    pa_assert(!u->pcm_handle);

    pa_log_info("Trying resume...");

    if ((is_iec958(u) || is_hdmi(u)) && pa_sink_is_passthrough(u->sink)) {
        /* Need to open device in NONAUDIO mode */
        int len = strlen(u->device_name) + 8;

        device_name = pa_xmalloc(len);
        pa_snprintf(device_name, len, "%s,AES0=6", u->device_name);
    }

    if ((err = snd_pcm_open(&u->pcm_handle, device_name ? device_name : u->device_name, SND_PCM_STREAM_PLAYBACK,
                            SND_PCM_NONBLOCK|
                            SND_PCM_NO_AUTO_RESAMPLE|
                            SND_PCM_NO_AUTO_CHANNELS|
                            SND_PCM_NO_AUTO_FORMAT)) < 0) {
        pa_log("Error opening PCM device %s: %s", u->device_name, pa_alsa_strerror(err));
        goto fail;
    }

    ss = u->sink->sample_spec;
    period_size = u->fragment_size / u->frame_size;
    buffer_size = u->hwbuf_size / u->frame_size;
    b = u->use_mmap;
    d = u->use_tsched;

    if ((err = pa_alsa_set_hw_params(u->pcm_handle, &ss, &period_size, &buffer_size, 0, &b, &d, TRUE)) < 0) {
        pa_log("Failed to set hardware parameters: %s", pa_alsa_strerror(err));
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

    if (period_size*u->frame_size != u->fragment_size ||
        buffer_size*u->frame_size != u->hwbuf_size) {
        pa_log_warn("Resume failed, couldn't restore original fragment settings. (Old: %lu/%lu, New %lu/%lu)",
                    (unsigned long) u->hwbuf_size, (unsigned long) u->fragment_size,
                    (unsigned long) (buffer_size*u->frame_size), (unsigned long) (period_size*u->frame_size));
        goto fail;
    }

    if (update_sw_params(u) < 0)
        goto fail;

    if (build_pollfd(u) < 0)
        goto fail;

    u->write_count = 0;
    pa_smoother_reset(u->smoother, pa_rtclock_now(), TRUE);
    u->smoother_interval = SMOOTHER_MIN_INTERVAL;
    u->last_smoother_update = 0;

    u->first = TRUE;
    u->since_start = 0;

    /* reset the watermark to the value defined when sink was created */
    if (u->use_tsched)
        reset_watermark(u, u->tsched_watermark_ref, &u->sink->sample_spec, TRUE);

    pa_log_info("Resumed successfully...");

    pa_xfree(device_name);
    return 0;

fail:
    if (u->pcm_handle) {
        snd_pcm_close(u->pcm_handle);
        u->pcm_handle = NULL;
    }

    pa_xfree(device_name);

    return -PA_ERR_IO;
}

/* Called from IO context */
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

                case PA_SINK_SUSPENDED: {
                    int r;

                    pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

                    if ((r = suspend(u)) < 0)
                        return r;

                    break;
                }

                case PA_SINK_IDLE:
                case PA_SINK_RUNNING: {
                    int r;

                    if (u->sink->thread_info.state == PA_SINK_INIT) {
                        if (build_pollfd(u) < 0)
                            return -PA_ERR_IO;
                    }

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                        if ((r = unsuspend(u)) < 0)
                            return r;
                    }

                    break;
                }

                case PA_SINK_UNLINKED:
                case PA_SINK_INIT:
                case PA_SINK_INVALID_STATE:
                    ;
            }

            break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_cb(pa_sink *s, pa_sink_state_t new_state) {
    pa_sink_state_t old_state;
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    old_state = pa_sink_get_state(u->sink);

    if (PA_SINK_IS_OPENED(old_state) && new_state == PA_SINK_SUSPENDED)
        reserve_done(u);
    else if (old_state == PA_SINK_SUSPENDED && PA_SINK_IS_OPENED(new_state))
        if (reserve_init(u, u->device_name) < 0)
            return -PA_ERR_BUSY;

    return 0;
}

static int ctl_mixer_callback(snd_mixer_elem_t *elem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(elem);

    pa_assert(u);
    pa_assert(u->mixer_handle);

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    if (!PA_SINK_IS_LINKED(u->sink->state))
        return 0;

    if (u->sink->suspend_cause & PA_SUSPEND_SESSION) {
        pa_sink_set_mixer_dirty(u->sink, TRUE);
        return 0;
    }

    if (mask & SND_CTL_EVENT_MASK_VALUE) {
        pa_sink_get_volume(u->sink, TRUE);
        pa_sink_get_mute(u->sink, TRUE);
    }

    return 0;
}

static int io_mixer_callback(snd_mixer_elem_t *elem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(elem);

    pa_assert(u);
    pa_assert(u->mixer_handle);

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    if (u->sink->suspend_cause & PA_SUSPEND_SESSION) {
        pa_sink_set_mixer_dirty(u->sink, TRUE);
        return 0;
    }

    if (mask & SND_CTL_EVENT_MASK_VALUE)
        pa_sink_update_volume_and_mute(u->sink);

    return 0;
}

static void sink_get_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume r;
    char vol_str_pcnt[PA_CVOLUME_SNPRINT_MAX];

    pa_assert(u);
    pa_assert(u->mixer_path);
    pa_assert(u->mixer_handle);

    if (pa_alsa_path_get_volume(u->mixer_path, u->mixer_handle, &s->channel_map, &r) < 0)
        return;

    /* Shift down by the base volume, so that 0dB becomes maximum volume */
    pa_sw_cvolume_multiply_scalar(&r, &r, s->base_volume);

    pa_log_debug("Read hardware volume: %s", pa_cvolume_snprint(vol_str_pcnt, sizeof(vol_str_pcnt), &r));

    if (u->mixer_path->has_dB) {
        char vol_str_db[PA_SW_CVOLUME_SNPRINT_DB_MAX];

        pa_log_debug("               in dB: %s", pa_sw_cvolume_snprint_dB(vol_str_db, sizeof(vol_str_db), &r));
    }

    if (pa_cvolume_equal(&u->hardware_volume, &r))
        return;

    s->real_volume = u->hardware_volume = r;

    /* Hmm, so the hardware volume changed, let's reset our software volume */
    if (u->mixer_path->has_dB)
        pa_sink_set_soft_volume(s, NULL);
}

static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume r;
    char vol_str_pcnt[PA_CVOLUME_SNPRINT_MAX];
    pa_bool_t deferred_volume = !!(s->flags & PA_SINK_DEFERRED_VOLUME);

    pa_assert(u);
    pa_assert(u->mixer_path);
    pa_assert(u->mixer_handle);

    /* Shift up by the base volume */
    pa_sw_cvolume_divide_scalar(&r, &s->real_volume, s->base_volume);

    if (pa_alsa_path_set_volume(u->mixer_path, u->mixer_handle, &s->channel_map, &r, deferred_volume, !deferred_volume) < 0)
        return;

    /* Shift down by the base volume, so that 0dB becomes maximum volume */
    pa_sw_cvolume_multiply_scalar(&r, &r, s->base_volume);

    u->hardware_volume = r;

    if (u->mixer_path->has_dB) {
        pa_cvolume new_soft_volume;
        pa_bool_t accurate_enough;
        char vol_str_db[PA_SW_CVOLUME_SNPRINT_DB_MAX];

        /* Match exactly what the user requested by software */
        pa_sw_cvolume_divide(&new_soft_volume, &s->real_volume, &u->hardware_volume);

        /* If the adjustment to do in software is only minimal we
         * can skip it. That saves us CPU at the expense of a bit of
         * accuracy */
        accurate_enough =
            (pa_cvolume_min(&new_soft_volume) >= (PA_VOLUME_NORM - VOLUME_ACCURACY)) &&
            (pa_cvolume_max(&new_soft_volume) <= (PA_VOLUME_NORM + VOLUME_ACCURACY));

        pa_log_debug("Requested volume: %s", pa_cvolume_snprint(vol_str_pcnt, sizeof(vol_str_pcnt), &s->real_volume));
        pa_log_debug("           in dB: %s", pa_sw_cvolume_snprint_dB(vol_str_db, sizeof(vol_str_db), &s->real_volume));
        pa_log_debug("Got hardware volume: %s", pa_cvolume_snprint(vol_str_pcnt, sizeof(vol_str_pcnt), &u->hardware_volume));
        pa_log_debug("              in dB: %s", pa_sw_cvolume_snprint_dB(vol_str_db, sizeof(vol_str_db), &u->hardware_volume));
        pa_log_debug("Calculated software volume: %s (accurate-enough=%s)",
                     pa_cvolume_snprint(vol_str_pcnt, sizeof(vol_str_pcnt), &new_soft_volume),
                     pa_yes_no(accurate_enough));
        pa_log_debug("                     in dB: %s", pa_sw_cvolume_snprint_dB(vol_str_db, sizeof(vol_str_db), &new_soft_volume));

        if (!accurate_enough)
            s->soft_volume = new_soft_volume;

    } else {
        pa_log_debug("Wrote hardware volume: %s", pa_cvolume_snprint(vol_str_pcnt, sizeof(vol_str_pcnt), &r));

        /* We can't match exactly what the user requested, hence let's
         * at least tell the user about it */

        s->real_volume = r;
    }
}

static void sink_write_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume hw_vol = s->thread_info.current_hw_volume;

    pa_assert(u);
    pa_assert(u->mixer_path);
    pa_assert(u->mixer_handle);
    pa_assert(s->flags & PA_SINK_DEFERRED_VOLUME);

    /* Shift up by the base volume */
    pa_sw_cvolume_divide_scalar(&hw_vol, &hw_vol, s->base_volume);

    if (pa_alsa_path_set_volume(u->mixer_path, u->mixer_handle, &s->channel_map, &hw_vol, TRUE, TRUE) < 0)
        pa_log_error("Writing HW volume failed");
    else {
        pa_cvolume tmp_vol;
        pa_bool_t accurate_enough;

        /* Shift down by the base volume, so that 0dB becomes maximum volume */
        pa_sw_cvolume_multiply_scalar(&hw_vol, &hw_vol, s->base_volume);

        pa_sw_cvolume_divide(&tmp_vol, &hw_vol, &s->thread_info.current_hw_volume);
        accurate_enough =
            (pa_cvolume_min(&tmp_vol) >= (PA_VOLUME_NORM - VOLUME_ACCURACY)) &&
            (pa_cvolume_max(&tmp_vol) <= (PA_VOLUME_NORM + VOLUME_ACCURACY));

        if (!accurate_enough) {
            union {
                char db[2][PA_SW_CVOLUME_SNPRINT_DB_MAX];
                char pcnt[2][PA_CVOLUME_SNPRINT_MAX];
            } vol;

            pa_log_debug("Written HW volume did not match with the request: %s (request) != %s",
                         pa_cvolume_snprint(vol.pcnt[0], sizeof(vol.pcnt[0]), &s->thread_info.current_hw_volume),
                         pa_cvolume_snprint(vol.pcnt[1], sizeof(vol.pcnt[1]), &hw_vol));
            pa_log_debug("                                           in dB: %s (request) != %s",
                         pa_sw_cvolume_snprint_dB(vol.db[0], sizeof(vol.db[0]), &s->thread_info.current_hw_volume),
                         pa_sw_cvolume_snprint_dB(vol.db[1], sizeof(vol.db[1]), &hw_vol));
        }
    }
}

static void sink_get_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_bool_t b;

    pa_assert(u);
    pa_assert(u->mixer_path);
    pa_assert(u->mixer_handle);

    if (pa_alsa_path_get_mute(u->mixer_path, u->mixer_handle, &b) < 0)
        return;

    s->muted = b;
}

static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);
    pa_assert(u->mixer_path);
    pa_assert(u->mixer_handle);

    pa_alsa_path_set_mute(u->mixer_path, u->mixer_handle, s->muted);
}

static void mixer_volume_init(struct userdata *u) {
    pa_assert(u);

    if (!u->mixer_path->has_volume) {
        pa_sink_set_write_volume_callback(u->sink, NULL);
        pa_sink_set_get_volume_callback(u->sink, NULL);
        pa_sink_set_set_volume_callback(u->sink, NULL);

        pa_log_info("Driver does not support hardware volume control, falling back to software volume control.");
    } else {
        pa_sink_set_get_volume_callback(u->sink, sink_get_volume_cb);
        pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);

        if (u->mixer_path->has_dB && u->deferred_volume) {
            pa_sink_set_write_volume_callback(u->sink, sink_write_volume_cb);
            pa_log_info("Successfully enabled deferred volume.");
        } else
            pa_sink_set_write_volume_callback(u->sink, NULL);

        if (u->mixer_path->has_dB) {
            pa_sink_enable_decibel_volume(u->sink, TRUE);
            pa_log_info("Hardware volume ranges from %0.2f dB to %0.2f dB.", u->mixer_path->min_dB, u->mixer_path->max_dB);

            u->sink->base_volume = pa_sw_volume_from_dB(-u->mixer_path->max_dB);
            u->sink->n_volume_steps = PA_VOLUME_NORM+1;

            pa_log_info("Fixing base volume to %0.2f dB", pa_sw_volume_to_dB(u->sink->base_volume));
        } else {
            pa_sink_enable_decibel_volume(u->sink, FALSE);
            pa_log_info("Hardware volume ranges from %li to %li.", u->mixer_path->min_volume, u->mixer_path->max_volume);

            u->sink->base_volume = PA_VOLUME_NORM;
            u->sink->n_volume_steps = u->mixer_path->max_volume - u->mixer_path->min_volume + 1;
        }

        pa_log_info("Using hardware volume control. Hardware dB scale %s.", u->mixer_path->has_dB ? "supported" : "not supported");
    }

    if (!u->mixer_path->has_mute) {
        pa_sink_set_get_mute_callback(u->sink, NULL);
        pa_sink_set_set_mute_callback(u->sink, NULL);
        pa_log_info("Driver does not support hardware mute control, falling back to software mute control.");
    } else {
        pa_sink_set_get_mute_callback(u->sink, sink_get_mute_cb);
        pa_sink_set_set_mute_callback(u->sink, sink_set_mute_cb);
        pa_log_info("Using hardware mute control.");
    }
}

static int sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    struct userdata *u = s->userdata;
    pa_alsa_port_data *data;

    pa_assert(u);
    pa_assert(p);
    pa_assert(u->mixer_handle);

    data = PA_DEVICE_PORT_DATA(p);

    pa_assert_se(u->mixer_path = data->path);
    pa_alsa_path_select(u->mixer_path, u->mixer_handle);

    mixer_volume_init(u);

    if (data->setting)
        pa_alsa_setting_select(data->setting, u->mixer_handle);

    if (s->set_mute)
        s->set_mute(s);
    if (s->flags & PA_SINK_DEFERRED_VOLUME) {
        if (s->write_volume)
            s->write_volume(s);
    } else {
        if (s->set_volume)
            s->set_volume(s);
    }

    return 0;
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    size_t before;
    pa_assert(u);
    pa_assert(u->use_tsched); /* only when timer scheduling is used
                               * we can dynamically adjust the
                               * latency */

    if (!u->pcm_handle)
        return;

    before = u->hwbuf_unused;
    update_sw_params(u);

    /* Let's check whether we now use only a smaller part of the
    buffer then before. If so, we need to make sure that subsequent
    rewinds are relative to the new maximum fill level and not to the
    current fill level. Thus, let's do a full rewind once, to clear
    things up. */

    if (u->hwbuf_unused > before) {
        pa_log_debug("Requesting rewind due to latency change.");
        pa_sink_request_rewind(s, (size_t) -1);
    }
}

static pa_idxset* sink_get_formats(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_idxset *ret = pa_idxset_new(NULL, NULL);
    pa_format_info *f;
    uint32_t idx;

    pa_assert(u);

    PA_IDXSET_FOREACH(f, u->formats, idx) {
        pa_idxset_put(ret, pa_format_info_copy(f), NULL);
    }

    return ret;
}

static pa_bool_t sink_set_formats(pa_sink *s, pa_idxset *formats) {
    struct userdata *u = s->userdata;
    pa_format_info *f, *g;
    uint32_t idx, n;

    pa_assert(u);

    /* FIXME: also validate sample rates against what the device supports */
    PA_IDXSET_FOREACH(f, formats, idx) {
        if (is_iec958(u) && f->encoding == PA_ENCODING_EAC3_IEC61937)
            /* EAC3 cannot be sent over over S/PDIF */
            return FALSE;
    }

    pa_idxset_free(u->formats, (pa_free2_cb_t) pa_format_info_free2, NULL);
    u->formats = pa_idxset_new(NULL, NULL);

    /* Note: the logic below won't apply if we're using software encoding.
     * This is fine for now since we don't support that via the passthrough
     * framework, but this must be changed if we do. */

    /* Count how many sample rates we support */
    for (idx = 0, n = 0; u->rates[idx]; idx++)
        n++;

    /* First insert non-PCM formats since we prefer those. */
    PA_IDXSET_FOREACH(f, formats, idx) {
        if (!pa_format_info_is_pcm(f)) {
            g = pa_format_info_copy(f);
            pa_format_info_set_prop_int_array(g, PA_PROP_FORMAT_RATE, (int *) u->rates, n);
            pa_idxset_put(u->formats, g, NULL);
        }
    }

    /* Now add any PCM formats */
    PA_IDXSET_FOREACH(f, formats, idx) {
        if (pa_format_info_is_pcm(f)) {
            /* We don't set rates here since we'll just tack on a resampler for
             * unsupported rates */
            pa_idxset_put(u->formats, pa_format_info_copy(f), NULL);
        }
    }

    return TRUE;
}

static pa_bool_t sink_update_rate_cb(pa_sink *s, uint32_t rate)
{
    struct userdata *u = s->userdata;
    int i;
    pa_bool_t supported = FALSE;

    pa_assert(u);

    for (i = 0; u->rates[i]; i++) {
        if (u->rates[i] == rate) {
            supported = TRUE;
            break;
        }
    }

    if (!supported) {
        pa_log_info("Sink does not support sample rate of %d Hz", rate);
        return FALSE;
    }

    if (!PA_SINK_IS_OPENED(s->state)) {
        pa_log_info("Updating rate for device %s, new rate is %d",u->device_name, rate);
        u->sink->sample_spec.rate = rate;
        return TRUE;
    }

    return FALSE;
}

static int process_rewind(struct userdata *u) {
    snd_pcm_sframes_t unused;
    size_t rewind_nbytes, unused_nbytes, limit_nbytes;
    pa_assert(u);

    /* Figure out how much we shall rewind and reset the counter */
    rewind_nbytes = u->sink->thread_info.rewind_nbytes;

    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    if (PA_UNLIKELY((unused = pa_alsa_safe_avail(u->pcm_handle, u->hwbuf_size, &u->sink->sample_spec)) < 0)) {
        pa_log("snd_pcm_avail() failed: %s", pa_alsa_strerror((int) unused));
        return -1;
    }

    unused_nbytes = (size_t) unused * u->frame_size;

    /* make sure rewind doesn't go too far, can cause issues with DMAs */
    unused_nbytes += u->rewind_safeguard;

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
            pa_log("snd_pcm_rewind() failed: %s", pa_alsa_strerror((int) out_frames));
            if (try_recover(u, "process_rewind", out_frames) < 0)
                return -1;
            out_frames = 0;
        }

        pa_log_debug("after: %lu", (unsigned long) out_frames);

        rewind_nbytes = (size_t) out_frames * u->frame_size;

        if (rewind_nbytes <= 0)
            pa_log_info("Tried rewind, but was apparently not possible.");
        else {
            u->write_count -= rewind_nbytes;
            pa_log_debug("Rewound %lu bytes.", (unsigned long) rewind_nbytes);
            pa_sink_process_rewind(u->sink, rewind_nbytes);

            u->after_rewind = TRUE;
            return 0;
        }
    } else
        pa_log_debug("Mhmm, actually there is nothing to rewind.");

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

    for (;;) {
        int ret;
        pa_usec_t rtpoll_sleep = 0;

#ifdef DEBUG_TIMING
        pa_log_debug("Loop");
#endif

        /* Render some data and write it to the dsp */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            int work_done;
            pa_usec_t sleep_usec = 0;
            pa_bool_t on_timeout = pa_rtpoll_timer_elapsed(u->rtpoll);

            if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
                if (process_rewind(u) < 0)
                        goto fail;

            if (u->use_mmap)
                work_done = mmap_write(u, &sleep_usec, revents & POLLOUT, on_timeout);
            else
                work_done = unix_write(u, &sleep_usec, revents & POLLOUT, on_timeout);

            if (work_done < 0)
                goto fail;

/*             pa_log_debug("work_done = %i", work_done); */

            if (work_done) {

                if (u->first) {
                    pa_log_info("Starting playback.");
                    snd_pcm_start(u->pcm_handle);

                    pa_smoother_resume(u->smoother, pa_rtclock_now(), TRUE);

                    u->first = FALSE;
                }

                update_smoother(u);
            }

            if (u->use_tsched) {
                pa_usec_t cusec;

                if (u->since_start <= u->hwbuf_size) {

                    /* USB devices on ALSA seem to hit a buffer
                     * underrun during the first iterations much
                     * quicker then we calculate here, probably due to
                     * the transport latency. To accommodate for that
                     * we artificially decrease the sleep time until
                     * we have filled the buffer at least once
                     * completely.*/

                    if (pa_log_ratelimit(PA_LOG_DEBUG))
                        pa_log_debug("Cutting sleep time for the initial iterations by half.");
                    sleep_usec /= 2;
                }

                /* OK, the playback buffer is now full, let's
                 * calculate when to wake up next */
#ifdef DEBUG_TIMING
                pa_log_debug("Waking up in %0.2fms (sound card clock).", (double) sleep_usec / PA_USEC_PER_MSEC);
#endif

                /* Convert from the sound card time domain to the
                 * system time domain */
                cusec = pa_smoother_translate(u->smoother, pa_rtclock_now(), sleep_usec);

#ifdef DEBUG_TIMING
                pa_log_debug("Waking up in %0.2fms (system clock).", (double) cusec / PA_USEC_PER_MSEC);
#endif

                /* We don't trust the conversion, so we wake up whatever comes first */
                rtpoll_sleep = PA_MIN(sleep_usec, cusec);
            }

            u->after_rewind = FALSE;

        }

        if (u->sink->flags & PA_SINK_DEFERRED_VOLUME) {
            pa_usec_t volume_sleep;
            pa_sink_volume_change_apply(u->sink, &volume_sleep);
            if (volume_sleep > 0) {
                if (rtpoll_sleep > 0)
                    rtpoll_sleep = PA_MIN(volume_sleep, rtpoll_sleep);
                else
                    rtpoll_sleep = volume_sleep;
            }
        }

        if (rtpoll_sleep > 0)
            pa_rtpoll_set_timer_relative(u->rtpoll, rtpoll_sleep);
        else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
            goto fail;

        if (u->sink->flags & PA_SINK_DEFERRED_VOLUME)
            pa_sink_volume_change_apply(u->sink, NULL);

        if (ret == 0)
            goto finish;

        /* Tell ALSA about this and process its response */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            struct pollfd *pollfd;
            int err;
            unsigned n;

            pollfd = pa_rtpoll_item_get_pollfd(u->alsa_rtpoll_item, &n);

            if ((err = snd_pcm_poll_descriptors_revents(u->pcm_handle, pollfd, n, &revents)) < 0) {
                pa_log("snd_pcm_poll_descriptors_revents() failed: %s", pa_alsa_strerror(err));
                goto fail;
            }

            if (revents & ~POLLOUT) {
                if (pa_alsa_recover_from_poll(u->pcm_handle, revents) < 0)
                    goto fail;

                u->first = TRUE;
                u->since_start = 0;
                revents = 0;
            } else if (revents && u->use_tsched && pa_log_ratelimit(PA_LOG_DEBUG))
                pa_log_debug("Wakeup from ALSA!");

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

static void set_sink_name(pa_sink_new_data *data, pa_modargs *ma, const char *device_id, const char *device_name, pa_alsa_mapping *mapping) {
    const char *n;
    char *t;

    pa_assert(data);
    pa_assert(ma);
    pa_assert(device_name);

    if ((n = pa_modargs_get_value(ma, "sink_name", NULL))) {
        pa_sink_new_data_set_name(data, n);
        data->namereg_fail = TRUE;
        return;
    }

    if ((n = pa_modargs_get_value(ma, "name", NULL)))
        data->namereg_fail = TRUE;
    else {
        n = device_id ? device_id : device_name;
        data->namereg_fail = FALSE;
    }

    if (mapping)
        t = pa_sprintf_malloc("alsa_output.%s.%s", n, mapping->name);
    else
        t = pa_sprintf_malloc("alsa_output.%s", n);

    pa_sink_new_data_set_name(data, t);
    pa_xfree(t);
}

static void find_mixer(struct userdata *u, pa_alsa_mapping *mapping, const char *element, pa_bool_t ignore_dB) {
    snd_hctl_t *hctl;

    if (!mapping && !element)
        return;

    if (!(u->mixer_handle = pa_alsa_open_mixer_for_pcm(u->pcm_handle, &u->control_device, &hctl))) {
        pa_log_info("Failed to find a working mixer device.");
        return;
    }

    if (element) {

        if (!(u->mixer_path = pa_alsa_path_synthesize(element, PA_ALSA_DIRECTION_OUTPUT)))
            goto fail;

        if (pa_alsa_path_probe(u->mixer_path, u->mixer_handle, hctl, ignore_dB) < 0)
            goto fail;

        pa_log_debug("Probed mixer path %s:", u->mixer_path->name);
        pa_alsa_path_dump(u->mixer_path);
    } else if (!(u->mixer_path_set = mapping->output_path_set))
        goto fail;

    return;

fail:

    if (u->mixer_path) {
        pa_alsa_path_free(u->mixer_path);
        u->mixer_path = NULL;
    }

    if (u->mixer_handle) {
        snd_mixer_close(u->mixer_handle);
        u->mixer_handle = NULL;
    }
}


static int setup_mixer(struct userdata *u, pa_bool_t ignore_dB) {
    pa_bool_t need_mixer_callback = FALSE;

    pa_assert(u);

    if (!u->mixer_handle)
        return 0;

    if (u->sink->active_port) {
        pa_alsa_port_data *data;

        /* We have a list of supported paths, so let's activate the
         * one that has been chosen as active */

        data = PA_DEVICE_PORT_DATA(u->sink->active_port);
        u->mixer_path = data->path;

        pa_alsa_path_select(data->path, u->mixer_handle);

        if (data->setting)
            pa_alsa_setting_select(data->setting, u->mixer_handle);

    } else {

        if (!u->mixer_path && u->mixer_path_set)
            u->mixer_path = pa_hashmap_first(u->mixer_path_set->paths);

        if (u->mixer_path) {
            /* Hmm, we have only a single path, then let's activate it */

            pa_alsa_path_select(u->mixer_path, u->mixer_handle);

            if (u->mixer_path->settings)
                pa_alsa_setting_select(u->mixer_path->settings, u->mixer_handle);
        } else
            return 0;
    }

    mixer_volume_init(u);

    /* Will we need to register callbacks? */
    if (u->mixer_path_set && u->mixer_path_set->paths) {
        pa_alsa_path *p;
        void *state;

        PA_HASHMAP_FOREACH(p, u->mixer_path_set->paths, state) {
            if (p->has_volume || p->has_mute)
                need_mixer_callback = TRUE;
        }
    }
    else if (u->mixer_path)
        need_mixer_callback = u->mixer_path->has_volume || u->mixer_path->has_mute;

    if (need_mixer_callback) {
        int (*mixer_callback)(snd_mixer_elem_t *, unsigned int);
        if (u->sink->flags & PA_SINK_DEFERRED_VOLUME) {
            u->mixer_pd = pa_alsa_mixer_pdata_new();
            mixer_callback = io_mixer_callback;

            if (pa_alsa_set_mixer_rtpoll(u->mixer_pd, u->mixer_handle, u->rtpoll) < 0) {
                pa_log("Failed to initialize file descriptor monitoring");
                return -1;
            }
        } else {
            u->mixer_fdl = pa_alsa_fdlist_new();
            mixer_callback = ctl_mixer_callback;

            if (pa_alsa_fdlist_set_handle(u->mixer_fdl, u->mixer_handle, NULL, u->core->mainloop) < 0) {
                pa_log("Failed to initialize file descriptor monitoring");
                return -1;
            }
        }

        if (u->mixer_path_set)
            pa_alsa_path_set_set_callback(u->mixer_path_set, u->mixer_handle, mixer_callback, u);
        else
            pa_alsa_path_set_callback(u->mixer_path, u->mixer_handle, mixer_callback, u);
    }

    return 0;
}

pa_sink *pa_alsa_sink_new(pa_module *m, pa_modargs *ma, const char*driver, pa_card *card, pa_alsa_mapping *mapping) {

    struct userdata *u = NULL;
    const char *dev_id = NULL;
    pa_sample_spec ss;
    uint32_t alternate_sample_rate;
    pa_channel_map map;
    uint32_t nfrags, frag_size, buffer_size, tsched_size, tsched_watermark, rewind_safeguard;
    snd_pcm_uframes_t period_frames, buffer_frames, tsched_frames;
    size_t frame_size;
    pa_bool_t use_mmap = TRUE, b, use_tsched = TRUE, d, ignore_dB = FALSE, namereg_fail = FALSE, deferred_volume = FALSE, set_formats = FALSE, fixed_latency_range = FALSE;
    pa_sink_new_data data;
    pa_alsa_profile_set *profile_set = NULL;

    pa_assert(m);
    pa_assert(ma);

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_ALSA) < 0) {
        pa_log("Failed to parse sample specification and channel map");
        goto fail;
    }

    alternate_sample_rate = m->core->alternate_sample_rate;
    if (pa_modargs_get_alternate_sample_rate(ma, &alternate_sample_rate) < 0) {
        pa_log("Failed to parse alternate sample rate");
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

    buffer_size = nfrags * frag_size;

    period_frames = frag_size/frame_size;
    buffer_frames = buffer_size/frame_size;
    tsched_frames = tsched_size/frame_size;

    if (pa_modargs_get_value_boolean(ma, "mmap", &use_mmap) < 0) {
        pa_log("Failed to parse mmap argument.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "tsched", &use_tsched) < 0) {
        pa_log("Failed to parse tsched argument.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "ignore_dB", &ignore_dB) < 0) {
        pa_log("Failed to parse ignore_dB argument.");
        goto fail;
    }

    rewind_safeguard = PA_MAX(DEFAULT_REWIND_SAFEGUARD_BYTES, pa_usec_to_bytes(DEFAULT_REWIND_SAFEGUARD_USEC, &ss));
    if (pa_modargs_get_value_u32(ma, "rewind_safeguard", &rewind_safeguard) < 0) {
        pa_log("Failed to parse rewind_safeguard argument");
        goto fail;
    }

    deferred_volume = m->core->deferred_volume;
    if (pa_modargs_get_value_boolean(ma, "deferred_volume", &deferred_volume) < 0) {
        pa_log("Failed to parse deferred_volume argument.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "fixed_latency_range", &fixed_latency_range) < 0) {
        pa_log("Failed to parse fixed_latency_range argument.");
        goto fail;
    }

    use_tsched = pa_alsa_may_tsched(use_tsched);

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->use_mmap = use_mmap;
    u->use_tsched = use_tsched;
    u->deferred_volume = deferred_volume;
    u->fixed_latency_range = fixed_latency_range;
    u->first = TRUE;
    u->rewind_safeguard = rewind_safeguard;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    u->smoother = pa_smoother_new(
            SMOOTHER_ADJUST_USEC,
            SMOOTHER_WINDOW_USEC,
            TRUE,
            TRUE,
            5,
            pa_rtclock_now(),
            TRUE);
    u->smoother_interval = SMOOTHER_MIN_INTERVAL;

    dev_id = pa_modargs_get_value(
            ma, "device_id",
            pa_modargs_get_value(ma, "device", DEFAULT_DEVICE));

    u->paths_dir = pa_xstrdup(pa_modargs_get_value(ma, "paths_dir", NULL));

    if (reserve_init(u, dev_id) < 0)
        goto fail;

    if (reserve_monitor_init(u, dev_id) < 0)
        goto fail;

    b = use_mmap;
    d = use_tsched;

    if (mapping) {

        if (!(dev_id = pa_modargs_get_value(ma, "device_id", NULL))) {
            pa_log("device_id= not set");
            goto fail;
        }

        if (!(u->pcm_handle = pa_alsa_open_by_device_id_mapping(
                      dev_id,
                      &u->device_name,
                      &ss, &map,
                      SND_PCM_STREAM_PLAYBACK,
                      &period_frames, &buffer_frames, tsched_frames,
                      &b, &d, mapping)))
            goto fail;

    } else if ((dev_id = pa_modargs_get_value(ma, "device_id", NULL))) {

        if (!(profile_set = pa_alsa_profile_set_new(NULL, &map)))
            goto fail;

        if (!(u->pcm_handle = pa_alsa_open_by_device_id_auto(
                      dev_id,
                      &u->device_name,
                      &ss, &map,
                      SND_PCM_STREAM_PLAYBACK,
                      &period_frames, &buffer_frames, tsched_frames,
                      &b, &d, profile_set, &mapping)))
            goto fail;

    } else {

        if (!(u->pcm_handle = pa_alsa_open_by_device_string(
                      pa_modargs_get_value(ma, "device", DEFAULT_DEVICE),
                      &u->device_name,
                      &ss, &map,
                      SND_PCM_STREAM_PLAYBACK,
                      &period_frames, &buffer_frames, tsched_frames,
                      &b, &d, FALSE)))
            goto fail;
    }

    pa_assert(u->device_name);
    pa_log_info("Successfully opened device %s.", u->device_name);

    if (pa_alsa_pcm_is_modem(u->pcm_handle)) {
        pa_log_notice("Device %s is modem, refusing further initialization.", u->device_name);
        goto fail;
    }

    if (mapping)
        pa_log_info("Selected mapping '%s' (%s).", mapping->description, mapping->name);

    if (use_mmap && !b) {
        pa_log_info("Device doesn't support mmap(), falling back to UNIX read/write mode.");
        u->use_mmap = use_mmap = FALSE;
    }

    if (use_tsched && (!b || !d)) {
        pa_log_info("Cannot enable timer-based scheduling, falling back to sound IRQ scheduling.");
        u->use_tsched = use_tsched = FALSE;
    }

    if (u->use_mmap)
        pa_log_info("Successfully enabled mmap() mode.");

    if (u->use_tsched) {
        pa_log_info("Successfully enabled timer-based scheduling mode.");

        if (u->fixed_latency_range)
            pa_log_info("Disabling latency range changes on underrun");
    }

    if (is_iec958(u) || is_hdmi(u))
        set_formats = TRUE;

    u->rates = pa_alsa_get_supported_rates(u->pcm_handle);
    if (!u->rates) {
        pa_log_error("Failed to find any supported sample rates.");
        goto fail;
    }

    /* ALSA might tweak the sample spec, so recalculate the frame size */
    frame_size = pa_frame_size(&ss);

    find_mixer(u, mapping, pa_modargs_get_value(ma, "control", NULL), ignore_dB);

    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    data.card = card;
    set_sink_name(&data, ma, dev_id, u->device_name, mapping);

    /* We need to give pa_modargs_get_value_boolean() a pointer to a local
     * variable instead of using &data.namereg_fail directly, because
     * data.namereg_fail is a bitfield and taking the address of a bitfield
     * variable is impossible. */
    namereg_fail = data.namereg_fail;
    if (pa_modargs_get_value_boolean(ma, "namereg_fail", &namereg_fail) < 0) {
        pa_log("Failed to parse namereg_fail argument.");
        pa_sink_new_data_done(&data);
        goto fail;
    }
    data.namereg_fail = namereg_fail;

    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_sink_new_data_set_alternate_sample_rate(&data, alternate_sample_rate);

    pa_alsa_init_proplist_pcm(m->core, data.proplist, u->pcm_handle);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->device_name);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_BUFFERING_BUFFER_SIZE, "%lu", (unsigned long) (buffer_frames * frame_size));
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_BUFFERING_FRAGMENT_SIZE, "%lu", (unsigned long) (period_frames * frame_size));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_ACCESS_MODE, u->use_tsched ? "mmap+timer" : (u->use_mmap ? "mmap" : "serial"));

    if (mapping) {
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_PROFILE_NAME, mapping->name);
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_PROFILE_DESCRIPTION, mapping->description);
    }

    pa_alsa_init_description(data.proplist);

    if (u->control_device)
        pa_alsa_init_proplist_ctl(data.proplist, u->control_device);

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    if (u->mixer_path_set)
        pa_alsa_add_ports(&data.ports, u->mixer_path_set, card);

    u->sink = pa_sink_new(m->core, &data, PA_SINK_HARDWARE | PA_SINK_LATENCY | (u->use_tsched ? PA_SINK_DYNAMIC_LATENCY : 0) |
                          (set_formats ? PA_SINK_SET_FORMATS : 0));
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "deferred_volume_safety_margin",
                                 &u->sink->thread_info.volume_change_safety_margin) < 0) {
        pa_log("Failed to parse deferred_volume_safety_margin parameter");
        goto fail;
    }

    if (pa_modargs_get_value_s32(ma, "deferred_volume_extra_delay",
                                 &u->sink->thread_info.volume_change_extra_delay) < 0) {
        pa_log("Failed to parse deferred_volume_extra_delay parameter");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    if (u->use_tsched)
        u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->set_state = sink_set_state_cb;
    u->sink->set_port = sink_set_port_cb;
    if (u->sink->alternate_sample_rate)
        u->sink->update_rate = sink_update_rate_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->frame_size = frame_size;
    u->fragment_size = frag_size = (size_t) (period_frames * frame_size);
    u->hwbuf_size = buffer_size = (size_t) (buffer_frames * frame_size);
    pa_cvolume_mute(&u->hardware_volume, u->sink->sample_spec.channels);

    pa_log_info("Using %0.1f fragments of size %lu bytes (%0.2fms), buffer size is %lu bytes (%0.2fms)",
                (double) u->hwbuf_size / (double) u->fragment_size,
                (long unsigned) u->fragment_size,
                (double) pa_bytes_to_usec(u->fragment_size, &ss) / PA_USEC_PER_MSEC,
                (long unsigned) u->hwbuf_size,
                (double) pa_bytes_to_usec(u->hwbuf_size, &ss) / PA_USEC_PER_MSEC);

    pa_sink_set_max_request(u->sink, u->hwbuf_size);
    if (pa_alsa_pcm_is_hw(u->pcm_handle))
        pa_sink_set_max_rewind(u->sink, u->hwbuf_size);
    else {
        pa_log_info("Disabling rewind for device %s", u->device_name);
        pa_sink_set_max_rewind(u->sink, 0);
    }

    if (u->use_tsched) {
        u->tsched_watermark_ref = tsched_watermark;
        reset_watermark(u, u->tsched_watermark_ref, &ss, FALSE);
    } else
        pa_sink_set_fixed_latency(u->sink, pa_bytes_to_usec(u->hwbuf_size, &ss));

    reserve_update(u);

    if (update_sw_params(u) < 0)
        goto fail;

    if (setup_mixer(u, ignore_dB) < 0)
        goto fail;

    pa_alsa_dump(PA_LOG_DEBUG, u->pcm_handle);

    if (!(u->thread = pa_thread_new("alsa-sink", thread_func, u))) {
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

    if ((data.volume_is_set || data.muted_is_set) && u->sink->write_volume)
        u->sink->write_volume(u->sink);

    if (set_formats) {
        /* For S/PDIF and HDMI, allow getting/setting custom formats */
        pa_format_info *format;

        /* To start with, we only support PCM formats. Other formats may be added
         * with pa_sink_set_formats().*/
        format = pa_format_info_new();
        format->encoding = PA_ENCODING_PCM;
        u->formats = pa_idxset_new(NULL, NULL);
        pa_idxset_put(u->formats, format, NULL);

        u->sink->get_formats = sink_get_formats;
        u->sink->set_formats = sink_set_formats;
    }

    pa_sink_put(u->sink);

    if (profile_set)
        pa_alsa_profile_set_free(profile_set);

    return u->sink;

fail:

    if (u)
        userdata_free(u);

    if (profile_set)
        pa_alsa_profile_set_free(profile_set);

    return NULL;
}

static void userdata_free(struct userdata *u) {
    pa_assert(u);

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

    if (u->mixer_pd)
        pa_alsa_mixer_pdata_free(u->mixer_pd);

    if (u->alsa_rtpoll_item)
        pa_rtpoll_item_free(u->alsa_rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->pcm_handle) {
        snd_pcm_drop(u->pcm_handle);
        snd_pcm_close(u->pcm_handle);
    }

    if (u->mixer_fdl)
        pa_alsa_fdlist_free(u->mixer_fdl);

    if (u->mixer_path && !u->mixer_path_set)
        pa_alsa_path_free(u->mixer_path);

    if (u->mixer_handle)
        snd_mixer_close(u->mixer_handle);

    if (u->smoother)
        pa_smoother_free(u->smoother);

    if (u->formats)
        pa_idxset_free(u->formats, (pa_free2_cb_t) pa_format_info_free2, NULL);

    if (u->rates)
        pa_xfree(u->rates);

    reserve_done(u);
    monitor_done(u);

    pa_xfree(u->device_name);
    pa_xfree(u->control_device);
    pa_xfree(u->paths_dir);
    pa_xfree(u);
}

void pa_alsa_sink_free(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    userdata_free(u);
}
