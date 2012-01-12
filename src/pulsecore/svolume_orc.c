/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk>
  Copyright 2010 Arun Raghavan <arun.raghavan@collabora.co.uk>

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

#include "cpu-orc.h"
#include <pulse/rtclock.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/random.h>
#include <pulsecore/svolume-orc-gen.h>

pa_do_volume_func_t fallback;

static void
pa_volume_s16ne_orc(int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
    if (channels == 2) {
        int64_t v = (int64_t)volumes[1] << 32 | volumes[0];
        pa_volume_s16ne_orc_2ch (samples, v, ((length / (sizeof(int16_t))) / 2));
    } else if (channels == 1)
        pa_volume_s16ne_orc_1ch (samples, volumes[0], length / (sizeof(int16_t)));
    else
        fallback(samples, volumes, channels, length);
}

#undef RUN_TEST

#ifdef RUN_TEST
#define CHANNELS 2
#define SAMPLES 1022
#define TIMES 1000
#define TIMES2 100
#define PADDING 16

static void run_test(void) {
    int16_t samples[SAMPLES];
    int16_t samples_ref[SAMPLES];
    int16_t samples_orig[SAMPLES];
    int32_t volumes[CHANNELS + PADDING];
    int i, j, padding;
    pa_do_volume_func_t func;
    pa_usec_t start, stop;
    int k;
    pa_usec_t min = INT_MAX, max = 0;
    double s1 = 0, s2 = 0;

    func = pa_get_volume_func(PA_SAMPLE_S16NE);

    printf("checking ORC %zd\n", sizeof(samples));

    pa_random(samples, sizeof(samples));
    memcpy(samples_ref, samples, sizeof(samples));
    memcpy(samples_orig, samples, sizeof(samples));

    for (i = 0; i < CHANNELS; i++)
        volumes[i] = PA_CLAMP_VOLUME(rand() >> 15);
    for (padding = 0; padding < PADDING; padding++, i++)
        volumes[i] = volumes[padding];

    func(samples_ref, volumes, CHANNELS, sizeof(samples));
    pa_volume_s16ne_orc(samples, volumes, CHANNELS, sizeof(samples));
    for (i = 0; i < SAMPLES; i++) {
        if (samples[i] != samples_ref[i]) {
            printf ("%d: %04x != %04x (%04x * %04x)\n", i, samples[i], samples_ref[i],
                      samples_orig[i], volumes[i % CHANNELS]);
        }
    }

    for (k = 0; k < TIMES2; k++) {
        start = pa_rtclock_now();
        for (j = 0; j < TIMES; j++) {
            memcpy(samples, samples_orig, sizeof(samples));
            pa_volume_s16ne_orc(samples, volumes, CHANNELS, sizeof(samples));
        }
        stop = pa_rtclock_now();

        if (min > (stop - start)) min = stop - start;
        if (max < (stop - start)) max = stop - start;
        s1 += stop - start;
        s2 += (stop - start) * (stop - start);
    }
    pa_log_info("ORC: %llu usec (min = %llu, max = %llu, stddev = %g).", (long long unsigned int)s1,
            (long long unsigned int)min, (long long unsigned int)max, sqrt(TIMES2 * s2 - s1 * s1) / TIMES2);

    min = INT_MAX; max = 0;
    s1 = s2 = 0;
    for (k = 0; k < TIMES2; k++) {
        start = pa_rtclock_now();
        for (j = 0; j < TIMES; j++) {
            memcpy(samples_ref, samples_orig, sizeof(samples));
            func(samples_ref, volumes, CHANNELS, sizeof(samples));
        }
        stop = pa_rtclock_now();

        if (min > (stop - start)) min = stop - start;
        if (max < (stop - start)) max = stop - start;
        s1 += stop - start;
        s2 += (stop - start) * (stop - start);
    }
    pa_log_info("ref: %llu usec (min = %llu, max = %llu, stddev = %g).", (long long unsigned int)s1,
            (long long unsigned int)min, (long long unsigned int)max, sqrt(TIMES2 * s2 - s1 * s1) / TIMES2);

    pa_assert_se(memcmp(samples_ref, samples, sizeof(samples)) == 0);
}
#endif

void pa_volume_func_init_orc(void) {
    pa_log_info("Initialising ORC optimized volume functions.");

#ifdef RUN_TEST
    run_test();
#endif

    fallback = pa_get_volume_func(PA_SAMPLE_S16NE);
    pa_set_volume_func(PA_SAMPLE_S16NE, (pa_do_volume_func_t) pa_volume_s16ne_orc);
}
