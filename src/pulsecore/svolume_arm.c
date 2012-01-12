/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk>

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

#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>

#include "cpu-arm.h"

#include "sample-util.h"

#if defined (__arm__) && defined (HAVE_ARMV6)

#define MOD_INC() \
    " subs  r0, r6, %2              \n\t" \
    " itt cs                        \n\t" \
    " addcs r0, %1                  \n\t" \
    " movcs r6, r0                  \n\t"

static void pa_volume_s16ne_arm(int16_t *samples, int32_t *volumes, unsigned channels, unsigned length) {
    int32_t *ve;

    /* Channels must be at least 4, and always a multiple of the original number.
     * This is also the max amount we overread the volume array, which should
     * have enough padding. */
    channels = channels == 3 ? 6 : PA_MAX (4U, channels);
    ve = volumes + channels;

    __asm__ __volatile__ (
        " mov r6, %1                      \n\t"
        " mov %3, %3, LSR #1              \n\t" /* length /= sizeof (int16_t) */
        " tst %3, #1                      \n\t" /* check for odd samples */
        " beq  2f                         \n\t"

        "1:                               \n\t"
        " ldr  r0, [r6], #4               \n\t" /* odd samples volumes */
        " ldrh r2, [%0]                   \n\t"

        " smulwb r0, r0, r2               \n\t"
        " ssat r0, #16, r0                \n\t"

        " strh r0, [%0], #2               \n\t"

        MOD_INC()

        "2:                               \n\t"
        " mov %3, %3, LSR #1              \n\t"
        " tst %3, #1                      \n\t" /* check for odd samples */
        " beq  4f                         \n\t"

        "3:                               \n\t"
        " ldrd r2, [r6], #8               \n\t" /* 2 samples at a time */
        " ldr  r0, [%0]                   \n\t"

        " smulwt r2, r2, r0               \n\t"
        " smulwb r3, r3, r0               \n\t"

        " ssat r2, #16, r2                \n\t"
        " ssat r3, #16, r3                \n\t"

        " pkhbt r0, r3, r2, LSL #16       \n\t"
        " str  r0, [%0], #4               \n\t"

        MOD_INC()

        "4:                               \n\t"
        " movs %3, %3, LSR #1             \n\t"
        " beq  6f                         \n\t"

        "5:                               \n\t"
        " ldrd r2, [r6], #8               \n\t" /* 4 samples at a time */
        " ldrd r4, [r6], #8               \n\t"
        " ldrd r0, [%0]                   \n\t"

        " smulwt r2, r2, r0               \n\t"
        " smulwb r3, r3, r0               \n\t"
        " smulwt r4, r4, r1               \n\t"
        " smulwb r5, r5, r1               \n\t"

        " ssat r2, #16, r2                \n\t"
        " ssat r3, #16, r3                \n\t"
        " ssat r4, #16, r4                \n\t"
        " ssat r5, #16, r5                \n\t"

        " pkhbt r0, r3, r2, LSL #16       \n\t"
        " pkhbt r1, r5, r4, LSL #16       \n\t"
        " strd  r0, [%0], #8              \n\t"

        MOD_INC()

        " subs %3, %3, #1                 \n\t"
        " bne 5b                          \n\t"
        "6:                               \n\t"

        : "+r" (samples), "+r" (volumes), "+r" (ve), "+r" (length)
        :
        : "r6", "r5", "r4", "r3", "r2", "r1", "r0", "cc"
    );
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

    printf("checking ARM %zd\n", sizeof(samples));

    pa_random(samples, sizeof(samples));
    memcpy(samples_ref, samples, sizeof(samples));
    memcpy(samples_orig, samples, sizeof(samples));

    for (i = 0; i < CHANNELS; i++)
        volumes[i] = PA_CLAMP_VOLUME(rand() >> 15);
    for (padding = 0; padding < PADDING; padding++, i++)
        volumes[i] = volumes[padding];

    func(samples_ref, volumes, CHANNELS, sizeof(samples));
    pa_volume_s16ne_arm(samples, volumes, CHANNELS, sizeof(samples));
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
            pa_volume_s16ne_arm(samples, volumes, CHANNELS, sizeof(samples));
        }
        stop = pa_rtclock_now();

        if (min > (stop - start)) min = stop - start;
        if (max < (stop - start)) max = stop - start;
        s1 += stop - start;
        s2 += (stop - start) * (stop - start);
    }
    pa_log_info("ARM: %llu usec (min = %llu, max = %llu, stddev = %g).", (long long unsigned int)s1,
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

#endif /* defined (__arm__) && defined (HAVE_ARMV6) */


void pa_volume_func_init_arm(pa_cpu_arm_flag_t flags) {
#if defined (__arm__) && defined (HAVE_ARMV6)
    pa_log_info("Initialising ARM optimized volume functions.");

#ifdef RUN_TEST
    run_test();
#endif

    pa_set_volume_func(PA_SAMPLE_S16NE, (pa_do_volume_func_t) pa_volume_s16ne_arm);
#endif /* defined (__arm__) && defined (HAVE_ARMV6) */
}
