/***
  This file is part of PulseAudio.

  Copyright (C) 2020 Asymptotic <sanchayan@asymptotic.io>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arpa/inet.h>
#include <stdint.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/once.h>
#include <pulsecore/core-util.h>
#include <pulse/sample.h>
#include <pulse/timeval.h>
#include <pulse/util.h>

#include "a2dp-codecs.h"
#include "a2dp-codec-api.h"
#include "a2dp-codec-gst.h"

/* Called from the GStreamer streaming thread */
static void app_sink_eos(GstAppSink *appsink, gpointer userdata) {
    pa_log_debug("Sink got EOS");
}

static void gst_deinit_common(struct gst_info *info) {
    if (!info)
        return;
    if (info->app_sink)
        gst_object_unref(info->app_sink);
    if (info->bin)
        gst_object_unref(info->bin);
}

bool gst_init_common(struct gst_info *info) {
    GstElement *bin = NULL;
    GstElement *appsink = NULL;
    GstAppSinkCallbacks callbacks = { 0, };

    appsink = gst_element_factory_make("appsink", "app_sink");
    if (!appsink) {
        pa_log_error("Could not create appsink element");
        goto fail;
    }
    g_object_set(appsink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);

    callbacks.eos = app_sink_eos;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, info, NULL);

    bin = gst_bin_new(NULL);
    pa_assert(bin);

    info->app_sink = appsink;
    info->bin = bin;

    return true;

fail:
    if (appsink)
        gst_object_unref(appsink);

    return false;
}

static GstCaps *gst_create_caps_from_sample_spec(const pa_sample_spec *ss) {
    gchar *sample_format;
    GstCaps *caps;
    uint64_t channel_mask;

    switch (ss->format) {
        case PA_SAMPLE_S16LE:
            sample_format = "S16LE";
            break;
        case PA_SAMPLE_S24LE:
            sample_format = "S24LE";
            break;
        case PA_SAMPLE_S32LE:
            sample_format = "S32LE";
            break;
        case PA_SAMPLE_FLOAT32LE:
            sample_format = "F32LE";
            break;
        default:
            pa_assert_not_reached();
            break;
    }

    switch (ss->channels) {
        case 1:
            channel_mask = 0x1;
            break;
        case 2:
            channel_mask = 0x3;
            break;
        default:
            pa_assert_not_reached();
            break;
    }

    caps = gst_caps_new_simple("audio/x-raw",
            "format", G_TYPE_STRING, sample_format,
            "rate", G_TYPE_INT, (int) ss->rate,
            "channels", G_TYPE_INT, (int) ss->channels,
            "channel-mask", GST_TYPE_BITMASK, channel_mask,
            "layout", G_TYPE_STRING, "interleaved",
            NULL);

    pa_assert(caps);
    return caps;
}

bool gst_codec_init(struct gst_info *info, bool for_encoding, GstElement *transcoder) {
    GstPad *pad;
    GstCaps *caps;
    GstEvent *event;
    GstSegment segment;
    GstEvent *stream_start;
    guint group_id;

    pa_assert(transcoder);

    info->seq_num = 0;

    if (!gst_init_common(info))
        goto common_fail;

    gst_bin_add_many(GST_BIN(info->bin), transcoder, info->app_sink, NULL);

    if (!gst_element_link_many(transcoder, info->app_sink, NULL)) {
        pa_log_error("Failed to link codec elements into pipeline");
        goto pipeline_fail;
    }

    pad = gst_element_get_static_pad(transcoder, "sink");
    pa_assert_se(gst_element_add_pad(info->bin, gst_ghost_pad_new("sink", pad)));
    /**
     * Only the sink pad is needed to push buffers.  Cache it since
     * gst_element_get_static_pad is relatively expensive and verbose
     * on higher log levels.
     */
    info->pad_sink = pad;

    if (gst_element_set_state(info->bin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        pa_log_error("Could not start pipeline");
        goto pipeline_fail;
    }

    /* First, send stream-start sticky event */
    group_id = gst_util_group_id_next();
    stream_start = gst_event_new_stream_start("gst-codec-pa");
    gst_event_set_group_id(stream_start, group_id);
    gst_pad_send_event(info->pad_sink, stream_start);

    /* Retrieve the pad that handles the PCM format between PA and GStreamer */
    if (for_encoding)
        pad = gst_element_get_static_pad(transcoder, "sink");
    else
        pad = gst_element_get_static_pad(transcoder, "src");

    /* Second, send caps sticky event */
    caps = gst_create_caps_from_sample_spec(info->ss);
    pa_assert_se(gst_pad_set_caps(pad, caps));
    gst_caps_unref(caps);
    gst_object_unref(GST_OBJECT(pad));

    /* Third, send segment sticky event */
    gst_segment_init(&segment, GST_FORMAT_TIME);
    event = gst_event_new_segment(&segment);
    gst_pad_send_event(info->pad_sink, event);

    pa_log_info("GStreamer pipeline initialisation succeeded");

    return true;

pipeline_fail:
    gst_deinit_common(info);

    pa_log_error("GStreamer pipeline initialisation failed");

    return false;

common_fail:
    /* If common initialization fails the bin has not yet had its ownership
     * transferred to the pipeline yet.
     */
    gst_object_unref(transcoder);

    pa_log_error("GStreamer pipeline creation failed");

    return false;
}

size_t gst_transcode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct gst_info *info = (struct gst_info *) codec_info;
    gsize transcoded;
    GstBuffer *in_buf;
    GstFlowReturn ret;
    size_t written = 0;
    GstSample *sample;

    pa_assert(info->pad_sink);

    in_buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
                (gpointer)input_buffer, input_size, 0, input_size, NULL, NULL);
    pa_assert(in_buf);
    /* Acquire an extra reference to validate refcount afterwards */
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(in_buf));
    pa_assert(GST_MINI_OBJECT_REFCOUNT_VALUE(in_buf) == 2);

    if (timestamp == -1)
        GST_BUFFER_TIMESTAMP(in_buf) = GST_CLOCK_TIME_NONE;
    else {
        // Timestamp is monotonically increasing with samplerate/packets-per-second;
        // convert it to a timestamp in nanoseconds:
        GST_BUFFER_TIMESTAMP(in_buf) = timestamp * PA_USEC_PER_SEC / info->ss->rate;
    }

    ret = gst_pad_chain(info->pad_sink, in_buf);
    /**
     * Ensure we're the only one holding a reference to this buffer after gst_pad_chain,
     * which internally holds a pointer reference to input_buffer.  The caller provides
     * no guarantee to the validity of this pointer after returning from this function.
     */
    pa_assert(GST_MINI_OBJECT_REFCOUNT_VALUE(in_buf) == 1);
    gst_mini_object_unref(GST_MINI_OBJECT_CAST(in_buf));

    if (ret != GST_FLOW_OK) {
        pa_log_error("failed to push buffer for transcoding %d", ret);
        goto fail;
    }

    while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(info->app_sink), 0))) {
        in_buf = gst_sample_get_buffer(sample);

        transcoded = gst_buffer_get_size(in_buf);
        written += transcoded;
        pa_assert(written <= output_size);

        GstMapInfo map_info;
        pa_assert_se(gst_buffer_map(in_buf, &map_info, GST_MAP_READ));
        memcpy(output_buffer, map_info.data, transcoded);
        gst_buffer_unmap(in_buf, &map_info);
        gst_sample_unref(sample);
    }

    *processed = input_size;

    return written;

fail:
    *processed = 0;

    return written;
}

void gst_codec_deinit(void *codec_info) {
    struct gst_info *info = (struct gst_info *) codec_info;

    if (info->bin) {
        gst_element_set_state(info->bin, GST_STATE_NULL);
        gst_object_unref(info->bin);
    }

    if (info->pad_sink)
        gst_object_unref(GST_OBJECT(info->pad_sink));

    pa_xfree(info);
}
