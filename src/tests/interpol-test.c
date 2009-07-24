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

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>

#include <pulse/pulseaudio.h>
#include <pulse/mainloop.h>

#include <pulsecore/thread.h>

#define INTERPOLATE
//#define CORK

static pa_context *context = NULL;
static pa_stream *stream = NULL;
static pa_mainloop_api *mainloop_api = NULL;
static pa_bool_t playback = TRUE;
static pa_usec_t latency = 0;

static void stream_write_cb(pa_stream *p, size_t nbytes, void *userdata) {
    /* Just some silence */

    for (;;) {
        void *data;

        pa_assert_se((nbytes = pa_stream_writable_size(p)) != (size_t) -1);

        if (nbytes <= 0)
            break;

        pa_assert_se(pa_stream_begin_write(p, &data, &nbytes) == 0);
        pa_memzero(data, nbytes);
        pa_assert_se(pa_stream_write(p, data, nbytes, NULL, 0, PA_SEEK_RELATIVE) == 0);
    }
}

static void stream_read_cb(pa_stream *p, size_t nbytes, void *userdata) {
    /* We don't care about the data, just drop it */

    for (;;) {
        const void *data;

        pa_assert_se((nbytes = pa_stream_readable_size(p)) != (size_t) -1);

        if (nbytes <= 0)
            break;

        pa_assert_se(pa_stream_peek(p, &data, &nbytes) == 0);
        pa_assert_se(pa_stream_drop(p) == 0);
    }
}

static void stream_latency_cb(pa_stream *p, void *userdata) {
#ifndef INTERPOLATE
    pa_operation *o;

    o = pa_stream_update_timing_info(p, NULL, NULL);
    pa_operation_unref(o);
#endif
}

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY: {
            pa_stream_flags_t flags = PA_STREAM_AUTO_TIMING_UPDATE;
            pa_buffer_attr attr;
            static const pa_sample_spec ss = {
                .format = PA_SAMPLE_S16LE,
                .rate = 44100,
                .channels = 2
            };

            pa_zero(attr);
            attr.maxlength = (uint32_t) -1;
            attr.tlength = latency > 0 ? (uint32_t) pa_usec_to_bytes(latency, &ss) : (uint32_t) -1;
            attr.prebuf = (uint32_t) -1;
            attr.minreq = (uint32_t) -1;
            attr.fragsize = (uint32_t) -1;

#ifdef INTERPOLATE
            flags |= PA_STREAM_INTERPOLATE_TIMING;
#endif

            if (latency > 0)
                flags |= PA_STREAM_ADJUST_LATENCY;

            fprintf(stderr, "Connection established.\n");

            pa_assert_se(stream = pa_stream_new(c, "interpol-test", &ss, NULL));

            if (playback) {
                pa_assert_se(pa_stream_connect_playback(stream, NULL, &attr, flags, NULL, NULL) == 0);
                pa_stream_set_write_callback(stream, stream_write_cb, NULL);
            } else {
                pa_assert_se(pa_stream_connect_record(stream, NULL, &attr, flags) == 0);
                pa_stream_set_read_callback(stream, stream_read_cb, NULL);
            }

            pa_stream_set_latency_update_callback(stream, stream_latency_cb, NULL);

            break;
        }

        case PA_CONTEXT_TERMINATED:
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, "Context error: %s\n", pa_strerror(pa_context_errno(c)));
            abort();
    }
}

int main(int argc, char *argv[]) {
    pa_threaded_mainloop* m = NULL;
    int k;
    struct timeval start, last_info = { 0, 0 };
    pa_usec_t old_t = 0, old_rtc = 0;
#ifdef CORK
    pa_bool_t corked = FALSE;
#endif

    pa_log_set_level(PA_LOG_DEBUG);

    playback = argc <= 1 || !pa_streq(argv[1], "-r");

    latency =
        (argc >= 2 && !pa_streq(argv[1], "-r")) ? atoi(argv[1]) :
        (argc >= 3 ? atoi(argv[2]) : 0);

    /* Set up a new main loop */
    pa_assert_se(m = pa_threaded_mainloop_new());
    pa_assert_se(mainloop_api = pa_threaded_mainloop_get_api(m));
    pa_assert_se(context = pa_context_new(mainloop_api, argv[0]));

    pa_context_set_state_callback(context, context_state_callback, NULL);

    pa_assert_se(pa_context_connect(context, NULL, 0, NULL) >= 0);

    pa_gettimeofday(&start);

    pa_assert_se(pa_threaded_mainloop_start(m) >= 0);

/* #ifdef CORK */
    for (k = 0; k < 20000; k++)
/* #else */
/*     for (k = 0; k < 2000; k++) */
/* #endif */
    {
        pa_bool_t success = FALSE, changed = FALSE;
        pa_usec_t t, rtc, d;
        struct timeval now, tv;
        pa_bool_t playing = FALSE;

        pa_threaded_mainloop_lock(m);

        if (stream) {
            const pa_timing_info *info;

            if (pa_stream_get_time(stream, &t) >= 0 &&
                pa_stream_get_latency(stream, &d, NULL) >= 0)
                success = TRUE;

            if ((info = pa_stream_get_timing_info(stream))) {
                if (memcmp(&last_info, &info->timestamp, sizeof(struct timeval))) {
                    changed = TRUE;
                    last_info = info->timestamp;
                }
                if (info->playing)
                    playing = TRUE;
            }
        }

        pa_threaded_mainloop_unlock(m);

        pa_gettimeofday(&now);

        if (success) {
#ifdef CORK
            pa_bool_t cork_now;
#endif
            rtc = pa_timeval_diff(&now, &start);
            printf("%i\t%llu\t%llu\t%llu\t%llu\t%lli\t%u\t%u\t%llu\t%llu\n", k,
                   (unsigned long long) rtc,
                   (unsigned long long) t,
                   (unsigned long long) (rtc-old_rtc),
                   (unsigned long long) (t-old_t),
                   (signed long long) rtc - (signed long long) t,
                   changed,
                   playing,
                   (unsigned long long) latency,
                   (unsigned long long) d);

            fflush(stdout);
            old_t = t;
            old_rtc = rtc;

#ifdef CORK
            cork_now = (rtc / (2*PA_USEC_PER_SEC)) % 2 == 1;

            if (corked != cork_now) {
                pa_threaded_mainloop_lock(m);
                pa_operation_unref(pa_stream_cork(stream, cork_now, NULL, NULL));
                pa_threaded_mainloop_unlock(m);

                pa_log(cork_now ? "Corking" : "Uncorking");

                corked = cork_now;
            }
#endif
        }

        /* Spin loop, ugly but normal usleep() is just too badly grained */

        tv = now;
        while (pa_timeval_diff(pa_gettimeofday(&now), &tv) < 1000)
            pa_thread_yield();
    }

    if (m)
        pa_threaded_mainloop_stop(m);

    if (stream) {
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }

    if (context) {
        pa_context_disconnect(context);
        pa_context_unref(context);
    }

    if (m)
        pa_threaded_mainloop_free(m);

    return 0;
}
