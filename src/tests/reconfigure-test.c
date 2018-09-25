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

#include "test-util.h"

#include <check.h>

#include <pulse/pulseaudio.h>

pa_test_context *ctx;
uint32_t sink_idx = PA_INVALID_INDEX;
static const char *bname = NULL;

static void reconfigure_setup() {
    ctx = pa_test_context_new(bname);

    sink_idx = pa_test_context_load_null_sink(ctx, NULL);
}

static void reconfigure_teardown() {
    pa_test_context_free(ctx);
}

#define SAMPLE_FORMAT PA_SAMPLE_S24_32LE
#define RATE 384000
#define CHANNELS 8

static bool check_sink_format(const pa_sink_info *i, void *userdata) {
    pa_channel_map *map = (pa_channel_map *) userdata;

    pa_assert(map);

    return (i->sample_spec.format == SAMPLE_FORMAT) &&
        (i->sample_spec.rate == RATE) &&
        (i->sample_spec.channels == CHANNELS) &&
        pa_channel_map_equal(&i->channel_map, map);
}

START_TEST (reconfigure_test) {
    pa_format_info *format;
    pa_stream *s;
    int rate = RATE;
    int channels = CHANNELS;
    pa_sample_format_t sample_format = SAMPLE_FORMAT;
    pa_channel_map map;
    /* Prepare 0.25s data, don't want more since RATE is quite high */
    uint32_t data[RATE * CHANNELS / 4] = { 0, };

    /* Pick a non-standard channel mapping */
    pa_channel_map_init_auto(&map, channels, PA_CHANNEL_MAP_AUX);

    format = pa_format_info_new();
    format->encoding = PA_ENCODING_PCM;
    pa_format_info_set_sample_format(format, sample_format);
    pa_format_info_set_rate(format, rate);
    pa_format_info_set_channels(format, channels);
    pa_format_info_set_channel_map(format, &map);

    s = pa_test_context_create_stream(ctx, "reconfigure test", sink_idx, format, PA_STREAM_PASSTHROUGH, data, sizeof(data));
    fail_unless(s != NULL);

    pa_test_context_check_sink(ctx, sink_idx, check_sink_format, &map);

    pa_test_context_destroy_stream(ctx, s);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    bname = argv[0];

    s = suite_create("Reconfigure");
    tc = tcase_create("reconfigure");
    tcase_add_checked_fixture(tc, reconfigure_setup, reconfigure_teardown);
    tcase_add_test(tc, reconfigure_test);
    tcase_set_timeout(tc, 2);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
