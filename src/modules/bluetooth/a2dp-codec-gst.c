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
static void enc_sink_eos(GstAppSink *appsink, gpointer userdata) {
    pa_log_debug("Encoder got EOS");
}

/* Called from the GStreamer streaming thread */
static GstFlowReturn enc_sink_new_sample(GstAppSink *appsink, gpointer userdata) {
    struct gst_info *info = (struct gst_info *) userdata;
    GstSample *sample = NULL;
    GstBuffer *buf;

    sample = gst_app_sink_pull_sample(GST_APP_SINK(info->enc_sink));
    if (!sample)
        return GST_FLOW_OK;

    buf = gst_sample_get_buffer(sample);
    gst_buffer_ref(buf);
    gst_adapter_push(info->enc_adapter, buf);
    gst_sample_unref(sample);
    pa_fdsem_post(info->enc_fdsem);

    return GST_FLOW_OK;
}

/* Called from the GStreamer streaming thread */
static void dec_sink_eos(GstAppSink *appsink, gpointer userdata) {
    pa_log_debug("Decoder got EOS");
}

/* Called from the GStreamer streaming thread */
static GstFlowReturn dec_sink_new_sample(GstAppSink *appsink, gpointer userdata) {
    struct gst_info *info = (struct gst_info *) userdata;
    GstSample *sample = NULL;
    GstBuffer *buf;

    sample = gst_app_sink_pull_sample(GST_APP_SINK(info->dec_sink));
    if (!sample)
        return GST_FLOW_OK;

    buf = gst_sample_get_buffer(sample);
    gst_buffer_ref(buf);
    gst_adapter_push(info->dec_adapter, buf);
    gst_sample_unref(sample);
    pa_fdsem_post(info->dec_fdsem);

    return GST_FLOW_OK;
}

void gst_deinit_enc_common(struct gst_info *info) {
    if (!info)
        return;
    if (info->enc_fdsem)
        pa_fdsem_free(info->enc_fdsem);
    if (info->enc_src)
        gst_object_unref(info->enc_src);
    if (info->enc_sink)
        gst_object_unref(info->enc_sink);
    if (info->enc_adapter)
        g_object_unref(info->enc_adapter);
    if (info->enc_pipeline)
        gst_object_unref(info->enc_pipeline);
}

void gst_deinit_dec_common(struct gst_info *info) {
    if (!info)
        return;
    if (info->dec_fdsem)
        pa_fdsem_free(info->dec_fdsem);
    if (info->dec_src)
        gst_object_unref(info->dec_src);
    if (info->dec_sink)
        gst_object_unref(info->dec_sink);
    if (info->dec_adapter)
        g_object_unref(info->dec_adapter);
    if (info->dec_pipeline)
        gst_object_unref(info->dec_pipeline);
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

bool gst_init_enc_common(struct gst_info *info) {
    GstElement *pipeline = NULL;
    GstElement *appsrc = NULL, *appsink = NULL;
    GstAdapter *adapter;
    GstAppSinkCallbacks callbacks = { 0, };
    GstBus *bus;

    appsrc = gst_element_factory_make("appsrc", "enc_source");
    if (!appsrc) {
        pa_log_error("Could not create appsrc element");
        goto fail;
    }
    g_object_set(appsrc, "is-live", FALSE, "format", GST_FORMAT_TIME, "stream-type", 0, "max-bytes", 0, NULL);

    appsink = gst_element_factory_make("appsink", "enc_sink");
    if (!appsink) {
        pa_log_error("Could not create appsink element");
        goto fail;
    }
    g_object_set(appsink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);

    callbacks.eos = enc_sink_eos;
    callbacks.new_sample = enc_sink_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, info, NULL);

    adapter = gst_adapter_new();
    pa_assert(adapter);

    pipeline = gst_pipeline_new(NULL);
    pa_assert(pipeline);

    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    gst_bus_set_sync_handler (bus, (GstBusSyncHandler) sync_bus_handler, info, NULL);
    gst_object_unref (bus);

    info->enc_src = appsrc;
    info->enc_sink = appsink;
    info->enc_adapter = adapter;
    info->enc_pipeline = pipeline;
    info->enc_fdsem = pa_fdsem_new();

    return true;

fail:
    if (appsrc)
        gst_object_unref(appsrc);
    if (appsink)
        gst_object_unref(appsink);

    return false;
}

bool gst_init_dec_common(struct gst_info *info) {
    GstElement *pipeline = NULL;
    GstElement *appsrc = NULL, *appsink = NULL;
    GstAdapter *adapter;
    GstAppSinkCallbacks callbacks = { 0, };
    GstBus *bus;

    appsrc = gst_element_factory_make("appsrc", "dec_source");
    if (!appsrc) {
        pa_log_error("Could not create decoder appsrc element");
        goto fail;
    }
    g_object_set(appsrc, "is-live", FALSE, "format", GST_FORMAT_TIME, "stream-type", 0, "max-bytes", 0, NULL);

    appsink = gst_element_factory_make("appsink", "dec_sink");
    if (!appsink) {
        pa_log_error("Could not create decoder appsink element");
        goto fail;
    }
    g_object_set(appsink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, NULL);

    callbacks.eos = dec_sink_eos;
    callbacks.new_sample = dec_sink_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, info, NULL);

    adapter = gst_adapter_new();
    pa_assert(adapter);

    pipeline = gst_pipeline_new(NULL);
    pa_assert(pipeline);

    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    gst_bus_set_sync_handler (bus, (GstBusSyncHandler) sync_bus_handler, info, NULL);
    gst_object_unref (bus);

    info->dec_src = appsrc;
    info->dec_sink = appsink;
    info->dec_adapter = adapter;
    info->dec_pipeline = pipeline;
    info->dec_fdsem = pa_fdsem_new();

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
static GstPadProbeReturn gst_enc_appsink_buffer_probe(GstPad *pad, GstPadProbeInfo *probe_info, gpointer userdata)
{
    struct gst_info *info = (struct gst_info *)userdata;

    pa_assert(probe_info->type & GST_PAD_PROBE_TYPE_IDLE);

    pa_fdsem_post(info->enc_fdsem);

    return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn gst_encoder_buffer_probe(GstPad *pad, GstPadProbeInfo *probe_info, gpointer userdata)
{
    struct gst_info *info = (struct gst_info *)userdata;
    GstPad *peer_pad;

    pa_assert(probe_info->type & GST_PAD_PROBE_TYPE_BUFFER);

    peer_pad = gst_pad_get_peer(pad);
    gst_pad_add_probe(peer_pad, GST_PAD_PROBE_TYPE_IDLE, gst_enc_appsink_buffer_probe, info, NULL);
    gst_object_unref(peer_pad);

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn gst_dec_appsink_buffer_probe(GstPad *pad, GstPadProbeInfo *probe_info, gpointer userdata)
{
    struct gst_info *info = (struct gst_info *)userdata;

    pa_assert(probe_info->type & GST_PAD_PROBE_TYPE_IDLE);

    pa_fdsem_post(info->dec_fdsem);

    return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn gst_decoder_buffer_probe(GstPad *pad, GstPadProbeInfo *probe_info, gpointer userdata)
{
    struct gst_info *info = (struct gst_info *)userdata;
    GstPad *peer_pad;

    pa_assert(probe_info->type & GST_PAD_PROBE_TYPE_BUFFER);

    peer_pad = gst_pad_get_peer(pad);
    gst_pad_add_probe(peer_pad, GST_PAD_PROBE_TYPE_IDLE, gst_dec_appsink_buffer_probe, info, NULL);
    gst_object_unref(peer_pad);

    return GST_PAD_PROBE_OK;
}

bool gst_codec_init(struct gst_info *info, bool for_encoding) {
    GstPad *pad;

    info->seq_num = 0;

    /* In case if we ever have a codec which supports decoding but not encoding */
    if (for_encoding && info->enc_bin) {
        gst_bin_add_many(GST_BIN(info->enc_pipeline), info->enc_src, info->enc_bin, info->enc_sink, NULL);

        if (!gst_element_link_many(info->enc_src, info->enc_bin, info->enc_sink, NULL)) {
            pa_log_error("Failed to link encoder elements");
            goto enc_dec_fail;
        }

        if (gst_element_set_state(info->enc_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            pa_log_error("Could not start encoder pipeline");
            goto enc_dec_fail;
        }

        /* See the comment on buffer probe functions */
        pad = gst_element_get_static_pad(info->enc_bin, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, gst_encoder_buffer_probe, info, NULL);
        gst_object_unref(pad);
    } else if (!for_encoding && info->dec_bin) {
        gst_bin_add_many(GST_BIN(info->dec_pipeline), info->dec_src, info->dec_bin, info->dec_sink, NULL);

        if (!gst_element_link_many(info->dec_src, info->dec_bin, info->dec_sink, NULL)) {
            pa_log_error("Failed to link decoder elements");
            goto enc_dec_fail;
        }

        if (gst_element_set_state(info->dec_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            pa_log_error("Could not start decoder pipeline");
            goto enc_dec_fail;
        }

        /* See the comment on buffer probe functions */
        pad = gst_element_get_static_pad(info->dec_bin, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, gst_decoder_buffer_probe, info, NULL);
        gst_object_unref(pad);
    } else
        pa_assert_not_reached();

    pa_log_info("Gstreamer pipeline initialisation succeeded");

    return true;

enc_dec_fail:
    if (for_encoding)
        gst_deinit_enc_common(info);
    else
        gst_deinit_dec_common(info);

    pa_log_error("Gstreamer pipeline initialisation failed");

    return false;
}

size_t gst_encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct gst_info *info = (struct gst_info *) codec_info;
    gsize available, encoded;
    GstBuffer *in_buf;
    GstMapInfo map_info;
    GstFlowReturn ret;
    size_t written = 0;

    in_buf = gst_buffer_new_allocate(NULL, input_size, NULL);
    pa_assert(in_buf);

    pa_assert_se(gst_buffer_map(in_buf, &map_info, GST_MAP_WRITE));
    memcpy(map_info.data, input_buffer, input_size);
    gst_buffer_unmap(in_buf, &map_info);

    ret = gst_app_src_push_buffer(GST_APP_SRC(info->enc_src), in_buf);
    if (ret != GST_FLOW_OK) {
        pa_log_error("failed to push buffer for encoding %d", ret);
        goto fail;
    }

    pa_fdsem_wait(info->enc_fdsem);

    available = gst_adapter_available(info->enc_adapter);

    if (available) {
        encoded = PA_MIN(available, output_size);

        gst_adapter_copy(info->enc_adapter, output_buffer, 0, encoded);
        gst_adapter_flush(info->enc_adapter, encoded);

        written += encoded;
    } else
        pa_log_debug("No encoded data available in adapter");

    *processed = input_size;

    return written;

fail:
    *processed = 0;

    return written;
}

size_t gst_decode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct gst_info *info = (struct gst_info *) codec_info;
    gsize available, decoded;
    GstBuffer *in_buf;
    GstMapInfo map_info;
    GstFlowReturn ret;
    size_t written = 0;

    in_buf = gst_buffer_new_allocate(NULL, input_size, NULL);
    pa_assert(in_buf);

    pa_assert_se(gst_buffer_map(in_buf, &map_info, GST_MAP_WRITE));
    memcpy(map_info.data, input_buffer, input_size);
    gst_buffer_unmap(in_buf, &map_info);

    ret = gst_app_src_push_buffer(GST_APP_SRC(info->dec_src), in_buf);
    if (ret != GST_FLOW_OK) {
        pa_log_error("failed to push buffer for decoding %d", ret);
        goto fail;
    }

    pa_fdsem_wait(info->dec_fdsem);

    available = gst_adapter_available(info->dec_adapter);

    if (available) {
        decoded = PA_MIN(available, output_size);

        gst_adapter_copy(info->dec_adapter, output_buffer, 0, decoded);
        gst_adapter_flush(info->dec_adapter, decoded);

        written += decoded;
    } else
        pa_log_debug("No decoded data available in adapter");

    *processed = input_size;

    return written;

fail:
    *processed = 0;

    return written;
}

void gst_codec_deinit(void *codec_info) {
    struct gst_info *info = (struct gst_info *) codec_info;

    if (info->enc_fdsem)
        pa_fdsem_free(info->enc_fdsem);

    if (info->dec_fdsem)
        pa_fdsem_free(info->dec_fdsem);

    if (info->enc_pipeline) {
        gst_element_set_state(info->enc_pipeline, GST_STATE_NULL);
        gst_object_unref(info->enc_pipeline);
    }

    if (info->dec_pipeline) {
        gst_element_set_state(info->dec_pipeline, GST_STATE_NULL);
        gst_object_unref(info->dec_pipeline);
    }

    if (info->enc_adapter)
        g_object_unref(info->enc_adapter);

    if (info->dec_adapter)
        g_object_unref(info->dec_adapter);

    pa_xfree(info);
}
