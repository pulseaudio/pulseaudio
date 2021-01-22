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

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/once.h>
#include <pulsecore/core-util.h>
#include <pulse/sample.h>
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
    if (info->pipeline)
        gst_object_unref(info->pipeline);
}

bool gst_init_common(struct gst_info *info) {
    GstElement *pipeline = NULL;
    GstElement *appsink = NULL;
    GstAdapter *adapter;
    GstAppSinkCallbacks callbacks = { 0, };

    appsink = gst_element_factory_make("appsink", "app_sink");
    if (!appsink) {
        pa_log_error("Could not create appsink element");
        goto fail;
    }
    g_object_set(appsink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);

    callbacks.eos = app_sink_eos;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, info, NULL);

    adapter = gst_adapter_new();
    pa_assert(adapter);

    pipeline = gst_pipeline_new(NULL);
    pa_assert(pipeline);

    info->app_sink = appsink;
    info->pipeline = pipeline;

    return true;

fail:
    if (appsink)
        gst_object_unref(appsink);

    return false;
}

static GstCaps *gst_create_caps_from_sample_spec(const pa_sample_spec *ss) {
    gchar *sample_format;
    GstCaps *caps;
    int channel_mask;

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

    pa_assert(transcoder);

    info->seq_num = 0;

    if (!gst_init_common(info))
        goto common_fail;

    gst_bin_add_many(GST_BIN(info->pipeline), transcoder, info->app_sink, NULL);

    if (!gst_element_link_many(transcoder, info->app_sink, NULL)) {
        pa_log_error("Failed to link codec elements into pipeline");
        goto pipeline_fail;
    }

    // Expose sink pad through `info->pipeline`
    pad = gst_element_get_static_pad(transcoder, "sink");
    pa_assert_se(gst_element_add_pad(info->pipeline, gst_ghost_pad_new("sink", pad)));
    gst_object_unref(GST_OBJECT(pad));

    if (gst_element_set_state(info->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        pa_log_error("Could not start pipeline");
        goto pipeline_fail;
    }

    caps = gst_create_caps_from_sample_spec(info->ss);
    if (for_encoding)
        pad = gst_element_get_static_pad(transcoder, "sink");
    else
        pad = gst_element_get_static_pad(transcoder, "src");
    /* Only works after enabling play state */
    pa_assert_se(gst_pad_set_caps(pad, caps));
    gst_caps_unref(caps);
    gst_object_unref(GST_OBJECT(pad));

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

size_t gst_transcode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct gst_info *info = (struct gst_info *) codec_info;
    gsize transcoded;
    GstBuffer *in_buf;
    GstFlowReturn ret;
    size_t written = 0;
    GstPad *in_pad;
    GstSample *sample;

    // pa_log_debug("%s thread %p", __func__, pa_thread_mq_get());

    // TODO: Silence startup warnings
    // gst_element_send_event(info->enc_bin, )

    in_buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
            (gpointer)input_buffer, input_size, 0, input_size, NULL, NULL);
    pa_assert(in_buf);

    in_pad = gst_element_get_static_pad(info->pipeline, "sink");
    pa_assert(in_pad);

    // pa_log_debug("Pushing %d new bytes", input_size);
    ret = gst_pad_chain(in_pad, in_buf);
    gst_object_unref(GST_OBJECT(in_pad));
    // pa_log_debug("Flow %d", ret);
    if (ret != GST_FLOW_OK) {
        pa_log_error("failed to push buffer for transcoding %d", ret);
        goto fail;
    }

    // TODO: This might block! Can use try_pull_sample!
    // sample = gst_app_sink_pull_sample(GST_APP_SINK(info->enc_sink));
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(info->app_sink), 0);

    if (sample) {
        in_buf = gst_sample_get_buffer(sample);

        transcoded = gst_buffer_get_size(in_buf);
        pa_assert(transcoded <= output_size);

        // transcoded = PA_MIN(gst_buffer_get_size(in_buf), output_size);

        GstMapInfo map_info;
        pa_assert_se(gst_buffer_map(in_buf, &map_info, GST_MAP_READ));
        memcpy(output_buffer, map_info.data, output_size);
        gst_buffer_unmap(in_buf, &map_info);

        written += transcoded;
    } else
        pa_log_debug("No transcoded data available in adapter");

    *processed = input_size;

    return written;

fail:
    *processed = 0;

    return written;
}

void gst_codec_deinit(void *codec_info) {
    struct gst_info *info = (struct gst_info *) codec_info;

    if (info->pipeline) {
        gst_element_set_state(info->pipeline, GST_STATE_NULL);
        gst_object_unref(info->pipeline);
    }

    pa_xfree(info);
}
