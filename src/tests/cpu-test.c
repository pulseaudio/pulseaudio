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
#include <pulsecore/sample-util.h>

/* Common defines for svolume tests */
#define CHANNELS 2
#define SAMPLES 1022
#define TIMES 1000
#define TIMES2 100
#define PADDING 16

static void run_volume_test(pa_do_volume_func_t func, pa_do_volume_func_t orig_func) {
    int16_t samples[SAMPLES];
    int16_t samples_ref[SAMPLES];
    int16_t samples_orig[SAMPLES];
    int32_t volumes[CHANNELS + PADDING];
    int i, j, padding;
    pa_usec_t start, stop;
    int k;
    pa_usec_t min = INT_MAX, max = 0;
    double s1 = 0, s2 = 0;


    pa_random(samples, sizeof(samples));
    memcpy(samples_ref, samples, sizeof(samples));
    memcpy(samples_orig, samples, sizeof(samples));

    for (i = 0; i < CHANNELS; i++)
        volumes[i] = PA_CLAMP_VOLUME((pa_volume_t)(rand() >> 15));
    for (padding = 0; padding < PADDING; padding++, i++)
        volumes[i] = volumes[padding];

    orig_func(samples_ref, volumes, CHANNELS, sizeof(samples));
    func(samples, volumes, CHANNELS, sizeof(samples));
    for (i = 0; i < SAMPLES; i++) {
        if (samples[i] != samples_ref[i]) {
            printf("%d: %04x != %04x (%04x * %08x)\n", i, samples[i], samples_ref[i],
                  samples_orig[i], volumes[i % CHANNELS]);
            fail();
        }
    }

    for (k = 0; k < TIMES2; k++) {
        start = pa_rtclock_now();
        for (j = 0; j < TIMES; j++) {
            memcpy(samples, samples_orig, sizeof(samples));
            func(samples, volumes, CHANNELS, sizeof(samples));
        }
        stop = pa_rtclock_now();

        if (min > (stop - start)) min = stop - start;
        if (max < (stop - start)) max = stop - start;
        s1 += stop - start;
        s2 += (stop - start) * (stop - start);
    }
    pa_log_debug("func: %llu usec (min = %llu, max = %llu, stddev = %g).", (long long unsigned int)s1,
            (long long unsigned int)min, (long long unsigned int)max, sqrt(TIMES2 * s2 - s1 * s1) / TIMES2);

    min = INT_MAX; max = 0;
    s1 = s2 = 0;
    for (k = 0; k < TIMES2; k++) {
        start = pa_rtclock_now();
        for (j = 0; j < TIMES; j++) {
            memcpy(samples_ref, samples_orig, sizeof(samples));
            orig_func(samples_ref, volumes, CHANNELS, sizeof(samples));
        }
        stop = pa_rtclock_now();

        if (min > (stop - start)) min = stop - start;
        if (max < (stop - start)) max = stop - start;
        s1 += stop - start;
        s2 += (stop - start) * (stop - start);
    }
    pa_log_debug("orig: %llu usec (min = %llu, max = %llu, stddev = %g).", (long long unsigned int)s1,
            (long long unsigned int)min, (long long unsigned int)max, sqrt(TIMES2 * s2 - s1 * s1) / TIMES2);

    fail_unless(memcmp(samples_ref, samples, sizeof(samples)) == 0);
}

START_TEST (svolume_mmx_test) {
    pa_do_volume_func_t orig_func, mmx_func;
    pa_cpu_x86_flag_t flags = 0;

    pa_cpu_get_x86_flags(&flags);

    if (!((flags & PA_CPU_X86_MMX) && (flags & PA_CPU_X86_CMOV))) {
        pa_log_info("MMX/CMOV not supported. Skipping");
        return;
    }

    orig_func = pa_get_volume_func(PA_SAMPLE_S16NE);
    pa_volume_func_init_mmx(flags);
    mmx_func = pa_get_volume_func(PA_SAMPLE_S16NE);

    pa_log_debug("Checking MMX svolume");
    run_volume_test(mmx_func, orig_func);
}
END_TEST

START_TEST (svolume_sse_test) {
    pa_do_volume_func_t orig_func, sse_func;
    pa_cpu_x86_flag_t flags = 0;

    pa_cpu_get_x86_flags(&flags);

    if (!(flags & PA_CPU_X86_SSE2)) {
        pa_log_info("SSE2 not supported. Skipping");
        return;
    }

    orig_func = pa_get_volume_func(PA_SAMPLE_S16NE);
    pa_volume_func_init_sse(flags);
    sse_func = pa_get_volume_func(PA_SAMPLE_S16NE);

    pa_log_debug("Checking SSE2 svolume");
    run_volume_test(sse_func, orig_func);
}
END_TEST

START_TEST (svolume_orc_test) {
    pa_do_volume_func_t orig_func, orc_func;
    pa_cpu_info cpu_info;

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
    run_volume_test(orc_func, orig_func);
}
END_TEST

#undef CHANNELS
#undef SAMPLES
#undef TIMES
#undef TIMES2
#undef PADDING
/* End svolume tests */

START_TEST (sconv_sse_test) {
#define SAMPLES 1019
#define TIMES 1000

    int16_t samples[SAMPLES];
    int16_t samples_ref[SAMPLES];
    float floats[SAMPLES];
    int i;
    pa_usec_t start, stop;
    pa_convert_func_t orig_func, sse_func;
    pa_cpu_x86_flag_t flags = 0;

    pa_cpu_get_x86_flags(&flags);

    if (!(flags & PA_CPU_X86_SSE2)) {
        pa_log_info("SSE2 not supported. Skipping");
        return;
    }

    pa_log_debug("Checking SSE sconv (%zd)\n", sizeof(samples));

    memset(samples_ref, 0, sizeof(samples_ref));
    memset(samples, 0, sizeof(samples));

    for (i = 0; i < SAMPLES; i++) {
        floats[i] = 2.1f * (rand()/(float) RAND_MAX - 0.5f);
    }

    orig_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);
    pa_convert_func_init_sse(flags);
    sse_func = pa_get_convert_from_float32ne_function(PA_SAMPLE_S16LE);

    orig_func(SAMPLES, floats, samples_ref);
    sse_func(SAMPLES, floats, samples);

    for (i = 0; i < SAMPLES; i++) {
        if (samples[i] != samples_ref[i]) {
            printf ("%d: %04x != %04x (%f)\n", i, samples[i], samples_ref[i],
                      floats[i]);
            fail();
        }
    }

    start = pa_rtclock_now();
    for (i = 0; i < TIMES; i++) {
        sse_func(SAMPLES, floats, samples);
    }
    stop = pa_rtclock_now();
    pa_log_debug("SSE: %llu usec.", (long long unsigned int)(stop - start));

    start = pa_rtclock_now();
    for (i = 0; i < TIMES; i++) {
        orig_func(SAMPLES, floats, samples_ref);
    }
    stop = pa_rtclock_now();
    pa_log_debug("ref: %llu usec.", (long long unsigned int)(stop - start));

#undef SAMPLES
#undef TIMES
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("CPU");
    tc = tcase_create("x86");
    tcase_add_test(tc, svolume_mmx_test);
    tcase_add_test(tc, svolume_sse_test);
    tcase_add_test(tc, svolume_orc_test);
    tcase_add_test(tc, sconv_sse_test);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
