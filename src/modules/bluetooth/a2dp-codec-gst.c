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

/* Called from the GStreamer streaming thread */
static GstFlowReturn app_sink_new_sample(GstAppSink *appsink, gpointer userdata) {
    struct gst_info *info = (struct gst_info *) userdata;
    GstSample *sample = NULL;
    GstBuffer *buf;

    sample = gst_app_sink_pull_sample(GST_APP_SINK(info->app_sink));
    if (!sample)
        return GST_FLOW_OK;

    buf = gst_sample_get_buffer(sample);
    gst_buffer_ref(buf);
    gst_adapter_push(info->sink_adapter, buf);
    gst_sample_unref(sample);
    pa_fdsem_post(info->sample_ready_fdsem);

    return GST_FLOW_OK;
}

static void gst_deinit_common(struct gst_info *info) {
    if (!info)
        return;
    if (info->sample_ready_fdsem)
        pa_fdsem_free(info->sample_ready_fdsem);
    if (info->app_src)
        gst_object_unref(info->app_src);
    if (info->app_sink)
        gst_object_unref(info->app_sink);
    if (info->sink_adapter)
        g_object_unref(info->sink_adapter);
    if (info->pipeline)
        gst_object_unref(info->pipeline);
}

static GstBusSyncReply sync_bus_handler (GstBus *bus, GstMessage *message, struct gst_info *info) {
    GstStreamStatusType type;
    GstElement *owner;

    switch (GST_MESSAGE_TYPE (message)) {
        case GST_MESSAGE_STREAM_STATUS:

            gst_message_parse_stream_status (message, &type, &owner);

            switch (type) {
            case GST_STREAM_STATUS_TYPE_ENTER:
                pa_log_debug("GStreamer pipeline thread starting up");
                if (info->core->realtime_scheduling)
                    pa_thread_make_realtime(info->core->realtime_priority);
                break;
            case GST_STREAM_STATUS_TYPE_LEAVE:
                pa_log_debug("GStreamer pipeline thread shutting down");
                break;
            default:
                break;
            }
        break;
        default:
            break;
    }

    /* pass all messages on the async queue */
    return GST_BUS_PASS;
}

bool gst_init_common(struct gst_info *info) {
    GstElement *pipeline = NULL;
    GstElement *appsrc = NULL, *appsink = NULL;
    GstAdapter *adapter;
    GstAppSinkCallbacks callbacks = { 0, };
    GstBus *bus;

    appsrc = gst_element_factory_make("appsrc", "app_source");
    if (!appsrc) {
        pa_log_error("Could not create appsrc element");
        goto fail;
    }
    g_object_set(appsrc, "is-live", FALSE, "format", GST_FORMAT_TIME, "stream-type", 0, "max-bytes", 0, NULL);

    appsink = gst_element_factory_make("appsink", "app_sink");
    if (!appsink) {
        pa_log_error("Could not create appsink element");
        goto fail;
    }
    g_object_set(appsink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);

    callbacks.eos = app_sink_eos;
    callbacks.new_sample = app_sink_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, info, NULL);

    adapter = gst_adapter_new();
    pa_assert(adapter);

    pipeline = gst_pipeline_new(NULL);
    pa_assert(pipeline);

    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    gst_bus_set_sync_handler (bus, (GstBusSyncHandler) sync_bus_handler, info, NULL);
    gst_object_unref (bus);

    info->app_src = appsrc;
    info->app_sink = appsink;
    info->sink_adapter = adapter;
    info->pipeline = pipeline;
    info->sample_ready_fdsem = pa_fdsem_new();

    return true;

fail:
    if (appsrc)
        gst_object_unref(appsrc);
    if (appsink)
        gst_object_unref(appsink);

    return false;
}

/*
 * The idea of using buffer probes is as follows. We set a buffer probe on the
 * encoder sink pad. In the buffer probe, we set an idle probe on the upstream
 * source pad. In encode_buffer, we wait on the fdsem. The fdsem gets posted
 * when either new_sample or idle probe gets called. We do this, to make the
 * appsink behave synchronously.
 *
 * For buffer probes, see
 * https://gstreamer.freedesktop.org/documentation/additional/design/probes.html?gi-language=c
 */
static GstPadProbeReturn gst_sink_buffer_idle_probe(GstPad *pad, GstPadProbeInfo *probe_info, gpointer userdata)
{
    struct gst_info *info = (struct gst_info *)userdata;

    pa_assert(probe_info->type & GST_PAD_PROBE_TYPE_IDLE);

    pa_fdsem_post(info->sample_ready_fdsem);

    return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn gst_sink_buffer_probe(GstPad *pad, GstPadProbeInfo *probe_info, gpointer userdata)
{
    struct gst_info *info = (struct gst_info *)userdata;
    GstPad *peer_pad;

    pa_assert(probe_info->type & GST_PAD_PROBE_TYPE_BUFFER);

    peer_pad = gst_pad_get_peer(pad);
    gst_pad_add_probe(peer_pad, GST_PAD_PROBE_TYPE_IDLE, gst_sink_buffer_idle_probe, info, NULL);
    gst_object_unref(peer_pad);

    return GST_PAD_PROBE_OK;
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

    caps = gst_create_caps_from_sample_spec(info->ss);
    if (for_encoding)
        g_object_set(info->app_src, "caps", caps, NULL);
    else
        g_object_set(info->app_sink, "caps", caps, NULL);
    gst_caps_unref(caps);


    gst_bin_add_many(GST_BIN(info->pipeline), info->app_src, transcoder, info->app_sink, NULL);

    if (!gst_element_link_many(info->app_src, transcoder, info->app_sink, NULL)) {
        pa_log_error("Failed to link codec elements into pipeline");
        goto pipeline_fail;
    }

    if (gst_element_set_state(info->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        pa_log_error("Could not start pipeline");
        goto pipeline_fail;
    }

    /* See the comment on buffer probe functions */
    pad = gst_element_get_static_pad(transcoder, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, gst_sink_buffer_probe, info, NULL);
    gst_object_unref(pad);

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
    gsize available, transcoded;
    GstBuffer *in_buf;
    GstMapInfo map_info;
    GstFlowReturn ret;
    size_t written = 0;

    in_buf = gst_buffer_new_allocate(NULL, input_size, NULL);
    pa_assert(in_buf);

    pa_assert_se(gst_buffer_map(in_buf, &map_info, GST_MAP_WRITE));
    memcpy(map_info.data, input_buffer, input_size);
    gst_buffer_unmap(in_buf, &map_info);

    ret = gst_app_src_push_buffer(GST_APP_SRC(info->app_src), in_buf);
    if (ret != GST_FLOW_OK) {
        pa_log_error("failed to push buffer for transcoding %d", ret);
        goto fail;
    }

    pa_fdsem_wait(info->sample_ready_fdsem);

    available = gst_adapter_available(info->sink_adapter);

    if (available) {
        transcoded = PA_MIN(available, output_size);

        gst_adapter_copy(info->sink_adapter, output_buffer, 0, transcoded);
        gst_adapter_flush(info->sink_adapter, transcoded);

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

    if (info->sample_ready_fdsem)
        pa_fdsem_free(info->sample_ready_fdsem);


    if (info->pipeline) {
        gst_element_set_state(info->pipeline, GST_STATE_NULL);
        gst_object_unref(info->pipeline);
    }

    if (info->sink_adapter)
        g_object_unref(info->sink_adapter);

    pa_xfree(info);
}
