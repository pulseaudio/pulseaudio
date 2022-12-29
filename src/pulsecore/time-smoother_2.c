/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

/* The code in this file is based on the theoretical background found at
 * https://www.freedesktop.org/software/pulseaudio/misc/rate_estimator.odt.
 * The theory has never been reviewed, so it may be inaccurate in places. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/macro.h>
#include <pulse/sample.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include "time-smoother_2.h"

struct pa_smoother_2 {
    /* Values set when the smoother is created */
    pa_usec_t smoother_window_time;
    uint32_t rate;
    uint32_t frame_size;

    /* USB hack parameters */
    bool usb_hack;
    bool enable_usb_hack;
    uint32_t hack_threshold;

    /* Smoother state */
    bool init;
    bool paused;

    /* Current byte count start value */
    double start_pos;
    /* System time corresponding to start_pos */
    pa_usec_t start_time;
    /* Conversion factor between time domains */
    double time_factor;

    /* Used if the smoother is paused while still in init state */
    pa_usec_t fixup_time;

    /* Time offset for USB devices */
    int64_t time_offset;

    /* Various time stamps */
    pa_usec_t resume_time;
    pa_usec_t pause_time;
    pa_usec_t smoother_start_time;
    pa_usec_t last_time;

    /* Variables used for Kalman filter */
    double time_variance;
    double time_factor_variance;
    double kalman_variance;

    /* Variables used for low pass filter */
    double drift_filter;
    double drift_filter_1;
};

/* Create new smoother */
pa_smoother_2* pa_smoother_2_new(pa_usec_t window, pa_usec_t time_stamp, uint32_t frame_size, uint32_t rate) {
    pa_smoother_2 *s;

    pa_assert(window > 0);

    s = pa_xnew(pa_smoother_2, 1);
    s->enable_usb_hack = false;
    s->usb_hack = false;
    s->hack_threshold = 0;
    s->smoother_window_time = window;
    s->rate = rate;
    s->frame_size = frame_size;

    pa_smoother_2_reset(s, time_stamp);

    return s;
}

/* Free the smoother */
void pa_smoother_2_free(pa_smoother_2* s) {

    pa_assert(s);

    pa_xfree(s);
}

void pa_smoother_2_set_rate(pa_smoother_2 *s, pa_usec_t time_stamp, uint32_t rate) {

    pa_assert(s);
    pa_assert(rate > 0);

    /* If the rate has changed, data in the smoother will be invalid,
     * therefore also reset the smoother */
    if (rate != s->rate) {
        s->rate = rate;
        pa_smoother_2_reset(s, time_stamp);
    }
}

void pa_smoother_2_set_sample_spec(pa_smoother_2 *s, pa_usec_t time_stamp, pa_sample_spec *spec) {
    size_t frame_size;

    pa_assert(s);
    pa_assert(pa_sample_spec_valid(spec));

    /* If the sample spec has changed, data in the smoother will be invalid,
     * therefore also reset the smoother */
    frame_size = pa_frame_size(spec);
    if (frame_size != s->frame_size || spec->rate != s->rate) {
        s->frame_size = frame_size;
        s->rate = spec->rate;
        pa_smoother_2_reset(s, time_stamp);
    }
}

/* Add a new data point and re-calculate time conversion factor */
void pa_smoother_2_put(pa_smoother_2 *s, pa_usec_t time_stamp, int64_t byte_count) {
    double byte_difference, iteration_time;
    double time_delta_system, time_delta_card, drift, filter_constant, filter_constant_1;
    double temp, filtered_time_delta_card, expected_time_delta_card;

    pa_assert(s);

    /* Smoother is paused, nothing to do */
    if (s->paused)
        return;

    /* Initial setup or resume */
    if PA_UNLIKELY((s->init)) {
        s->resume_time = time_stamp;

        /* We have no data yet, nothing to do */
        if (byte_count <= 0)
            return;

        /* Now we are playing/recording.
         * Get fresh time stamps and save the start count */
        s->start_pos = (double)byte_count;
        s->last_time = time_stamp;
        s->start_time = time_stamp;
        s->smoother_start_time = time_stamp;

        s->usb_hack = s->enable_usb_hack;
        s->init = false;
        return;
    }

    /* Duration of last iteration */
    iteration_time = (double)time_stamp - s->last_time;

    /* Don't go backwards in time */
    if (iteration_time <= 0)
        return;

    /* Wait at least 100 ms before starting calculations, otherwise the
     * impact of the offset error will slow down convergence */
    if (time_stamp < s->smoother_start_time + 100 * PA_USEC_PER_MSEC)
        return;

    /* Time difference in system time domain */
    time_delta_system = time_stamp - s->start_time;

    /* Number of bytes played since start_time */
    byte_difference = (double)byte_count - s->start_pos;

    /* Time difference in soundcard time domain. Don't use
     * pa_bytes_to_usec() here because byte_difference need not
     * be on a sample boundary */
    time_delta_card = byte_difference / s->frame_size / s->rate * PA_USEC_PER_SEC;
    filtered_time_delta_card = time_delta_card;

    /* Prediction of measurement */
    expected_time_delta_card = time_delta_system * s->time_factor;

    /* Filtered variance of card time measurements */
    s->time_variance = 0.9 * s->time_variance + 0.1 * (time_delta_card - expected_time_delta_card) * (time_delta_card - expected_time_delta_card);

    /* Kalman filter, will only be used when the time factor has converged good enough,
     * the value of 100 corresponds to a change rate of approximately 10e-6 per second. */
    if (s->time_factor_variance < 100) {
        filtered_time_delta_card = (time_delta_card * s->kalman_variance + expected_time_delta_card * s->time_variance) / (s->kalman_variance + s->time_variance);
        s->kalman_variance = s->kalman_variance * s->time_variance / (s->kalman_variance + s->time_variance) + s->time_variance / 4 + 500;
    }

    /* This is a horrible hack which is necessary because USB sinks seem to fix up
     * the reported delay by some millisecondsconds shortly after startup. This is
     * an artifact, the real latency does not change on the reported jump. If the
     * change is not caught or if the hack is triggered inadvertently, it will lead to
     * prolonged convergence time and decreased stability of the reported latency.
     * Since the fix up will occur within the first seconds, it is disabled later to
     * avoid false triggers. When run as batch device, the threshold for the hack must
     * be lower (1000) than for timer based scheduling (2000). */
    if (s->usb_hack && time_stamp - s->smoother_start_time < 5 * PA_USEC_PER_SEC) {
        if ((time_delta_system - filtered_time_delta_card / s->time_factor) > (double)s->hack_threshold) {
            /* Recalculate initial conditions */
            temp = time_stamp - time_delta_card - s->start_time;
            s->start_time += temp;
            s->smoother_start_time += temp;
            s->time_offset = -temp;

            /* Reset time factor variance */
            s->time_factor_variance = 10000;

            pa_log_debug("USB Hack, start time corrected by %0.2f usec", temp);
            s->usb_hack = false;
            return;
         }
    }

    /* Parameter for lowpass filters with time constants of smoother_window_time
     * and smoother_window_time/8 */
    temp = (double)s->smoother_window_time / 6.2831853;
    filter_constant = iteration_time / (iteration_time + temp / 8.0);
    filter_constant_1 = iteration_time / (iteration_time + temp);

    /* Temporarily save the current time factor */
    temp = s->time_factor;

    /* Calculate geometric series */
    drift = (s->drift_filter_1 + 1.0) * (1.5 - filtered_time_delta_card / time_delta_system);

    /* 2nd order lowpass */
    s->drift_filter = (1 - filter_constant) * s->drift_filter + filter_constant * drift;
    s->drift_filter_1 = (1 - filter_constant) * s->drift_filter_1 + filter_constant * s->drift_filter;

    /* Calculate time conversion factor, filter again */
    s->time_factor = (1 - filter_constant_1) * s->time_factor + filter_constant_1 * (s->drift_filter_1 + 3) / (s->drift_filter_1 + 1) / 2;

    /* Filtered variance of time factor derivative, used as measure for the convergence of the time factor */
    temp = (s->time_factor - temp) / iteration_time * 10000000000000;
    s->time_factor_variance = (1 - filter_constant_1) * s->time_factor_variance + filter_constant_1 * temp * temp;

    /* Calculate new start time and corresponding sample count after window time */
    if (time_stamp > s->smoother_start_time + s->smoother_window_time) {
        s->start_pos += ((double)byte_count - s->start_pos) / (time_stamp - s->start_time) * iteration_time;
        s->start_time += (pa_usec_t)iteration_time;
    }

    /* Save current system time */
    s->last_time = time_stamp;
}

/* Calculate the current latency. For a source, the sign must be inverted */
int64_t pa_smoother_2_get_delay(pa_smoother_2 *s, pa_usec_t time_stamp, uint64_t byte_count) {
    int64_t now, delay;

    pa_assert(s);

    /* If we do not have a valid frame size and rate, just return 0 */
    if (!s->frame_size || !s->rate)
        return 0;

    /* Smoother is paused or has been resumed but no new data has been received */
    if (s->paused || s->init) {
        delay = (int64_t)((double)byte_count * PA_USEC_PER_SEC / s->frame_size / s->rate);
        return delay - pa_smoother_2_get(s, time_stamp);
    }

    /* Convert system time difference to soundcard time difference */
    now = (time_stamp - s->start_time - s->time_offset) * s->time_factor;

    /* Don't use pa_bytes_to_usec(), u->start_pos needs not be on a sample boundary */
    return (int64_t)(((double)byte_count - s->start_pos) / s->frame_size / s->rate * PA_USEC_PER_SEC) - now;
}

/* Convert system time to sound card time */
pa_usec_t pa_smoother_2_get(pa_smoother_2 *s, pa_usec_t time_stamp) {
    pa_usec_t current_time;

    pa_assert(s);

    /* If we do not have a valid frame size and rate, just return 0 */
    if (!s->frame_size || !s->rate)
        return 0;

    /* Sound card time at start_time */
    current_time = (pa_usec_t)(s->start_pos / s->frame_size / s->rate * PA_USEC_PER_SEC);

    /* If the smoother has not started, just return system time since resume */
    if (!s->start_time) {
        if (time_stamp >= s->resume_time && !s->paused)
            current_time = time_stamp - s->resume_time;
        else
            current_time = 0;

    /* If we are paused return the sound card time at pause_time */
    } else if (s->paused)
        current_time += (s->pause_time - s->start_time - s->time_offset - s->fixup_time) * s->time_factor;

    /* If we are initializing, add the time since resume to the card time at pause_time */
    else if (s->init) {
        current_time += (s->pause_time - s->start_time - s->time_offset - s->fixup_time) * s->time_factor;
        if (time_stamp > s->resume_time)
            current_time += (time_stamp - s->resume_time) * s->time_factor;

    /* Smoother is running, calculate current sound card time */
    } else
        current_time += (time_stamp - s->start_time - s->time_offset) * s->time_factor;

    return current_time;
}

/* Convert a time interval from sound card time to system time */
pa_usec_t pa_smoother_2_translate(pa_smoother_2 *s, pa_usec_t time_difference) {

    pa_assert(s);

    /* If not started yet, return the time difference */
    if (!s->start_time)
        return time_difference;

    return (pa_usec_t)(time_difference / s->time_factor);
}

/* Enable USB hack */
void pa_smoother_2_usb_hack_enable(pa_smoother_2 *s, bool enable, pa_usec_t offset) {

    pa_assert(s);

    s->enable_usb_hack = enable;
    s->hack_threshold = offset;
}

/* Reset the smoother */
void pa_smoother_2_reset(pa_smoother_2 *s, pa_usec_t time_stamp) {

    pa_assert(s);

   /* Reset variables for time estimation */
    s->drift_filter = 1.0;
    s->drift_filter_1 = 1.0;
    s->time_factor = 1.0;
    s->start_pos = 0;
    s->init = true;
    s->time_offset = 0;
    s->time_factor_variance = 10000.0;
    s->kalman_variance = 10000000.0;
    s->time_variance = 100000.0;
    s->start_time = 0;
    s->last_time = 0;
    s->smoother_start_time = 0;
    s->usb_hack = false;
    s->pause_time = time_stamp;
    s->fixup_time = 0;
    s->resume_time = time_stamp;
    s->paused = false;

    /* Set smoother to paused if rate or frame size are invalid */
    if (!s->frame_size || !s->rate)
        s->paused = true;
}

/* Pause the smoother */
void pa_smoother_2_pause(pa_smoother_2 *s, pa_usec_t time_stamp) {

    pa_assert(s);

    /* Smoother is already paused, nothing to do */
    if (s->paused)
        return;

    /* If we are in init state, add the pause time to the fixup time */
    if (s->init)
        s->fixup_time += s->resume_time - s->pause_time;
    else
        s->fixup_time = 0;

    s->smoother_start_time = 0;
    s->resume_time = time_stamp;
    s->pause_time = time_stamp;
    s->time_factor_variance = 10000.0;
    s->kalman_variance = 10000000.0;
    s->time_variance = 100000.0;
    s->init = true;
    s->paused = true;
}

/* Resume the smoother */
void pa_smoother_2_resume(pa_smoother_2 *s, pa_usec_t time_stamp) {

    pa_assert(s);

    if (!s->paused)
        return;

    /* Keep smoother paused if rate or frame size is not set */
    if (!s->frame_size || !s->rate)
        return;

    s->resume_time = time_stamp;
    s->paused = false;
}
