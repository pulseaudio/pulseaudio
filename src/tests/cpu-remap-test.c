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

#include <pulsecore/cpu-x86.h>
#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/remap.h>

#include "runtime-test-util.h"

#define SAMPLES 1028
#define TIMES 1000
#define TIMES2 100

static void run_remap_test_mono_stereo_float(
        pa_remap_t *remap_func,
        pa_remap_t *remap_orig,
        int align,
        bool correct,
        bool perf) {

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
        remap_orig->do_remap(remap_orig, stereo_ref, mono, nsamples);
        remap_func->do_remap(remap_func, stereo, mono, nsamples);

        for (i = 0; i < nsamples * 2; i++) {
            if (fabsf(stereo[i] - stereo_ref[i]) > 0.0001f) {
                pa_log_debug("Correctness test failed: align=%d", align);
                pa_log_debug("%d: %.24f != %.24f (%.24f)\n", i, stereo[i], stereo_ref[i], mono[i]);
                fail();
            }
        }
    }

    if (perf) {
        pa_log_debug("Testing remap performance with %d sample alignment", align);

        PA_RUNTIME_TEST_RUN_START("func", TIMES, TIMES2) {
            remap_func->do_remap(remap_func, stereo, mono, nsamples);
        } PA_RUNTIME_TEST_RUN_STOP

        PA_RUNTIME_TEST_RUN_START("orig", TIMES, TIMES2) {
            remap_orig->do_remap(remap_orig, stereo_ref, mono, nsamples);
        } PA_RUNTIME_TEST_RUN_STOP
    }
}

static void run_remap_test_mono_stereo_s16(
        pa_remap_t *remap_func,
        pa_remap_t *remap_orig,
        int align,
        bool correct,
        bool perf) {

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
        remap_orig->do_remap(remap_orig, stereo_ref, mono, nsamples);
        remap_func->do_remap(remap_func, stereo, mono, nsamples);

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

        PA_RUNTIME_TEST_RUN_START("func", TIMES, TIMES2) {
            remap_func->do_remap(remap_orig, stereo, mono, nsamples);
        } PA_RUNTIME_TEST_RUN_STOP

        PA_RUNTIME_TEST_RUN_START("orig", TIMES, TIMES2) {
            remap_func->do_remap(remap_func, stereo_ref, mono, nsamples);
        } PA_RUNTIME_TEST_RUN_STOP
    }
}

static void setup_remap_mono_stereo(pa_remap_t *m, pa_sample_format_t f) {
    m->format = f;
    m->i_ss.channels = 1;
    m->o_ss.channels = 2;
    m->map_table_f[0][0] = 1.0f;
    m->map_table_f[1][0] = 1.0f;
    m->map_table_i[0][0] = 0x10000;
    m->map_table_i[1][0] = 0x10000;
}

static void remap_test_mono_stereo_float(
        pa_init_remap_func_t init_func,
        pa_init_remap_func_t orig_init_func) {

    pa_remap_t remap_orig, remap_func;

    setup_remap_mono_stereo(&remap_orig, PA_SAMPLE_FLOAT32NE);
    orig_init_func(&remap_orig);
    if (!remap_orig.do_remap) {
        pa_log_warn("No reference remapping function, abort test");
        return;
    }

    setup_remap_mono_stereo(&remap_func, PA_SAMPLE_FLOAT32NE);
    init_func(&remap_func);
    if (!remap_func.do_remap || remap_func.do_remap == remap_orig.do_remap) {
        pa_log_warn("No remapping function, abort test");
        return;
    }

    run_remap_test_mono_stereo_float(&remap_func, &remap_orig, 0, true, false);
    run_remap_test_mono_stereo_float(&remap_func, &remap_orig, 1, true, false);
    run_remap_test_mono_stereo_float(&remap_func, &remap_orig, 2, true, false);
    run_remap_test_mono_stereo_float(&remap_func, &remap_orig, 3, true, true);
}

static void remap_test_mono_stereo_s16(
        pa_init_remap_func_t init_func,
        pa_init_remap_func_t orig_init_func) {

    pa_remap_t remap_orig, remap_func;

    setup_remap_mono_stereo(&remap_orig, PA_SAMPLE_S16NE);
    orig_init_func(&remap_orig);
    if (!remap_orig.do_remap) {
        pa_log_warn("No reference remapping function, abort test");
        return;
    }

    init_func(&remap_func);
    if (!remap_func.do_remap || remap_func.do_remap == remap_orig.do_remap) {
        pa_log_warn("No remapping function, abort test");
        return;
    }

    run_remap_test_mono_stereo_s16(&remap_func, &remap_orig, 0, true, false);
    run_remap_test_mono_stereo_s16(&remap_func, &remap_orig, 1, true, false);
    run_remap_test_mono_stereo_s16(&remap_func, &remap_orig, 2, true, false);
    run_remap_test_mono_stereo_s16(&remap_func, &remap_orig, 3, true, true);
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

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("CPU");

    tc = tcase_create("remap");
#if defined (__i386__) || defined (__amd64__)
    tcase_add_test(tc, remap_mmx_test);
    tcase_add_test(tc, remap_sse2_test);
#endif
    tcase_set_timeout(tc, 120);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
