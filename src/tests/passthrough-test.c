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

#include <check.h>

#include <pulse/pulseaudio.h>

#include "test-util.h"

#define SINK_NAME "passthrough-test"

#define RATE 48000
#define CHANNELS 6

static pa_test_context *ctx = NULL;
static uint32_t sink_idx = PA_INVALID_INDEX;
static const char *bname = NULL;
int16_t data[RATE * 2] = { 0.0, }; /* one second space */

static void success_cb(pa_context *c, int success, void *userdata) {
    fail_unless(success != 0);

    pa_threaded_mainloop_signal(ctx->mainloop, false);
}

static void passthrough_teardown() {
    pa_test_context_free(ctx);
}

static void passthrough_setup() {
    ctx = pa_test_context_new(bname);

    sink_idx = pa_test_context_load_null_sink(ctx,
            "formats='ac3-iec61937, format.rate=\"[32000, 44100, 48000]\" format.channels=\"6\"; pcm'");
}

static pa_stream* connect_stream() {
    pa_stream *s;
    pa_format_info *format;

    format = pa_format_info_new();
    format->encoding = PA_ENCODING_AC3_IEC61937;
    /* We set rate and channels to test that negotiation actually works. This
     * must correspond to the rate and channels we configure module-null-sink
     * for above. */
    pa_format_info_set_rate(format, RATE);
    pa_format_info_set_channels(format, CHANNELS);

    s = pa_test_context_create_stream(ctx, "passthrough test", sink_idx, format, PA_STREAM_NOFLAGS, data, sizeof(data));
    fail_unless(s != NULL);

    pa_format_info_free(format);

    return s;
}

static void disconnect_stream(pa_stream *s) {
    pa_test_context_destroy_stream(ctx, s);
}

START_TEST (passthrough_playback_test) {
    /* Create a passthrough stream, and make sure format negotiation actually
     * works */
    pa_stream *stream;

    stream = connect_stream();

    /* Wait for the stream to be drained */
    pa_threaded_mainloop_lock(ctx->mainloop);
    pa_threaded_mainloop_wait(ctx->mainloop);
    fail_unless(pa_stream_get_state(stream) == PA_STREAM_READY);
    pa_threaded_mainloop_unlock(ctx->mainloop);

    disconnect_stream(stream);
}
END_TEST

static bool check_sink_volume(const pa_sink_info *sink_info, void *userdata) {
    pa_cvolume *v = (pa_cvolume *) userdata;

    pa_assert(v);

    return pa_cvolume_equal(&sink_info->volume, v);
}

START_TEST (passthrough_volume_test) {
    /* Set a non-100% volume of the sink before playback, create a passthrough
     * stream, make sure volume gets set to 100%, and then restored when the
     * stream goes away */
    pa_stream *stream;
    pa_operation *o;
    pa_cvolume volume, tmp;

    pa_threaded_mainloop_lock(ctx->mainloop);

    pa_cvolume_set(&volume, 2, PA_VOLUME_NORM / 2);
    o = pa_context_set_sink_volume_by_index(ctx->context, sink_idx, &volume, success_cb, NULL);
    WAIT_FOR_OPERATION(ctx, o);

    pa_threaded_mainloop_unlock(ctx->mainloop);

    stream = connect_stream();

    /* Wait for the stream to be drained */
    pa_threaded_mainloop_lock(ctx->mainloop);
    pa_threaded_mainloop_wait(ctx->mainloop);
    fail_unless(PA_STREAM_IS_GOOD(pa_stream_get_state(stream)));
    pa_threaded_mainloop_unlock(ctx->mainloop);

    pa_cvolume_set(&tmp, 2, PA_VOLUME_NORM);
    fail_unless(pa_test_context_check_sink(ctx, sink_idx, check_sink_volume, &tmp));

    disconnect_stream(stream);

    fail_unless(pa_test_context_check_sink(ctx, sink_idx, check_sink_volume, &volume));
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
    tcase_add_test(tc, passthrough_volume_test);
    tcase_set_timeout(tc, 5);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
