#ifndef foopulsetimesmoother2hfoo
#define foopulsetimesmoother2hfoo

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

#include <pulse/sample.h>

typedef struct pa_smoother_2 pa_smoother_2;

/* Create new smoother */
pa_smoother_2* pa_smoother_2_new(pa_usec_t window, pa_usec_t time_stamp, uint32_t frame_size, uint32_t rate);
/* Free the smoother */
void pa_smoother_2_free(pa_smoother_2* s);
/* Reset the smoother */
void pa_smoother_2_reset(pa_smoother_2 *s, pa_usec_t time_stamp);
/* Pause the smoother */
void pa_smoother_2_pause(pa_smoother_2 *s, pa_usec_t time_stamp);
/* Resume the smoother */
void pa_smoother_2_resume(pa_smoother_2 *s, pa_usec_t time_stamp);

/* Add a new data point and re-calculate time conversion factor */
void pa_smoother_2_put(pa_smoother_2 *s, pa_usec_t time_stamp, int64_t byte_count);

/* Calculate the current latency. For a source, the sign of the result must be inverted */
int64_t pa_smoother_2_get_delay(pa_smoother_2 *s, pa_usec_t time_stamp, uint64_t byte_count);
/* Convert system time since start to sound card time */
pa_usec_t pa_smoother_2_get(pa_smoother_2 *s, pa_usec_t time_stamp);
/* Convert a time interval from sound card time to system time */
pa_usec_t pa_smoother_2_translate(pa_smoother_2 *s, pa_usec_t time_difference);

/* Enable USB hack, only used for alsa sinks */
void pa_smoother_2_usb_hack_enable(pa_smoother_2 *s, bool enable, pa_usec_t offset);
/* Set sample rate */
void pa_smoother_2_set_rate(pa_smoother_2 *s, pa_usec_t time_stamp, uint32_t rate);
/* Set rate and frame size */
void pa_smoother_2_set_sample_spec(pa_smoother_2 *s, pa_usec_t time_stamp, pa_sample_spec *spec);

#endif
