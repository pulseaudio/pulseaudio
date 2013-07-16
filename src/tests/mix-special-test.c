/***
  This file is part of PulseAudio.

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

#include <check.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>

#include <pulse/rtclock.h>
#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/mix.h>
#include <pulsecore/sample-util.h>

#include "runtime-test-util.h"

static void acquire_mix_streams(pa_mix_info streams[], unsigned nstreams) {
    unsigned i;

    for (i = 0; i < nstreams; i++)
        streams[i].ptr = pa_memblock_acquire_chunk(&streams[i].chunk);
}

static void release_mix_streams(pa_mix_info streams[], unsigned nstreams) {
    unsigned i;

    for (i = 0; i < nstreams; i++)
        pa_memblock_release(streams[i].chunk.memblock);
}

/* special case: mix 2 s16ne streams, 1 channel each */
static void pa_mix2_ch1_s16ne(pa_mix_info streams[], int16_t *data, unsigned length) {
    const int16_t *ptr0 = streams[0].ptr;
    const int16_t *ptr1 = streams[1].ptr;

    const int32_t cv0 = streams[0].linear[0].i;
    const int32_t cv1 = streams[1].linear[0].i;

    length /= sizeof(int16_t);

    for (; length > 0; length--) {
        int32_t sum;

        sum = pa_mult_s16_volume(*ptr0++, cv0);
        sum += pa_mult_s16_volume(*ptr1++, cv1);

        sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
        *data++ = sum;
    }
}

/* special case: mix 2 s16ne streams, 2 channels each */
static void pa_mix2_ch2_s16ne(pa_mix_info streams[], int16_t *data, unsigned length) {
    const int16_t *ptr0 = streams[0].ptr;
    const int16_t *ptr1 = streams[1].ptr;

    length /= sizeof(int16_t) * 2;

    for (; length > 0; length--) {
        int32_t sum;

        sum = pa_mult_s16_volume(*ptr0++, streams[0].linear[0].i);
        sum += pa_mult_s16_volume(*ptr1++, streams[1].linear[0].i);

        sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
        *data++ = sum;

        sum = pa_mult_s16_volume(*ptr0++, streams[0].linear[1].i);
        sum += pa_mult_s16_volume(*ptr1++, streams[1].linear[1].i);

        sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
        *data++ = sum;
    }
}

/* special case: mix 2 s16ne streams */
static void pa_mix2_s16ne(pa_mix_info streams[], unsigned channels, int16_t *data, unsigned length) {
    const int16_t *ptr0 = streams[0].ptr;
    const int16_t *ptr1 = streams[1].ptr;
    unsigned channel = 0;

    length /= sizeof(int16_t);

    for (; length > 0; length--) {
        int32_t sum;

        sum = pa_mult_s16_volume(*ptr0++, streams[0].linear[channel].i);
        sum += pa_mult_s16_volume(*ptr1++, streams[1].linear[channel].i);

        sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
        *data++ = sum;

        if (PA_UNLIKELY(++channel >= channels))
            channel = 0;
    }
}

/* special case: mix s16ne streams, 2 channels each */
static void pa_mix_ch2_s16ne(pa_mix_info streams[], unsigned nstreams, int16_t *data, unsigned length) {

    length /= sizeof(int16_t) * 2;

    for (; length > 0; length--) {
        int32_t sum0 = 0, sum1 = 0;
        unsigned i;

        for (i = 0; i < nstreams; i++) {
            pa_mix_info *m = streams + i;
            int32_t cv0 = m->linear[0].i;
            int32_t cv1 = m->linear[1].i;

            sum0 += pa_mult_s16_volume(*((int16_t*) m->ptr), cv0);
            m->ptr = (uint8_t*) m->ptr + sizeof(int16_t);

            sum1 += pa_mult_s16_volume(*((int16_t*) m->ptr), cv1);
            m->ptr = (uint8_t*) m->ptr + sizeof(int16_t);
        }

        *data++ = PA_CLAMP_UNLIKELY(sum0, -0x8000, 0x7FFF);
        *data++ = PA_CLAMP_UNLIKELY(sum1, -0x8000, 0x7FFF);
    }
}

static void pa_mix_generic_s16ne(pa_mix_info streams[], unsigned nstreams, unsigned channels, int16_t *data, unsigned length) {
    unsigned channel = 0;

    length /= sizeof(int16_t);

    for (; length > 0; length--) {
        int32_t sum = 0;
        unsigned i;

        for (i = 0; i < nstreams; i++) {
            pa_mix_info *m = streams + i;
            int32_t cv = m->linear[channel].i;

            if (PA_LIKELY(cv > 0))
                sum += pa_mult_s16_volume(*((int16_t*) m->ptr), cv);
            m->ptr = (uint8_t*) m->ptr + sizeof(int16_t);
        }

        sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
        *data++ = sum;

        if (PA_UNLIKELY(++channel >= channels))
            channel = 0;
    }
}

#define SAMPLES 1028
#define TIMES 1000
#define TIMES2 100

START_TEST (mix_special_1ch_test) {
    int16_t samples0[SAMPLES];
    int16_t samples1[SAMPLES];
    int16_t out[SAMPLES];
    int16_t out_ref[SAMPLES];
    pa_mempool *pool;
    pa_memchunk c0, c1;
    pa_mix_info m[2];
    unsigned nsamples = SAMPLES;

    fail_unless((pool = pa_mempool_new(false, 0)) != NULL, NULL);

    pa_random(samples0, nsamples * sizeof(int16_t));
    c0.memblock = pa_memblock_new_fixed(pool, samples0, nsamples * sizeof(int16_t), false);
    c0.length = pa_memblock_get_length(c0.memblock);
    c0.index = 0;

    pa_random(samples1, nsamples * sizeof(int16_t));
    c1.memblock = pa_memblock_new_fixed(pool, samples1, nsamples * sizeof(int16_t), false);
    c1.length = pa_memblock_get_length(c1.memblock);
    c1.index = 0;

    m[0].chunk = c0;
    m[0].volume.channels = 1;
    m[0].volume.values[0] = PA_VOLUME_NORM;
    m[0].linear[0].i = 0x5555;

    m[1].chunk = c1;
    m[1].volume.channels = 1;
    m[1].volume.values[0] = PA_VOLUME_NORM;
    m[1].linear[0].i = 0x6789;

    PA_RUNTIME_TEST_RUN_START("mix s16 generic 1 channel", TIMES, TIMES2) {
        acquire_mix_streams(m, 2);
        pa_mix_generic_s16ne(m, 2, 1, out_ref, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);
    } PA_RUNTIME_TEST_RUN_STOP

    PA_RUNTIME_TEST_RUN_START("mix s16 2 streams 1 channel", TIMES, TIMES2) {
        acquire_mix_streams(m, 2);
        pa_mix2_ch1_s16ne(m, out, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);
    } PA_RUNTIME_TEST_RUN_STOP

    fail_unless(memcmp(out, out_ref, nsamples * sizeof(int16_t)) == 0);

    pa_memblock_unref(c0.memblock);
    pa_memblock_unref(c1.memblock);

    pa_mempool_free(pool);
}
END_TEST

START_TEST (mix_special_2ch_test) {
    int16_t samples0[SAMPLES*2];
    int16_t samples1[SAMPLES*2];
    int16_t out[SAMPLES*2];
    int16_t out_ref[SAMPLES*2];
    int i;
    pa_mempool *pool;
    pa_memchunk c0, c1;
    pa_mix_info m[2];
    unsigned nsamples = SAMPLES * 2;

    fail_unless((pool = pa_mempool_new(false, 0)) != NULL, NULL);

    pa_random(samples0, nsamples * sizeof(int16_t));
    c0.memblock = pa_memblock_new_fixed(pool, samples0, nsamples * sizeof(int16_t), false);
    c0.length = pa_memblock_get_length(c0.memblock);
    c0.index = 0;

    pa_random(samples1, nsamples * sizeof(int16_t));
    c1.memblock = pa_memblock_new_fixed(pool, samples1, nsamples * sizeof(int16_t), false);
    c1.length = pa_memblock_get_length(c1.memblock);
    c1.index = 0;

    m[0].chunk = c0;
    m[0].volume.channels = 2;
    for (i = 0; i < m[0].volume.channels; i++) {
        m[0].volume.values[i] = PA_VOLUME_NORM;
        m[0].linear[i].i = 0x5555;
    }

    m[1].chunk = c1;
    m[1].volume.channels = 2;
    for (i = 0; i < m[1].volume.channels; i++) {
        m[1].volume.values[i] = PA_VOLUME_NORM;
        m[1].linear[i].i = 0x6789;
    }

    PA_RUNTIME_TEST_RUN_START("mix s16 generic 2 channels", TIMES, TIMES2) {
        acquire_mix_streams(m, 2);
        pa_mix_generic_s16ne(m, 2, 2, out_ref, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);
    } PA_RUNTIME_TEST_RUN_STOP

    PA_RUNTIME_TEST_RUN_START("mix s16 2 channels", TIMES, TIMES2) {
        acquire_mix_streams(m, 2);
        pa_mix_ch2_s16ne(m, 2, out, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);
    } PA_RUNTIME_TEST_RUN_STOP

    fail_unless(memcmp(out, out_ref, nsamples * sizeof(int16_t)) == 0);

    PA_RUNTIME_TEST_RUN_START("mix s16 2 streams", TIMES, TIMES2) {
        acquire_mix_streams(m, 2);
        pa_mix2_s16ne(m, 2, out, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);
    } PA_RUNTIME_TEST_RUN_STOP

    fail_unless(memcmp(out, out_ref, nsamples * sizeof(int16_t)) == 0);

    PA_RUNTIME_TEST_RUN_START("mix s16 2 streams 2 channels", TIMES, TIMES2) {
        acquire_mix_streams(m, 2);
        pa_mix2_ch2_s16ne(m, out, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);
    } PA_RUNTIME_TEST_RUN_STOP

    fail_unless(memcmp(out, out_ref, nsamples * sizeof(int16_t)) == 0);

    pa_memblock_unref(c0.memblock);
    pa_memblock_unref(c1.memblock);

    pa_mempool_free(pool);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("Mix-special");
    tc = tcase_create("mix-special 1ch");
    tcase_add_test(tc, mix_special_1ch_test);
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);
    tc = tcase_create("mix-special 2ch");
    tcase_add_test(tc, mix_special_2ch_test);
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
