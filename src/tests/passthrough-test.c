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
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>

#include <check.h>

#include <pulse/pulseaudio.h>

#include <pulsecore/core-util.h>

#define SINK_NAME "passthrough-test"

#define RATE 48000
#define CHANNELS 6

#define WAIT_FOR_OPERATION(o)                                           \
    do {                                                                \
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {     \
            pa_threaded_mainloop_wait(mainloop);                        \
        }                                                               \
                                                                        \
        fail_unless(pa_operation_get_state(o) == PA_OPERATION_DONE);    \
        pa_operation_unref(o);                                          \
    } while (false)

static pa_threaded_mainloop *mainloop = NULL;
static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;
static uint32_t module_idx = PA_INVALID_INDEX;
static int sink_num = 0;
static char sink_name[256] = { 0, };
static const char *bname = NULL;

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    fail_unless(c != NULL);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            fprintf(stderr, "Connection established.\n");
            pa_threaded_mainloop_signal(mainloop, false);
            break;

        case PA_CONTEXT_TERMINATED:
            mainloop_api->quit(mainloop_api, 0);
            pa_threaded_mainloop_signal(mainloop, false);
            break;

        case PA_CONTEXT_FAILED:
            mainloop_api->quit(mainloop_api, 0);
            pa_threaded_mainloop_signal(mainloop, false);
            fprintf(stderr, "Context error: %s\n", pa_strerror(pa_context_errno(c)));
            fail();
            break;

        default:
            fail();
    }
}

static void module_index_cb(pa_context *c, uint32_t idx, void *userdata) {
    fail_unless(idx != PA_INVALID_INDEX);

    module_idx = idx;

    pa_threaded_mainloop_signal(mainloop, false);
}

static void success_cb(pa_context *c, int success, void *userdata) {
    fail_unless(success != 0);

    pa_threaded_mainloop_signal(mainloop, false);
}

static void passthrough_teardown() {
    pa_operation *o;

    pa_threaded_mainloop_lock(mainloop);

    if (module_idx != PA_INVALID_INDEX) {
        o = pa_context_unload_module(context, module_idx, success_cb, NULL);
        WAIT_FOR_OPERATION(o);
    }

    pa_context_disconnect(context);
    pa_context_unref(context);

    pa_threaded_mainloop_unlock(mainloop);

    pa_threaded_mainloop_stop(mainloop);
    pa_threaded_mainloop_free(mainloop);
}

static void passthrough_setup() {
    char modargs[128];
    pa_operation *o;
    int r;

    /* Set up a new main loop */
    mainloop = pa_threaded_mainloop_new();
    fail_unless(mainloop != NULL);

    mainloop_api = pa_threaded_mainloop_get_api(mainloop);

    pa_threaded_mainloop_lock(mainloop);

    pa_threaded_mainloop_start(mainloop);

    context = pa_context_new(mainloop_api, bname);
    fail_unless(context != NULL);

    pa_context_set_state_callback(context, context_state_callback, NULL);

    /* Connect the context */
    r = pa_context_connect(context, NULL, 0, NULL);
    fail_unless(r == 0);

    pa_threaded_mainloop_wait(mainloop);

    fail_unless(pa_context_get_state(context) == PA_CONTEXT_READY);

    pa_snprintf(sink_name, sizeof(sink_name), "%s-%d", SINK_NAME, sink_num);
    pa_snprintf(modargs, sizeof(modargs), "sink_name='%s' formats='ac3-iec61937, format.rate=\"[32000, 44100, 48000]\" format.channels=\"6\"; pcm'", sink_name);

    o = pa_context_load_module(context, "module-null-sink", modargs, module_index_cb, NULL);
    WAIT_FOR_OPERATION(o);

    pa_threaded_mainloop_unlock(mainloop);

    return;
}

static void nop_free_cb(void *p) {}

static void underflow_cb(struct pa_stream *s, void *userdata) {
    fprintf(stderr, "Stream finished\n");
    pa_threaded_mainloop_signal(mainloop, false);
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata) {
    /* We fill in fake AC3 data in terms of the corresponding PCM sample spec (S16LE, 2ch, at the given rate) */
    int16_t data[RATE * 2] = { 0, }; /* one second space */

    fail_unless(s != NULL);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
            break;

        case PA_STREAM_TERMINATED:
            pa_threaded_mainloop_signal(mainloop, false);
            break;

        case PA_STREAM_READY: {
            int r;

            r = pa_stream_write(s, data, sizeof(data), nop_free_cb, 0, PA_SEEK_ABSOLUTE);
            fail_unless(r == 0);

            /* Be notified when this stream is drained */
            pa_stream_set_underflow_callback(s, underflow_cb, userdata);

            pa_threaded_mainloop_signal(mainloop, false);
            break;
        }

        case PA_STREAM_FAILED:
            fprintf(stderr, "Stream error: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            pa_threaded_mainloop_signal(mainloop, false);
            break;

        default:
            fail();
    }
}

static pa_stream* connect_stream() {
    int r;
    pa_stream *s;
    pa_format_info *formats[1];

    pa_threaded_mainloop_lock(mainloop);

    formats[0] = pa_format_info_new();
    formats[0]->encoding = PA_ENCODING_AC3_IEC61937;
    /* We set rate and channels to test that negotiation actually works. This
     * must correspond to the rate and channels we configure module-null-sink
     * for above. */
    pa_format_info_set_rate(formats[0], RATE);
    pa_format_info_set_channels(formats[0], CHANNELS);

    s = pa_stream_new_extended(context, "passthrough test", formats, 1, NULL);
    fail_unless(s != NULL);

    pa_stream_set_state_callback(s, stream_state_callback, NULL);
    r = pa_stream_connect_playback(s, sink_name, NULL, PA_STREAM_NOFLAGS, NULL, NULL);

    fail_unless(r == 0);

    pa_threaded_mainloop_wait(mainloop);

    fail_unless(pa_stream_get_state(s) == PA_STREAM_READY);

    pa_threaded_mainloop_unlock(mainloop);

    return s;
}

static void disconnect_stream(pa_stream *s) {
    int r;

    pa_threaded_mainloop_lock(mainloop);

    r = pa_stream_disconnect(s);
    fail_unless(r == 0);

    pa_threaded_mainloop_wait(mainloop);
    fail_unless(pa_stream_get_state(s) == PA_STREAM_TERMINATED);

    pa_stream_unref(s);

    pa_threaded_mainloop_unlock(mainloop);
}

START_TEST (passthrough_playback_test) {
    /* Create a passthrough stream, and make sure format negotiation actually
     * works */
    pa_stream *stream;

    stream = connect_stream();

    /* Wait for underflow_cb() */
    pa_threaded_mainloop_lock(mainloop);
    pa_threaded_mainloop_wait(mainloop);
    fail_unless(pa_stream_get_state(stream) == PA_STREAM_READY);
    pa_threaded_mainloop_unlock(mainloop);

    disconnect_stream(stream);
}
END_TEST

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    pa_cvolume *v = (pa_cvolume *) userdata;

    if (eol)
        return;

    *v = i->volume;

    pa_threaded_mainloop_signal(mainloop, false);
}

static void get_sink_volume(pa_cvolume *v) {
    pa_operation *o;

    pa_threaded_mainloop_lock(mainloop);

    o = pa_context_get_sink_info_by_name(context, sink_name, sink_info_cb, v);
    WAIT_FOR_OPERATION(o);

    pa_threaded_mainloop_unlock(mainloop);
}

START_TEST (passthrough_volume_test) {
    /* Set a non-100% volume of the sink before playback, create a passthrough
     * stream, make sure volume gets set to 100%, and then restored when the
     * stream goes away */
    pa_stream *stream;
    pa_operation *o;
    pa_cvolume volume, tmp;

    pa_threaded_mainloop_lock(mainloop);

    pa_cvolume_set(&volume, 2, PA_VOLUME_NORM / 2);
    o = pa_context_set_sink_volume_by_name(context, sink_name, &volume, success_cb, NULL);
    WAIT_FOR_OPERATION(o);

    pa_threaded_mainloop_unlock(mainloop);

    stream = connect_stream();

    pa_threaded_mainloop_lock(mainloop);
    pa_threaded_mainloop_wait(mainloop);
    fail_unless(PA_STREAM_IS_GOOD(pa_stream_get_state(stream)));
    pa_threaded_mainloop_unlock(mainloop);

    get_sink_volume(&tmp);
    fail_unless(pa_cvolume_is_norm(&tmp));

    disconnect_stream(stream);

    get_sink_volume(&tmp);
    fail_unless(pa_cvolume_equal(&volume, &tmp));
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    bname = argv[0];

    s = suite_create("Passthrough");
    tc = tcase_create("passthrough");
    tcase_add_checked_fixture(tc, passthrough_setup, passthrough_teardown);
    tcase_add_test(tc, passthrough_playback_test);
    sink_num++;
    tcase_add_test(tc, passthrough_volume_test);
    tcase_set_timeout(tc, 5);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
