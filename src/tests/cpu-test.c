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
#include <math.h>

#include <pulse/rtclock.h>
#include <pulsecore/cpu-x86.h>
#include <pulsecore/cpu-orc.h>
#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>
#include <pulsecore/sconv.h>
#include <pulsecore/remap.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/mix.h>

#define PA_CPU_TEST_RUN_START(l, t1, t2)                        \
{                                                               \
    int _j, _k;                                                 \
    int _times = (t1), _times2 = (t2);                          \
    pa_usec_t _start, _stop;                                    \
    pa_usec_t _min = INT_MAX, _max = 0;                         \
    double _s1 = 0, _s2 = 0;                                    \
    const char *_label = (l);                                   \
                                                                \
    for (_k = 0; _k < _times2; _k++) {                          \
        _start = pa_rtclock_now();                              \
        for (_j = 0; _j < _times; _j++)

#define PA_CPU_TEST_RUN_STOP                                    \
        _stop = pa_rtclock_now();                               \
                                                                \
        if (_min > (_stop - _start)) _min = _stop - _start;     \
        if (_max < (_stop - _start)) _max = _stop - _start;     \
        _s1 += _stop - _start;                                  \
        _s2 += (_stop - _start) * (_stop - _start);             \
    }                                                           \
    pa_log_debug("%s: %llu usec (avg: %g, min = %llu, max = %llu, stddev = %g).", _label, \
            (long long unsigned int)_s1,                        \
            ((double)_s1 / _times2),                            \
            (long long unsigned int)_min,                       \
            (long long unsigned int)_max,                       \
            sqrt(_times2 * _s2 - _s1 * _s1) / _times2);         \
}

/* Common defines for svolume tests */
#define SAMPLES 1028
#define TIMES 1000
#define TIMES2 100
#define PADDING 16

static void run_volume_test(
        pa_do_volume_func_t func,
        pa_do_volume_func_t orig_func,
        int align,
        int channels,
        pa_bool_t correct,
        pa_bool_t perf) {

    PA_DECLARE_ALIGNED(8, int16_t, s[SAMPLES]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, s_ref[SAMPLES]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, s_orig[SAMPLES]) = { 0 };
    int32_t volumes[channels + PADDING];
    int16_t *samples, *samples_ref, *samples_orig;
    int i, padding, nsamples, size;

    /* Force sample alignment as requested */
    samples = s + (8 - align);
    samples_ref = s_ref + (8 - align);
    samples_orig = s_orig + (8 - align);
    nsamples = SAMPLES - (8 - align);
    if (nsamples % channels)
        nsamples -= nsamples % channels;
    size = nsamples * sizeof(int16_t);

    pa_random(samples, size);
    memcpy(samples_ref, samples, size);
    memcpy(samples_orig, samples, size);

    for (i = 0; i < channels; i++)
        volumes[i] = PA_CLAMP_VOLUME((pa_volume_t)(rand() >> 15));
    for (padding = 0; padding < PADDING; padding++, i++)
        volumes[i] = volumes[padding];

    if (correct) {
        orig_func(samples_ref, volumes, channels, size);
        func(samples, volumes, channels, size);

        for (i = 0; i < nsamples; i++) {
            if (samples[i] != samples_ref[i]) {
                pa_log_debug("Correctness test failed: align=%d, channels=%d", align, channels);
                pa_log_debug("%d: %04hx != %04hx (%04hx * %08x)\n", i, samples[i], samples_ref[i],
                        samples_orig[i], volumes[i % channels]);
                fail();
            }
        }
    }

    if (perf) {
        pa_log_debug("Testing svolume %dch performance with %d sample alignment", channels, align);

        PA_CPU_TEST_RUN_START("func", TIMES, TIMES2) {
            memcpy(samples, samples_orig, size);
            func(samples, volumes, channels, size);
        } PA_CPU_TEST_RUN_STOP

        PA_CPU_TEST_RUN_START("orig", TIMES, TIMES2) {
            memcpy(samples_ref, samples_orig, size);
            orig_func(samples_ref, volumes, channels, size);
        } PA_CPU_TEST_RUN_STOP

        fail_unless(memcmp(samples_ref, samples, size) == 0);
    }
}

#if defined (__i386__) || defined (__amd64__)
START_TEST (svolume_mmx_test) {
    pa_do_volume_func_t orig_func, mmx_func;
    pa_cpu_x86_flag_t flags = 0;
    int i, j;

    pa_cpu_get_x86_flags(&flags);

    if (!((flags & PA_CPU_X86_MMX) && (flags & PA_CPU_X86_CMOV))) {
        pa_log_info("MMX/CMOV not supported. Skipping");
        return;
    }

    orig_func = pa_get_volume_func(PA_SAMPLE_S16NE);
    pa_volume_func_init_mmx(flags);
    mmx_func = pa_get_volume_func(PA_SAMPLE_S16NE);

    pa_log_debug("Checking MMX svolume");
    for (i = 1; i <= 3; i++) {
        for (j = 0; j < 7; j++)
            run_volume_test(mmx_func, orig_func, j, i, TRUE, FALSE);
    }
    run_volume_test(mmx_func, orig_func, 7, 1, TRUE, TRUE);
    run_volume_test(mmx_func, orig_func, 7, 2, TRUE, TRUE);
    run_volume_test(mmx_func, orig_func, 7, 3, TRUE, TRUE);
}
END_TEST

START_TEST (svolume_sse_test) {
    pa_do_volume_func_t orig_func, sse_func;
    pa_cpu_x86_flag_t flags = 0;
    int i, j;

    pa_cpu_get_x86_flags(&flags);

    if (!(flags & PA_CPU_X86_SSE2)) {
        pa_log_info("SSE2 not supported. Skipping");
        return;
    }

    orig_func = pa_get_volume_func(PA_SAMPLE_S16NE);
    pa_volume_func_init_sse(flags);
    sse_func = pa_get_volume_func(PA_SAMPLE_S16NE);

    pa_log_debug("Checking SSE2 svolume");
    for (i = 1; i <= 3; i++) {
        for (j = 0; j < 7; j++)
            run_volume_test(sse_func, orig_func, j, i, TRUE, FALSE);
    }
    run_volume_test(sse_func, orig_func, 7, 1, TRUE, TRUE);
    run_volume_test(sse_func, orig_func, 7, 2, TRUE, TRUE);
    run_volume_test(sse_func, orig_func, 7, 3, TRUE, TRUE);
}
END_TEST
#endif /* defined (__i386__) || defined (__amd64__) */

#if defined (__arm__) && defined (__linux__)
START_TEST (svolume_arm_test) {
    pa_do_volume_func_t orig_func, arm_func;
    pa_cpu_arm_flag_t flags = 0;
    int i, j;

    pa_cpu_get_arm_flags(&flags);

    if (!(flags & PA_CPU_ARM_V6)) {
        pa_log_info("ARMv6 instructions not supported. Skipping");
        return;
    }

    orig_func = pa_get_volume_func(PA_SAMPLE_S16NE);
    pa_volume_func_init_arm(flags);
    arm_func = pa_get_volume_func(PA_SAMPLE_S16NE);

    pa_log_debug("Checking ARM svolume");
    for (i = 1; i <= 3; i++) {
        for (j = 0; j < 7; j++)
            run_volume_test(arm_func, orig_func, j, i, TRUE, FALSE);
    }
    run_volume_test(arm_func, orig_func, 7, 1, TRUE, TRUE);
    run_volume_test(arm_func, orig_func, 7, 2, TRUE, TRUE);
    run_volume_test(arm_func, orig_func, 7, 3, TRUE, TRUE);
}
END_TEST
#endif /* defined (__arm__) && defined (__linux__) */

START_TEST (svolume_orc_test) {
    pa_do_volume_func_t orig_func, orc_func;
    pa_cpu_info cpu_info;
    int i, j;

#if defined (__i386__) || defined (__amd64__)
    pa_zero(cpu_info);
    cpu_info.cpu_type = PA_CPU_X86;
    pa_cpu_get_x86_flags(&cpu_info.flags.x86);
#endif

    orig_func = pa_get_volume_func(PA_SAMPLE_S16NE);

    if (!pa_cpu_init_orc(cpu_info)) {
        pa_log_info("Orc not supported. Skipping");
        return;
    }

    orc_func = pa_get_volume_func(PA_SAMPLE_S16NE);

    pa_log_debug("Checking Orc svolume");
    for (i = 1; i <= 2; i++) {
        for (j = 0; j < 7; j++)
            run_volume_test(orc_func, orig_func, j, i, TRUE, FALSE);
    }
    run_volume_test(orc_func, orig_func, 7, 1, TRUE, TRUE);
    run_volume_test(orc_func, orig_func, 7, 2, TRUE, TRUE);
}
END_TEST

#undef SAMPLES
#undef TIMES
#undef TIMES2
#undef PADDING
/* End svolume tests */

/* Start conversion tests */
#define SAMPLES 1028
#define TIMES 1000
#define TIMES2 100

static void run_conv_test_float_to_s16(
        pa_convert_func_t func,
        pa_convert_func_t orig_func,
        int align,
        pa_bool_t correct,
        pa_bool_t perf) {

    PA_DECLARE_ALIGNED(8, int16_t, s[SAMPLES]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, s_ref[SAMPLES]) = { 0 };
    PA_DECLARE_ALIGNED(8, float, f[SAMPLES]);
    int16_t *samples, *samples_ref;
    float *floats;
    int i, nsamples;

    /* Force sample alignment as requested */
    samples = s + (8 - align);
    samples_ref = s_ref + (8 - align);
    floats = f + (8 - align);
    nsamples = SAMPLES - (8 - align);

    for (i = 0; i < nsamples; i++) {
        floats[i] = 2.1f * (rand()/(float) RAND_MAX - 0.5f);
    }

    if (correct) {
        orig_func(nsamples, floats, samples_ref);
        func(nsamples, floats, samples);

        for (i = 0; i < nsamples; i++) {
            if (abs(samples[i] - samples_ref[i]) > 1) {
                pa_log_debug("Correctness test failed: align=%d", align);
                pa_log_debug("%d: %04hx != %04hx (%.24f)\n", i, samples[i], samples_ref[i], floats[i]);
                fail();
            }
        }
    }

    if (perf) {
        pa_log_debug("Testing sconv performance with %d sample alignment", align);

        PA_CPU_TEST_RUN_START("func", TIMES, TIMES2) {
            func(nsamples, floats, samples);
        } PA_CPU_TEST_RUN_STOP

        PA_CPU_TEST_RUN_START("orig", TIMES, TIMES2) {
            orig_func(nsamples, floats, samples_ref);
        } PA_CPU_TEST_RUN_STOP
    }
}

/* This test is currently only run under NEON */
#if defined (__arm__) && defined (__linux__)
#ifdef HAVE_NEON
static void run_conv_test_s16_to_float(
        pa_convert_func_t func,
        pa_convert_func_t orig_func,
        int align,
        pa_bool_t correct,
        pa_bool_t perf) {

    PA_DECLARE_ALIGNED(8, float, f[SAMPLES]) = { 0 };
    PA_DECLARE_ALIGNED(8, float, f_ref[SAMPLES]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, s[SAMPLES]);
    float *floats, *floats_ref;
    int16_t *samples;
    int i, nsamples;

    /* Force sample alignment as requested */
    floats = f + (8 - align);
    floats_ref = f_ref + (8 - align);
    samples = s + (8 - align);
    nsamples = SAMPLES - (8 - align);

    pa_random(samples, nsamples * sizeof(int16_t));

    if (correct) {
        orig_func(nsamples, samples, floats_ref);
        func(nsamples, samples, floats);

        for (i = 0; i < nsamples; i++) {
            if (fabsf(floats[i] - floats_ref[i]) > 0.0001) {
                pa_log_debug("Correctness test failed: align=%d", align);
                pa_log_debug("%d: %.24f != %.24f (%d)\n", i, floats[i], floats_ref[i], samples[i]);
                fail();
            }
        }
    }

    if (perf) {
        pa_log_debug("Testing sconv performance with %d sample alignment", align);

        PA_CPU_TEST_RUN_START("func", TIMES, TIMES2) {
            func(nsamples, samples, floats);
        } PA_CPU_TEST_RUN_STOP

        PA_CPU_TEST_RUN_START("orig", TIMES, TIMES2) {
            orig_func(nsamples, samples, floats_ref);
        } PA_CPU_TEST_RUN_STOP
    }
}
#endif /* HAVE_NEON */
#endif /* defined (__arm__) && defined (__linux__) */

#if defined (__i386__) || defined (__amd64__)
START_TEST (sconv_sse2_test) {
    pa_cpu_x86_flag_t flags = 0;
    pa_convert_func_t orig_func, sse2_func;

    pa_cpu_get_x86_flags(&flags);

    if (!(flags & PA_CPU_X86_SSE2)) {
        pa_log_info("SSE2 not supported. Skipping");
        return;
    }

    orig_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);
    pa_convert_func_init_sse(PA_CPU_X86_SSE2);
    sse2_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);

    pa_log_debug("Checking SSE2 sconv (float -> s16)");
    run_conv_test_float_to_s16(sse2_func, orig_func, 0, TRUE, FALSE);
    run_conv_test_float_to_s16(sse2_func, orig_func, 1, TRUE, FALSE);
    run_conv_test_float_to_s16(sse2_func, orig_func, 2, TRUE, FALSE);
    run_conv_test_float_to_s16(sse2_func, orig_func, 3, TRUE, FALSE);
    run_conv_test_float_to_s16(sse2_func, orig_func, 4, TRUE, FALSE);
    run_conv_test_float_to_s16(sse2_func, orig_func, 5, TRUE, FALSE);
    run_conv_test_float_to_s16(sse2_func, orig_func, 6, TRUE, FALSE);
    run_conv_test_float_to_s16(sse2_func, orig_func, 7, TRUE, TRUE);
}
END_TEST

START_TEST (sconv_sse_test) {
    pa_cpu_x86_flag_t flags = 0;
    pa_convert_func_t orig_func, sse_func;

    pa_cpu_get_x86_flags(&flags);

    if (!(flags & PA_CPU_X86_SSE)) {
        pa_log_info("SSE not supported. Skipping");
        return;
    }

    orig_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);
    pa_convert_func_init_sse(PA_CPU_X86_SSE);
    sse_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);

    pa_log_debug("Checking SSE sconv (float -> s16)");
    run_conv_test_float_to_s16(sse_func, orig_func, 0, TRUE, FALSE);
    run_conv_test_float_to_s16(sse_func, orig_func, 1, TRUE, FALSE);
    run_conv_test_float_to_s16(sse_func, orig_func, 2, TRUE, FALSE);
    run_conv_test_float_to_s16(sse_func, orig_func, 3, TRUE, FALSE);
    run_conv_test_float_to_s16(sse_func, orig_func, 4, TRUE, FALSE);
    run_conv_test_float_to_s16(sse_func, orig_func, 5, TRUE, FALSE);
    run_conv_test_float_to_s16(sse_func, orig_func, 6, TRUE, FALSE);
    run_conv_test_float_to_s16(sse_func, orig_func, 7, TRUE, TRUE);
}
END_TEST
#endif /* defined (__i386__) || defined (__amd64__) */

#if defined (__arm__) && defined (__linux__)
#ifdef HAVE_NEON
START_TEST (sconv_neon_test) {
    pa_cpu_arm_flag_t flags = 0;
    pa_convert_func_t orig_from_func, neon_from_func;
    pa_convert_func_t orig_to_func, neon_to_func;

    pa_cpu_get_arm_flags(&flags);

    if (!(flags & PA_CPU_ARM_NEON)) {
        pa_log_info("NEON not supported. Skipping");
        return;
    }

    orig_from_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);
    orig_to_func = pa_get_convert_to_float32ne_function(PA_SAMPLE_S16LE);
    pa_convert_func_init_neon(flags);
    neon_from_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);
    neon_to_func = pa_get_convert_to_float32ne_function(PA_SAMPLE_S16LE);

    pa_log_debug("Checking NEON sconv (float -> s16)");
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 0, TRUE, FALSE);
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 1, TRUE, FALSE);
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 2, TRUE, FALSE);
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 3, TRUE, FALSE);
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 4, TRUE, FALSE);
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 5, TRUE, FALSE);
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 6, TRUE, FALSE);
    run_conv_test_float_to_s16(neon_from_func, orig_from_func, 7, TRUE, TRUE);

    pa_log_debug("Checking NEON sconv (s16 -> float)");
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 0, TRUE, FALSE);
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 1, TRUE, FALSE);
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 2, TRUE, FALSE);
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 3, TRUE, FALSE);
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 4, TRUE, FALSE);
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 5, TRUE, FALSE);
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 6, TRUE, FALSE);
    run_conv_test_s16_to_float(neon_to_func, orig_to_func, 7, TRUE, TRUE);
}
END_TEST
#endif /* HAVE_NEON */
#endif /* defined (__arm__) && defined (__linux__) */

#undef SAMPLES
#undef TIMES
/* End conversion tests */

/* Start remap tests */
#define SAMPLES 1028
#define TIMES 1000
#define TIMES2 100

static void run_remap_test_mono_stereo_float(
        pa_remap_t *remap,
        pa_do_remap_func_t func,
        pa_do_remap_func_t orig_func,
        int align,
        pa_bool_t correct,
        pa_bool_t perf) {

    PA_DECLARE_ALIGNED(8, float, s_ref[SAMPLES*2]) = { 0 };
    PA_DECLARE_ALIGNED(8, float, s[SAMPLES*2]) = { 0 };
    PA_DECLARE_ALIGNED(8, float, m[SAMPLES]);
    float *stereo, *stereo_ref;
    float *mono;
    int i, nsamples;

    /* Force sample alignment as requested */
    stereo = s + (8 - align);
    stereo_ref = s_ref + (8 - align);
    mono = m + (8 - align);
    nsamples = SAMPLES - (8 - align);

    for (i = 0; i < nsamples; i++)
        mono[i] = 2.1f * (rand()/(float) RAND_MAX - 0.5f);

    if (correct) {
        orig_func(remap, stereo_ref, mono, nsamples);
        func(remap, stereo, mono, nsamples);

        for (i = 0; i < nsamples * 2; i++) {
            if (fabsf(stereo[i] - stereo_ref[i]) > 0.0001) {
                pa_log_debug("Correctness test failed: align=%d", align);
                pa_log_debug("%d: %.24f != %.24f (%.24f)\n", i, stereo[i], stereo_ref[i], mono[i]);
                fail();
            }
        }
    }

    if (perf) {
        pa_log_debug("Testing remap performance with %d sample alignment", align);

        PA_CPU_TEST_RUN_START("func", TIMES, TIMES2) {
            func(remap, stereo, mono, nsamples);
        } PA_CPU_TEST_RUN_STOP

        PA_CPU_TEST_RUN_START("orig", TIMES, TIMES2) {
            orig_func(remap, stereo_ref, mono, nsamples);
        } PA_CPU_TEST_RUN_STOP
    }
}

static void run_remap_test_mono_stereo_s16(
        pa_remap_t *remap,
        pa_do_remap_func_t func,
        pa_do_remap_func_t orig_func,
        int align,
        pa_bool_t correct,
        pa_bool_t perf) {

    PA_DECLARE_ALIGNED(8, int16_t, s_ref[SAMPLES*2]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, s[SAMPLES*2]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, m[SAMPLES]);
    int16_t *stereo, *stereo_ref;
    int16_t *mono;
    int i, nsamples;

    /* Force sample alignment as requested */
    stereo = s + (8 - align);
    stereo_ref = s_ref + (8 - align);
    mono = m + (8 - align);
    nsamples = SAMPLES - (8 - align);

    pa_random(mono, nsamples * sizeof(int16_t));

    if (correct) {
        orig_func(remap, stereo_ref, mono, nsamples);
        func(remap, stereo, mono, nsamples);

        for (i = 0; i < nsamples * 2; i++) {
            if (abs(stereo[i] - stereo_ref[i]) > 1) {
                pa_log_debug("Correctness test failed: align=%d", align);
                pa_log_debug("%d: %d != %d (%d)\n", i, stereo[i], stereo_ref[i], mono[i]);
                fail();
            }
        }
    }

    if (perf) {
        pa_log_debug("Testing remap performance with %d sample alignment", align);

        PA_CPU_TEST_RUN_START("func", TIMES, TIMES2) {
            func(remap, stereo, mono, nsamples);
        } PA_CPU_TEST_RUN_STOP

        PA_CPU_TEST_RUN_START("orig", TIMES, TIMES2) {
            orig_func(remap, stereo_ref, mono, nsamples);
        } PA_CPU_TEST_RUN_STOP
    }
}

static void remap_test_mono_stereo_float(
        pa_init_remap_func_t init_func,
        pa_init_remap_func_t orig_init_func) {

    pa_sample_format_t sf;
    pa_remap_t remap;
    pa_sample_spec iss, oss;
    pa_do_remap_func_t orig_func, func;

    iss.format = oss.format = sf = PA_SAMPLE_FLOAT32NE;
    iss.channels = 1;
    oss.channels = 2;
    remap.format = &sf;
    remap.i_ss = &iss;
    remap.o_ss = &oss;
    remap.map_table_f[0][0] = 1.0;
    remap.map_table_f[1][0] = 1.0;
    remap.map_table_i[0][0] = 0x10000;
    remap.map_table_i[1][0] = 0x10000;
    orig_init_func(&remap);
    orig_func = remap.do_remap;
    if (!orig_func) {
        pa_log_warn("No reference remapping function, abort test");
        return;
    }

    init_func(&remap);
    func = remap.do_remap;
    if (!func || func == orig_func) {
        pa_log_warn("No remapping function, abort test");
        return;
    }

    run_remap_test_mono_stereo_float(&remap, func, orig_func, 0, TRUE, FALSE);
    run_remap_test_mono_stereo_float(&remap, func, orig_func, 1, TRUE, FALSE);
    run_remap_test_mono_stereo_float(&remap, func, orig_func, 2, TRUE, FALSE);
    run_remap_test_mono_stereo_float(&remap, func, orig_func, 3, TRUE, TRUE);
}

static void remap_test_mono_stereo_s16(
        pa_init_remap_func_t init_func,
        pa_init_remap_func_t orig_init_func) {

    pa_sample_format_t sf;
    pa_remap_t remap;
    pa_sample_spec iss, oss;
    pa_do_remap_func_t orig_func, func;

    iss.format = oss.format = sf = PA_SAMPLE_S16NE;
    iss.channels = 1;
    oss.channels = 2;
    remap.format = &sf;
    remap.i_ss = &iss;
    remap.o_ss = &oss;
    remap.map_table_f[0][0] = 1.0;
    remap.map_table_f[1][0] = 1.0;
    remap.map_table_i[0][0] = 0x10000;
    remap.map_table_i[1][0] = 0x10000;
    orig_init_func(&remap);
    orig_func = remap.do_remap;
    if (!orig_func) {
        pa_log_warn("No reference remapping function, abort test");
        return;
    }

    init_func(&remap);
    func = remap.do_remap;
    if (!func || func == orig_func) {
        pa_log_warn("No remapping function, abort test");
        return;
    }

    run_remap_test_mono_stereo_s16(&remap, func, orig_func, 0, TRUE, FALSE);
    run_remap_test_mono_stereo_s16(&remap, func, orig_func, 1, TRUE, FALSE);
    run_remap_test_mono_stereo_s16(&remap, func, orig_func, 2, TRUE, FALSE);
    run_remap_test_mono_stereo_s16(&remap, func, orig_func, 3, TRUE, TRUE);
}

#if defined (__i386__) || defined (__amd64__)
START_TEST (remap_mmx_test) {
    pa_cpu_x86_flag_t flags = 0;
    pa_init_remap_func_t init_func, orig_init_func;

    pa_cpu_get_x86_flags(&flags);
    if (!(flags & PA_CPU_X86_MMX)) {
        pa_log_info("MMX not supported. Skipping");
        return;
    }

    pa_log_debug("Checking MMX remap (float, mono->stereo)");
    orig_init_func = pa_get_init_remap_func();
    pa_remap_func_init_mmx(flags);
    init_func = pa_get_init_remap_func();
    remap_test_mono_stereo_float(init_func, orig_init_func);

    pa_log_debug("Checking MMX remap (s16, mono->stereo)");
    remap_test_mono_stereo_s16(init_func, orig_init_func);
}
END_TEST

START_TEST (remap_sse2_test) {
    pa_cpu_x86_flag_t flags = 0;
    pa_init_remap_func_t init_func, orig_init_func;

    pa_cpu_get_x86_flags(&flags);
    if (!(flags & PA_CPU_X86_SSE2)) {
        pa_log_info("SSE2 not supported. Skipping");
        return;
    }

    pa_log_debug("Checking SSE2 remap (float, mono->stereo)");
    orig_init_func = pa_get_init_remap_func();
    pa_remap_func_init_sse(flags);
    init_func = pa_get_init_remap_func();
    remap_test_mono_stereo_float(init_func, orig_init_func);

    pa_log_debug("Checking SSE2 remap (s16, mono->stereo)");
    remap_test_mono_stereo_s16(init_func, orig_init_func);
}
END_TEST
#endif /* defined (__i386__) || defined (__amd64__) */

#undef SAMPLES
#undef TIMES
#undef TIMES2
/* End remap tests */

/* Start mix tests */

/* Only ARM NEON has mix tests, so disable the related functions for other
 * architectures for now to avoid compiler warnings about unused functions. */
#if defined (__arm__) && defined (__linux__)
#ifdef HAVE_NEON

#define SAMPLES 1028
#define TIMES 1000
#define TIMES2 100

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

static void run_mix_test(
        pa_do_mix_func_t func,
        pa_do_mix_func_t orig_func,
        int align,
        int channels,
        pa_bool_t correct,
        pa_bool_t perf) {

    PA_DECLARE_ALIGNED(8, int16_t, in0[SAMPLES * 4]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, in1[SAMPLES * 4]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, out[SAMPLES * 4]) = { 0 };
    PA_DECLARE_ALIGNED(8, int16_t, out_ref[SAMPLES * 4]) = { 0 };
    int16_t *samples0, *samples1;
    int16_t *samples, *samples_ref;
    int nsamples;
    pa_mempool *pool;
    pa_memchunk c0, c1;
    pa_mix_info m[2];
    int i;

    pa_assert(channels == 1 || channels == 2 || channels == 4);

    /* Force sample alignment as requested */
    samples0 = in0 + (8 - align);
    samples1 = in1 + (8 - align);
    samples = out + (8 - align);
    samples_ref = out_ref + (8 - align);
    nsamples = channels * (SAMPLES - (8 - align));

    fail_unless((pool = pa_mempool_new(FALSE, 0)) != NULL, NULL);

    pa_random(samples0, nsamples * sizeof(int16_t));
    c0.memblock = pa_memblock_new_fixed(pool, samples0, nsamples * sizeof(int16_t), FALSE);
    c0.length = pa_memblock_get_length(c0.memblock);
    c0.index = 0;

    pa_random(samples1, nsamples * sizeof(int16_t));
    c1.memblock = pa_memblock_new_fixed(pool, samples1, nsamples * sizeof(int16_t), FALSE);
    c1.length = pa_memblock_get_length(c1.memblock);
    c1.index = 0;

    m[0].chunk = c0;
    m[0].volume.channels = channels;
    for (i = 0; i < channels; i++) {
        m[0].volume.values[i] = PA_VOLUME_NORM;
        m[0].linear[i].i = 0x5555;
    }

    m[1].chunk = c1;
    m[1].volume.channels = channels;
    for (i = 0; i < channels; i++) {
        m[1].volume.values[i] = PA_VOLUME_NORM;
        m[1].linear[i].i = 0x6789;
    }

    if (correct) {
        acquire_mix_streams(m, 2);
        orig_func(m, 2, channels, samples_ref, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);

        acquire_mix_streams(m, 2);
        func(m, 2, channels, samples, nsamples * sizeof(int16_t));
        release_mix_streams(m, 2);

        for (i = 0; i < nsamples; i++) {
            if (samples[i] != samples_ref[i]) {
                pa_log_debug("Correctness test failed: align=%d, channels=%d", align, channels);
                pa_log_debug("%d: %hd != %04hd (%hd + %hd)\n",
                    i,
                    samples[i], samples_ref[i],
                    samples0[i], samples1[i]);
                fail();
            }
        }
    }

    if (perf) {
        pa_log_debug("Testing %d-channel mixing performance with %d sample alignment", channels, align);

        PA_CPU_TEST_RUN_START("func", TIMES, TIMES2) {
            acquire_mix_streams(m, 2);
            func(m, 2, channels, samples, nsamples * sizeof(int16_t));
            release_mix_streams(m, 2);
        } PA_CPU_TEST_RUN_STOP

        PA_CPU_TEST_RUN_START("orig", TIMES, TIMES2) {
            acquire_mix_streams(m, 2);
            orig_func(m, 2, channels, samples_ref, nsamples * sizeof(int16_t));
            release_mix_streams(m, 2);
        } PA_CPU_TEST_RUN_STOP
    }

    pa_memblock_unref(c0.memblock);
    pa_memblock_unref(c1.memblock);

    pa_mempool_free(pool);
}
#endif /* HAVE_NEON */
#endif /* defined (__arm__) && defined (__linux__) */

#if defined (__arm__) && defined (__linux__)
#ifdef HAVE_NEON
START_TEST (mix_neon_test) {
    pa_do_mix_func_t orig_func, neon_func;
    pa_cpu_arm_flag_t flags = 0;

    pa_cpu_get_arm_flags(&flags);

    if (!(flags & PA_CPU_ARM_NEON)) {
        pa_log_info("NEON not supported. Skipping");
        return;
    }

    orig_func = pa_get_mix_func(PA_SAMPLE_S16NE);
    pa_mix_func_init_neon(flags);
    neon_func = pa_get_mix_func(PA_SAMPLE_S16NE);

    pa_log_debug("Checking NEON mix");
    run_mix_test(neon_func, orig_func, 7, 2, TRUE, TRUE);
}
END_TEST
#endif /* HAVE_NEON */
#endif /* defined (__arm__) && defined (__linux__) */
/* End mix tests */

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("CPU");

    /* Volume tests */
    tc = tcase_create("svolume");
#if defined (__i386__) || defined (__amd64__)
    tcase_add_test(tc, svolume_mmx_test);
    tcase_add_test(tc, svolume_sse_test);
#endif
#if defined (__arm__) && defined (__linux__)
    tcase_add_test(tc, svolume_arm_test);
#endif
    tcase_add_test(tc, svolume_orc_test);
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);

    /* Conversion tests */
    tc = tcase_create("sconv");
#if defined (__i386__) || defined (__amd64__)
    tcase_add_test(tc, sconv_sse2_test);
    tcase_add_test(tc, sconv_sse_test);
#endif
#if defined (__arm__) && defined (__linux__)
#if HAVE_NEON
    tcase_add_test(tc, sconv_neon_test);
#endif
#endif
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);

    /* Remap tests */
    tc = tcase_create("remap");
#if defined (__i386__) || defined (__amd64__)
    tcase_add_test(tc, remap_mmx_test);
    tcase_add_test(tc, remap_sse2_test);
#endif
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);
    /* Mix tests */
    tc = tcase_create("mix");
#if defined (__arm__) && defined (__linux__)
#if HAVE_NEON
    tcase_add_test(tc, mix_neon_test);
#endif
#endif
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
