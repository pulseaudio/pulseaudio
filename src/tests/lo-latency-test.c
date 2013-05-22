/***
  This file is part of PulseAudio.

  Copyright 2013 Collabora Ltd.
  Author: Arun Raghavan <arun.raghavan@collabora.co.uk>

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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <check.h>

#include <pulse/pulseaudio.h>
#include <pulse/mainloop.h>

/* for pa_make_realtime */
#include <pulsecore/core-util.h>

#define SAMPLE_HZ 44100
#define CHANNELS 2
#define N_OUT (SAMPLE_HZ * 1)

#define TONE_HZ (SAMPLE_HZ / 100)
#define PLAYBACK_LATENCY 25 /* ms */
#define CAPTURE_LATENCY 5 /* ms */

static pa_context *context = NULL;
static pa_stream *pstream, *rstream;
static pa_mainloop_api *mainloop_api = NULL;
static const char *context_name = NULL;

static float out[N_OUT][CHANNELS];
static int ppos = 0;

static int n_underflow = 0;
static int n_overflow = 0;

static struct timeval tv_out, tv_in;

static const pa_sample_spec sample_spec = {
    .format = PA_SAMPLE_FLOAT32,
    .rate = SAMPLE_HZ,
    .channels = CHANNELS,
};
static int ss, fs;

static void nop_free_cb(void *p) {}

static void underflow_cb(struct pa_stream *s, void *userdata) {
    fprintf(stderr, "Underflow\n");
    n_underflow++;
}

static void overflow_cb(struct pa_stream *s, void *userdata) {
    fprintf(stderr, "Overlow\n");
    n_overflow++;
}

static void write_cb(pa_stream *s, size_t nbytes, void *userdata) {
    int r, nsamp = nbytes / fs;

    if (ppos + nsamp > N_OUT) {
        r = pa_stream_write(s, &out[ppos][0], (N_OUT - ppos) * fs, nop_free_cb, 0, PA_SEEK_RELATIVE);
        nbytes -= (N_OUT - ppos) * fs;
        ppos = 0;
    }

    if (ppos == 0)
        pa_gettimeofday(&tv_out);

    r = pa_stream_write(s, &out[ppos][0], nbytes, nop_free_cb, 0, PA_SEEK_RELATIVE);
    fail_unless(r == 0);

    ppos = (ppos + nbytes / fs) % N_OUT;
}

static inline float rms(const float *s, int n) {
    float sq = 0;
    int i;

    for (i = 0; i < n; i++)
        sq += s[i] * s[i];

    return sqrtf(sq / n);
}

#define WINDOW (2 * CHANNELS)

static void read_cb(pa_stream *s, size_t nbytes, void *userdata) {
    static float last = 0.0f;
    const float *in;
    float cur;
    int r;
    unsigned int i = 0;
    size_t l;

    r = pa_stream_peek(s, (const void **)&in, &l);
    fail_unless(r == 0);

    if (l == 0)
        return;

#if 0
    {
        static int fd = -1;

        if (fd == -1) {
            fd = open("loopback.raw", O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
            fail_if(fd < 0);
        }

        r = write(fd, in, l);
    }
#endif

    do {
#if 0
        {
            int j;
            fprintf(stderr, "%g (", rms(in, WINDOW));
            for (j = 0; j < WINDOW; j++)
                fprintf(stderr, "%g ", in[j]);
            fprintf(stderr, ")\n");
        }
#endif
        if (i + (ss * WINDOW) < l)
            cur = rms(in, WINDOW);
        else
            cur = rms(in, (l - i)/ss);

        /* We leave the definition of 0 generous since the window might
         * straddle the 0->1 transition, raising the average power. We keep the
         * definition of 1 tight in this case and detect the transition in the
         * next round. */
        if (cur - last > 0.4f) {
            pa_gettimeofday(&tv_in);
            fprintf(stderr, "Latency %llu\n", (unsigned long long) pa_timeval_diff(&tv_in, &tv_out));
        }

        last = cur;
        in += WINDOW;
        i += ss * WINDOW;
    } while (i + (ss * WINDOW) <= l);

    pa_stream_drop(s);
}

/*
 * We run a simple volume calibration so that we know we can detect the signal
 * being played back. We start with the playback stream at 100% volume, and
 * capture at 0.
 *
 * First, we then play a sine wave and increase the capture volume till the
 * signal is clearly received.
 *
 * Next, we play back silence and make sure that the level is low enough to
 * distinguish from when playback is happening.
 *
 * Finally, we hand off to the real read/write callbacks to run the actual
 * test.
 */

enum {
    CALIBRATION_ONE,
    CALIBRATION_ZERO,
    CALIBRATION_DONE,
};

static int cal_state = CALIBRATION_ONE;

static void calibrate_write_cb(pa_stream *s, size_t nbytes, void *userdata) {
    int i, r, nsamp = nbytes / fs;
    float tmp[nsamp][2];
    static int count = 0;

    /* Write out a sine tone */
    for (i = 0; i < nsamp; i++)
        tmp[i][0] = tmp[i][1] = cal_state == CALIBRATION_ONE ? sinf(count++ * TONE_HZ * 2 * M_PI / SAMPLE_HZ) : 0.0f;

    r = pa_stream_write(s, &tmp, nbytes, nop_free_cb, 0, PA_SEEK_RELATIVE);
    fail_unless(r == 0);

    if (cal_state == CALIBRATION_DONE)
        pa_stream_set_write_callback(s, write_cb, NULL);
}

static void calibrate_read_cb(pa_stream *s, size_t nbytes, void *userdata) {
    static double v = 0;
    static int skip = 0, confirm;

    pa_cvolume vol;
    pa_operation *o;
    int r, nsamp;
    float *in;
    size_t l;

    r = pa_stream_peek(s, (const void **)&in, &l);
    fail_unless(r == 0);

    nsamp = l / fs;

    /* For each state or volume step change, throw out a few samples so we know
     * we're seeing the changed samples. */
    if (skip++ < 100)
        goto out;
    else
        skip = 0;

    switch (cal_state) {
        case CALIBRATION_ONE:
            /* Try to detect the sine wave. RMS is 0.5, */
            if (rms(in, nsamp) < 0.40f) {
                confirm = 0;
                v += 0.02f;

                if (v > 1.0) {
                    fprintf(stderr, "Capture signal too weak at 100%% volume (%g). Giving up.\n", rms(in, nsamp));
                    fail();
                }

                pa_cvolume_set(&vol, CHANNELS, v * PA_VOLUME_NORM);
                o = pa_context_set_source_output_volume(context, pa_stream_get_index(s), &vol, NULL, NULL);
                fail_if(o == NULL);
                pa_operation_unref(o);
            } else {
                /* Make sure the signal strength is steadily above our threshold */
                if (++confirm > 5) {
#if 0
                    fprintf(stderr, "Capture volume = %g (%g)\n", v, rms(in, nsamp));
#endif
                    cal_state = CALIBRATION_ZERO;
                }
            }

            break;

        case CALIBRATION_ZERO:
            /* Now make sure silence doesn't trigger a false positive because
             * of noise. */
            if (rms(in, nsamp) > 0.1f) {
                fprintf(stderr, "Too much noise on capture (%g). Giving up.\n", rms(in, nsamp));
                fail();
            }

            cal_state = CALIBRATION_DONE;
            pa_stream_set_read_callback(s, read_cb, NULL);

            break;

        default:
            break;
    }

out:
    pa_stream_drop(s);
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata) {
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
        case PA_STREAM_TERMINATED:
            break;

        case PA_STREAM_READY: {
            pa_cvolume vol;
            pa_operation *o;

            /* Set volumes for calibration */
            if (!userdata) {
                pa_cvolume_set(&vol, CHANNELS, PA_VOLUME_NORM);
                o = pa_context_set_sink_input_volume(context, pa_stream_get_index(s), &vol, NULL, NULL);
            } else {
                pa_cvolume_set(&vol, CHANNELS, pa_sw_volume_from_linear(0.0));
                o = pa_context_set_source_output_volume(context, pa_stream_get_index(s), &vol, NULL, NULL);
            }

            if (!o) {
                fprintf(stderr, "Could not set stream volume: %s\n", pa_strerror(pa_context_errno(context)));
                fail();
            } else
                pa_operation_unref(o);

            break;
        }

        case PA_STREAM_FAILED:
        default:
            fprintf(stderr, "Stream error: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            fail();
    }
}

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    fail_unless(c != NULL);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY: {
            pa_buffer_attr buffer_attr;

            pa_make_realtime(4);

            /* Create playback stream */
            buffer_attr.maxlength = -1;
            buffer_attr.tlength = SAMPLE_HZ * fs * PLAYBACK_LATENCY / 1000;
            buffer_attr.prebuf = 0; /* Setting prebuf to 0 guarantees us the stream will run synchronously, no matter what */
            buffer_attr.minreq = -1;
            buffer_attr.fragsize = -1;

            pstream = pa_stream_new(c, "loopback: play", &sample_spec, NULL);
            fail_unless(pstream != NULL);
            pa_stream_set_state_callback(pstream, stream_state_callback, (void *) 0);
            pa_stream_set_write_callback(pstream, calibrate_write_cb, NULL);
            pa_stream_set_underflow_callback(pstream, underflow_cb, userdata);

            pa_stream_connect_playback(pstream, getenv("TEST_SINK"), &buffer_attr,
                    PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE, NULL, NULL);

            /* Create capture stream */
            buffer_attr.maxlength = -1;
            buffer_attr.tlength = (uint32_t) -1;
            buffer_attr.prebuf = 0;
            buffer_attr.minreq = (uint32_t) -1;
            buffer_attr.fragsize = SAMPLE_HZ * fs * CAPTURE_LATENCY / 1000;

            rstream = pa_stream_new(c, "loopback: rec", &sample_spec, NULL);
            fail_unless(rstream != NULL);
            pa_stream_set_state_callback(rstream, stream_state_callback, (void *) 1);
            pa_stream_set_read_callback(rstream, calibrate_read_cb, NULL);
            pa_stream_set_overflow_callback(rstream, overflow_cb, userdata);

            pa_stream_connect_record(rstream, getenv("TEST_SOURCE"), &buffer_attr,
                    PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE);

            break;
        }

        case PA_CONTEXT_TERMINATED:
            mainloop_api->quit(mainloop_api, 0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, "Context error: %s\n", pa_strerror(pa_context_errno(c)));
            fail();
    }
}

START_TEST (loopback_test) {
    pa_mainloop* m = NULL;
    int i, ret = 0, pulse_hz = SAMPLE_HZ / 1000;

    /* Generate a square pulse */
    for (i = 0; i < N_OUT; i++)
        if (i < pulse_hz)
            out[i][0] = out[i][1] = 1.0f;
        else
            out[i][0] = out[i][1] = 0.0f;

    ss = pa_sample_size(&sample_spec);
    fs = pa_frame_size(&sample_spec);

    pstream = NULL;

    /* Set up a new main loop */
    m = pa_mainloop_new();
    fail_unless(m != NULL);

    mainloop_api = pa_mainloop_get_api(m);

    context = pa_context_new(mainloop_api, context_name);
    fail_unless(context != NULL);

    pa_context_set_state_callback(context, context_state_callback, NULL);

    /* Connect the context */
    if (pa_context_connect(context, NULL, 0, NULL) < 0) {
        fprintf(stderr, "pa_context_connect() failed.\n");
        goto quit;
    }

    if (pa_mainloop_run(m, &ret) < 0)
        fprintf(stderr, "pa_mainloop_run() failed.\n");

quit:
    pa_context_unref(context);

    if (pstream)
        pa_stream_unref(pstream);

    pa_mainloop_free(m);

    fail_unless(ret == 0);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    context_name = argv[0];

    s = suite_create("Loopback");
    tc = tcase_create("loopback");
    tcase_add_test(tc, loopback_test);
    tcase_set_timeout(tc, 5 * 60);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_set_fork_status(sr, CK_NOFORK);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
