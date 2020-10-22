/***
  This file is part of PulseAudio.

  Copyright 2016 Arun Raghavan <mail@arunraghavan.net>

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

#include <pulse/timeval.h>
#include <pulsecore/fdsem.h>
#include <pulsecore/core-rtclock.h>

#include "rtp.h"

#include <gio/gio.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstadapter.h>
#include <gst/rtp/gstrtpbuffer.h>

#define MAKE_ELEMENT_NAMED(v, e, n)                     \
    v = gst_element_factory_make(e, n);                 \
    if (!v) {                                           \
        pa_log("Could not create %s element", e);       \
        goto fail;                                      \
    }

#define MAKE_ELEMENT(v, e) MAKE_ELEMENT_NAMED((v), (e), NULL)
#define RTP_HEADER_SIZE    12

struct pa_rtp_context {
    pa_fdsem *fdsem;
    pa_sample_spec ss;

    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *appsink;
    GstCaps *meta_reference;

    bool first_buffer;
    uint32_t last_timestamp;

    uint8_t *send_buf;
    size_t mtu;
};

static GstCaps* caps_from_sample_spec(const pa_sample_spec *ss) {
    if (ss->format != PA_SAMPLE_S16BE)
        return NULL;

    return gst_caps_new_simple("audio/x-raw",
            "format", G_TYPE_STRING, "S16BE",
            "rate", G_TYPE_INT, (int) ss->rate,
            "channels", G_TYPE_INT, (int) ss->channels,
            "layout", G_TYPE_STRING, "interleaved",
            NULL);
}

static bool init_send_pipeline(pa_rtp_context *c, int fd, uint8_t payload, size_t mtu, const pa_sample_spec *ss) {
    GstElement *appsrc = NULL, *pay = NULL, *capsf = NULL, *rtpbin = NULL, *sink = NULL;
    GstCaps *caps;
    GSocket *socket;
    GInetSocketAddress *addr;
    GInetAddress *iaddr;
    guint16 port;
    gchar *addr_str;

    MAKE_ELEMENT(appsrc, "appsrc");
    MAKE_ELEMENT(pay, "rtpL16pay");
    MAKE_ELEMENT(capsf, "capsfilter");
    MAKE_ELEMENT(rtpbin, "rtpbin");
    MAKE_ELEMENT(sink, "udpsink");

    c->pipeline = gst_pipeline_new(NULL);

    gst_bin_add_many(GST_BIN(c->pipeline), appsrc, pay, capsf, rtpbin, sink, NULL);

    caps = caps_from_sample_spec(ss);
    if (!caps) {
        pa_log("Unsupported format to payload");
        goto fail;
    }

    socket = g_socket_new_from_fd(fd, NULL);
    if (!socket) {
        pa_log("Failed to create socket");
        goto fail;
    }

    addr = G_INET_SOCKET_ADDRESS(g_socket_get_remote_address(socket, NULL));
    iaddr = g_inet_socket_address_get_address(addr);
    addr_str = g_inet_address_to_string(iaddr);
    port = g_inet_socket_address_get_port(addr);

    g_object_set(appsrc, "caps", caps, "is-live", TRUE, "blocksize", mtu, "format", 3 /* time */, NULL);
    g_object_set(pay, "mtu", mtu, NULL);
    g_object_set(sink, "socket", socket, "host", addr_str, "port", port,
                 "enable-last-sample", FALSE, "sync", FALSE, "loop",
                 g_socket_get_multicast_loopback(socket), "ttl",
                 g_socket_get_ttl(socket), "ttl-mc",
                 g_socket_get_multicast_ttl(socket), "auto-multicast", FALSE,
                 NULL);

    g_free(addr_str);
    g_object_unref(addr);
    g_object_unref(socket);

    gst_caps_unref(caps);

    /* Force the payload type that we want */
    caps = gst_caps_new_simple("application/x-rtp", "payload", G_TYPE_INT, (int) payload, NULL);
    g_object_set(capsf, "caps", caps, NULL);
    gst_caps_unref(caps);

    if (!gst_element_link(appsrc, pay) ||
        !gst_element_link(pay, capsf) ||
        !gst_element_link_pads(capsf, "src", rtpbin, "send_rtp_sink_0") ||
        !gst_element_link_pads(rtpbin, "send_rtp_src_0", sink, "sink")) {

        pa_log("Could not set up send pipeline");
        goto fail;
    }

    if (gst_element_set_state(c->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        pa_log("Could not start pipeline");
        goto fail;
    }

    c->appsrc = gst_object_ref(appsrc);

    return true;

fail:
    if (c->pipeline) {
        gst_object_unref(c->pipeline);
    } else {
        /* These weren't yet added to pipeline, so we still have a ref */
        if (appsrc)
            gst_object_unref(appsrc);
        if (pay)
            gst_object_unref(pay);
        if (capsf)
            gst_object_unref(capsf);
        if (rtpbin)
            gst_object_unref(rtpbin);
        if (sink)
            gst_object_unref(sink);
    }

    return false;
}

pa_rtp_context* pa_rtp_context_new_send(int fd, uint8_t payload, size_t mtu, const pa_sample_spec *ss) {
    pa_rtp_context *c = NULL;
    GError *error = NULL;

    pa_assert(fd >= 0);

    pa_log_info("Initialising GStreamer RTP backend for send");

    c = pa_xnew0(pa_rtp_context, 1);

    c->ss = *ss;
    c->mtu = mtu - RTP_HEADER_SIZE;
    c->send_buf = pa_xmalloc(c->mtu);

    if (!gst_init_check(NULL, NULL, &error)) {
        pa_log_error("Could not initialise GStreamer: %s", error->message);
        g_error_free(error);
        goto fail;
    }

    if (!init_send_pipeline(c, fd, payload, mtu, ss))
        goto fail;

    return c;

fail:
    pa_rtp_context_free(c);
    return NULL;
}

/* Called from I/O thread context */
static bool process_bus_messages(pa_rtp_context *c) {
    GstBus *bus;
    GstMessage *message;
    bool ret = true;

    bus = gst_pipeline_get_bus(GST_PIPELINE(c->pipeline));

    while (ret && (message = gst_bus_pop(bus))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = NULL;

            ret = false;

            gst_message_parse_error(message, &error, NULL);
            pa_log("Got an error: %s", error->message);

            g_error_free(error);
        }

        gst_message_unref(message);
    }

    gst_object_unref(bus);

    return ret;
}

/* Called from I/O thread context */
int pa_rtp_send(pa_rtp_context *c, pa_memblockq *q) {
    GstBuffer *buf;
    size_t n = 0;

    pa_assert(c);
    pa_assert(q);

    if (!process_bus_messages(c))
        return -1;

    /*
     * While we check here for atleast MTU worth of data being available in
     * memblockq, we might not have exact equivalent to MTU. Hence, we walk
     * over the memchunks in memblockq and accumulate MTU bytes next.
     */
    if (pa_memblockq_get_length(q) < c->mtu)
        return 0;

    for (;;) {
        pa_memchunk chunk;
        int r;

        pa_memchunk_reset(&chunk);

        if ((r = pa_memblockq_peek(q, &chunk)) >= 0) {
            /*
             * Accumulate MTU bytes of data before sending. If the current
             * chunk length + accumulated bytes exceeds MTU, we drop bytes
             * considered for transfer in this iteration from memblockq.
             *
             * The remaining bytes will be available in the next iteration,
             * as these will be tracked and maintained by memblockq.
             */
            size_t k = n + chunk.length > c->mtu ? c->mtu - n : chunk.length;

            pa_assert(chunk.memblock);

            memcpy(c->send_buf + n, pa_memblock_acquire_chunk(&chunk), k);
            pa_memblock_release(chunk.memblock);
            pa_memblock_unref(chunk.memblock);

            n += k;
            pa_memblockq_drop(q, k);
        }

        if (r < 0 || n >= c->mtu) {
            GstClock *clock;
            GstClockTime timestamp, clock_time;
            GstMapInfo info;

            if (n > 0) {
                clock = gst_element_get_clock(c->pipeline);
                clock_time = gst_clock_get_time(clock);
                gst_object_unref(clock);

                timestamp = gst_element_get_base_time(c->pipeline);
                if (timestamp > clock_time)
                  timestamp -= clock_time;
                else
                  timestamp = 0;

                buf = gst_buffer_new_allocate(NULL, n, NULL);
                pa_assert(buf);

                GST_BUFFER_PTS(buf) = timestamp;

                pa_assert_se(gst_buffer_map(buf, &info, GST_MAP_WRITE));

                memcpy(info.data, c->send_buf, n);
                gst_buffer_unmap(buf, &info);

                if (gst_app_src_push_buffer(GST_APP_SRC(c->appsrc), buf) != GST_FLOW_OK) {
                    pa_log_error("Could not push buffer");
                    return -1;
                }
            }

            if (r < 0 || pa_memblockq_get_length(q) < c->mtu)
                break;

            n = 0;
        }
    }

    return 0;
}

static GstCaps* rtp_caps_from_sample_spec(const pa_sample_spec *ss) {
    if (ss->format != PA_SAMPLE_S16BE)
        return NULL;

    return gst_caps_new_simple("application/x-rtp",
            "media", G_TYPE_STRING, "audio",
            "encoding-name", G_TYPE_STRING, "L16",
            "clock-rate", G_TYPE_INT, (int) ss->rate,
            "payload", G_TYPE_INT, (int) pa_rtp_payload_from_sample_spec(ss),
            "layout", G_TYPE_STRING, "interleaved",
            NULL);
}

static void on_pad_added(GstElement *element, GstPad *pad, gpointer userdata) {
    pa_rtp_context *c = (pa_rtp_context *) userdata;
    GstElement *depay;
    GstPad *sinkpad;
    GstPadLinkReturn ret;

    depay = gst_bin_get_by_name(GST_BIN(c->pipeline), "depay");
    pa_assert(depay);

    sinkpad = gst_element_get_static_pad(depay, "sink");

    ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        GstBus *bus;
        GError *error;

        bus = gst_pipeline_get_bus(GST_PIPELINE(c->pipeline));
        error = g_error_new(GST_CORE_ERROR, GST_CORE_ERROR_PAD, "Could not link rtpbin to depayloader");
        gst_bus_post(bus, gst_message_new_error(GST_OBJECT(c->pipeline), error, NULL));

        /* Actually cause the I/O thread to wake up and process the error */
        pa_fdsem_post(c->fdsem);

        g_error_free(error);
        gst_object_unref(bus);
    }

    gst_object_unref(sinkpad);
    gst_object_unref(depay);
}

static GstPadProbeReturn udpsrc_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer userdata) {
    struct timeval tv;
    pa_usec_t timestamp;
    pa_rtp_context *c = (pa_rtp_context *) userdata;

    pa_assert(info->type & GST_PAD_PROBE_TYPE_BUFFER);

    pa_gettimeofday(&tv);
    timestamp = pa_timeval_load(&tv);

    gst_buffer_add_reference_timestamp_meta(GST_BUFFER(info->data), c->meta_reference, timestamp * GST_USECOND,
            GST_CLOCK_TIME_NONE);

    return GST_PAD_PROBE_OK;
}

static bool init_receive_pipeline(pa_rtp_context *c, int fd, const pa_sample_spec *ss) {
    GstElement *udpsrc = NULL, *rtpbin = NULL, *depay = NULL, *appsink = NULL;
    GstCaps *caps;
    GstPad *pad;
    GSocket *socket;
    GError *error = NULL;

    MAKE_ELEMENT(udpsrc, "udpsrc");
    MAKE_ELEMENT(rtpbin, "rtpbin");
    MAKE_ELEMENT_NAMED(depay, "rtpL16depay", "depay");
    MAKE_ELEMENT(appsink, "appsink");

    c->pipeline = gst_pipeline_new(NULL);

    gst_bin_add_many(GST_BIN(c->pipeline), udpsrc, rtpbin, depay, appsink, NULL);

    socket = g_socket_new_from_fd(fd, &error);
    if (error) {
        pa_log("Could not create socket: %s", error->message);
        g_error_free(error);
        goto fail;
    }

    caps = rtp_caps_from_sample_spec(ss);
    if (!caps) {
        pa_log("Unsupported format to payload");
        goto fail;
    }

    g_object_set(udpsrc, "socket", socket, "caps", caps, "auto-multicast" /* caller handles this */, FALSE, NULL);
    g_object_set(rtpbin, "latency", 0, "buffer-mode", 0 /* none */, NULL);
    g_object_set(appsink, "sync", FALSE, "enable-last-sample", FALSE, NULL);

    gst_caps_unref(caps);
    g_object_unref(socket);

    if (!gst_element_link_pads(udpsrc, "src", rtpbin, "recv_rtp_sink_0") ||
        !gst_element_link(depay, appsink)) {

        pa_log("Could not set up receive pipeline");
        goto fail;
    }

    g_signal_connect(G_OBJECT(rtpbin), "pad-added", G_CALLBACK(on_pad_added), c);

    /* This logic should go into udpsrc, and we should be populating the
     * receive timestamp using SCM_TIMESTAMP, but until we have that ... */
    c->meta_reference = gst_caps_new_empty_simple("timestamp/x-pulseaudio-wallclock");

    pad = gst_element_get_static_pad(udpsrc, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, udpsrc_buffer_probe, c, NULL);
    gst_object_unref(pad);

    if (gst_element_set_state(c->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        pa_log("Could not start pipeline");
        goto fail;
    }

    c->appsink = gst_object_ref(appsink);

    return true;

fail:
    if (c->pipeline) {
        gst_object_unref(c->pipeline);
    } else {
        /* These weren't yet added to pipeline, so we still have a ref */
        if (udpsrc)
            gst_object_unref(udpsrc);
        if (depay)
            gst_object_unref(depay);
        if (rtpbin)
            gst_object_unref(rtpbin);
        if (appsink)
            gst_object_unref(appsink);
    }

    return false;
}

/* Called from the GStreamer streaming thread */
static void appsink_eos(GstAppSink *appsink, gpointer userdata) {
    pa_rtp_context *c = (pa_rtp_context *) userdata;

    pa_fdsem_post(c->fdsem);
}

/* Called from the GStreamer streaming thread */
static GstFlowReturn appsink_new_sample(GstAppSink *appsink, gpointer userdata) {
    pa_rtp_context *c = (pa_rtp_context *) userdata;

    pa_fdsem_post(c->fdsem);

    return GST_FLOW_OK;
}

pa_rtp_context* pa_rtp_context_new_recv(int fd, uint8_t payload, const pa_sample_spec *ss) {
    pa_rtp_context *c = NULL;
    GstAppSinkCallbacks callbacks = { 0, };
    GError *error = NULL;

    pa_assert(fd >= 0);

    pa_log_info("Initialising GStreamer RTP backend for receive");

    c = pa_xnew0(pa_rtp_context, 1);

    c->fdsem = pa_fdsem_new();
    c->ss = *ss;
    c->send_buf = NULL;
    c->first_buffer = true;

    if (!gst_init_check(NULL, NULL, &error)) {
        pa_log_error("Could not initialise GStreamer: %s", error->message);
        g_error_free(error);
        goto fail;
    }

    if (!init_receive_pipeline(c, fd, ss))
        goto fail;

    callbacks.eos = appsink_eos;
    callbacks.new_sample = appsink_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(c->appsink), &callbacks, c, NULL);

    return c;

fail:
    pa_rtp_context_free(c);
    return NULL;
}

/* Called from I/O thread context */
int pa_rtp_recv(pa_rtp_context *c, pa_memchunk *chunk, pa_mempool *pool, uint32_t *rtp_tstamp, struct timeval *tstamp) {
    GstSample *sample = NULL;
    GstBufferList *buf_list;
    GstAdapter *adapter;
    GstBuffer *buf;
    GstMapInfo info;
    GstClockTime timestamp = GST_CLOCK_TIME_NONE;
    uint8_t *data;
    uint64_t data_len = 0;

    if (!process_bus_messages(c))
        goto fail;

    adapter = gst_adapter_new();
    pa_assert(adapter);

    while (true) {
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(c->appsink), 0);
        if (!sample)
            break;

        buf = gst_sample_get_buffer(sample);

        /* Get the timestamp from the first buffer */
        if (timestamp == GST_CLOCK_TIME_NONE) {
            GstReferenceTimestampMeta *meta = gst_buffer_get_reference_timestamp_meta(buf, c->meta_reference);

            /* Use the meta if we were able to insert it and it came through,
             * else try to fallback to the DTS, which is only available in
             * GStreamer 1.16 and earlier. */
            if (meta)
                timestamp = meta->timestamp;
            else if (GST_BUFFER_DTS(buf) != GST_CLOCK_TIME_NONE)
                timestamp = GST_BUFFER_DTS(buf);
            else
                timestamp = 0;
        }

        if (GST_BUFFER_IS_DISCONT(buf))
            pa_log_info("Discontinuity detected, possibly lost some packets");

        if (!gst_buffer_map(buf, &info, GST_MAP_READ)) {
            pa_log_info("Failed to map buffer");
            gst_sample_unref(sample);
            goto fail;
        }

        data_len += info.size;
        /* We need the buffer to be valid longer than the sample, which will
         * be valid only for the duration of this loop.
         *
         * To do this, increase the ref count. Ownership is transferred to the
         * adapter in gst_adapter_push.
         */
        gst_buffer_ref(buf);
        gst_adapter_push(adapter, buf);
        gst_buffer_unmap(buf, &info);

        gst_sample_unref(sample);
    }

    buf_list = gst_adapter_take_buffer_list(adapter, data_len);
    pa_assert(buf_list);

    pa_assert(pa_mempool_block_size_max(pool) >= data_len);

    chunk->memblock = pa_memblock_new(pool, data_len);
    chunk->index = 0;
    chunk->length = data_len;

    data = (uint8_t *) pa_memblock_acquire_chunk(chunk);

    for (int i = 0; i < gst_buffer_list_length(buf_list); i++) {
        buf = gst_buffer_list_get(buf_list, i);

        if (!gst_buffer_map(buf, &info, GST_MAP_READ)) {
            gst_buffer_list_unref(buf_list);
            goto fail;
        }

        memcpy(data, info.data, info.size);
        data += info.size;
        gst_buffer_unmap(buf, &info);
    }

    pa_memblock_release(chunk->memblock);

    /* When buffer-mode = none, the buffer PTS is the RTP timestamp, converted
     * to time units (instead of clock-rate units as is in the header) and
     * wraparound-corrected. */
    *rtp_tstamp = gst_util_uint64_scale_int(GST_BUFFER_PTS(gst_buffer_list_get(buf_list, 0)), c->ss.rate, GST_SECOND) & 0xFFFFFFFFU;
    if (timestamp != GST_CLOCK_TIME_NONE)
        pa_timeval_rtstore(tstamp, timestamp / PA_NSEC_PER_USEC, false);

    if (c->first_buffer) {
        c->first_buffer = false;
        c->last_timestamp = *rtp_tstamp;
    } else {
        /* The RTP clock -> time domain -> RTP clock transformation above might
         * add a ±1 rounding error, so let's get rid of that */
        uint32_t expected = c->last_timestamp + (uint32_t) (data_len / pa_rtp_context_get_frame_size(c));
        int delta = *rtp_tstamp - expected;

        if (delta == 1 || delta == -1)
            *rtp_tstamp -= delta;

        c->last_timestamp = *rtp_tstamp;
    }

    gst_buffer_list_unref(buf_list);
    gst_object_unref(adapter);

    return 0;

fail:
    if (adapter)
        gst_object_unref(adapter);

    if (chunk->memblock)
        pa_memblock_unref(chunk->memblock);

    return -1;
}

void pa_rtp_context_free(pa_rtp_context *c) {
    pa_assert(c);

    if (c->meta_reference)
        gst_caps_unref(c->meta_reference);

    if (c->appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(c->appsrc));
        gst_object_unref(c->appsrc);
        pa_xfree(c->send_buf);
    }

    if (c->appsink)
        gst_object_unref(c->appsink);

    if (c->pipeline) {
        gst_element_set_state(c->pipeline, GST_STATE_NULL);
        gst_object_unref(c->pipeline);
    }

    if (c->fdsem)
        pa_fdsem_free(c->fdsem);

    pa_xfree(c);
}

pa_rtpoll_item* pa_rtp_context_get_rtpoll_item(pa_rtp_context *c, pa_rtpoll *rtpoll) {
    return pa_rtpoll_item_new_fdsem(rtpoll, PA_RTPOLL_LATE, c->fdsem);
}

size_t pa_rtp_context_get_frame_size(pa_rtp_context *c) {
    return pa_frame_size(&c->ss);
}
