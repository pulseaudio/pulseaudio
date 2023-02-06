/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita
  Copyright 2011-2013 BMW Car IT GmbH.
  Copyright 2018-2019 Pali Rohár <pali.rohar@gmail.com>

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

#include <errno.h>

#include <arpa/inet.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/utf8.h>
#include <pulse/util.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/json.h>
#include <pulsecore/message-handler.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/poll.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/shared.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>

#ifdef USE_SMOOTHER_2
#include <pulsecore/time-smoother_2.h>
#else
#include <pulsecore/time-smoother.h>
#endif

#include "a2dp-codecs.h"
#include "a2dp-codec-util.h"
#include "bluez5-util.h"

PA_MODULE_AUTHOR("João Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("BlueZ 5 Bluetooth audio sink and source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
    "path=<device object path>"
    "autodetect_mtu=<boolean>"
    "output_rate_refresh_interval_ms=<interval between attempts to improve output rate in milliseconds>"
    "avrcp_absolute_volume=<synchronize volume with peer, true by default>"
);

#define FIXED_LATENCY_PLAYBACK_A2DP (25 * PA_USEC_PER_MSEC)
#define FIXED_LATENCY_PLAYBACK_SCO  (25 * PA_USEC_PER_MSEC)
#define FIXED_LATENCY_RECORD_A2DP   (25 * PA_USEC_PER_MSEC)
#define FIXED_LATENCY_RECORD_SCO    (25 * PA_USEC_PER_MSEC)

static const char* const valid_modargs[] = {
    "path",
    "autodetect_mtu",
    "output_rate_refresh_interval_ms",
    "avrcp_absolute_volume",
    NULL
};

enum {
    BLUETOOTH_MESSAGE_IO_THREAD_FAILED,
    BLUETOOTH_MESSAGE_STREAM_FD_HUP,
    BLUETOOTH_MESSAGE_SET_TRANSPORT_PLAYING,
    BLUETOOTH_MESSAGE_MAX
};

enum {
    PA_SOURCE_MESSAGE_SETUP_STREAM = PA_SOURCE_MESSAGE_MAX,
};

enum {
    PA_SINK_MESSAGE_SETUP_STREAM = PA_SINK_MESSAGE_MAX,
};

typedef struct bluetooth_msg {
    pa_msgobject parent;
    pa_card *card;
} bluetooth_msg;
PA_DEFINE_PRIVATE_CLASS(bluetooth_msg, pa_msgobject);
#define BLUETOOTH_MSG(o) (bluetooth_msg_cast(o))

struct userdata {
    pa_module *module;
    pa_core *core;

    pa_hook_slot *device_connection_changed_slot;
    pa_hook_slot *device_battery_level_changed_slot;
    pa_hook_slot *transport_state_changed_slot;
    pa_hook_slot *transport_sink_volume_changed_slot;
    pa_hook_slot *transport_source_volume_changed_slot;

    pa_hook_slot *sink_volume_changed_slot;
    pa_hook_slot *source_volume_changed_slot;

    pa_hook_slot *source_output_new_hook_slot;

    pa_bluetooth_discovery *discovery;
    pa_bluetooth_device *device;
    pa_bluetooth_transport *transport;
    bool transport_acquired;
    bool stream_setup_done;

    pa_card *card;
    pa_sink *sink;
    pa_source *source;
    pa_bluetooth_profile_t profile;
    char *output_port_name;
    char *input_port_name;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    bluetooth_msg *msg;

    int stream_fd;
    size_t read_link_mtu;
    size_t write_link_mtu;
    size_t read_block_size;
    size_t write_block_size;
    uint64_t read_index;
    uint64_t write_index;
    pa_usec_t started_at;

#ifdef USE_SMOOTHER_2
    pa_smoother_2 *read_smoother;
#else
    pa_smoother *read_smoother;
#endif

    pa_memchunk write_memchunk;

    const pa_bt_codec *bt_codec;

    void *encoder_info;
    pa_sample_spec encoder_sample_spec;
    void *encoder_buffer;                        /* Codec transfer buffer */
    size_t encoder_buffer_size;                  /* Size of the buffer */
    size_t encoder_buffer_used;                  /* Used space in the buffer */

    void *decoder_info;
    pa_sample_spec decoder_sample_spec;
    void *decoder_buffer;                        /* Codec transfer buffer */
    size_t decoder_buffer_size;                  /* Size of the buffer */

    bool message_handler_registered;
};

typedef enum pa_bluetooth_form_factor {
    PA_BLUETOOTH_FORM_FACTOR_UNKNOWN,
    PA_BLUETOOTH_FORM_FACTOR_HEADSET,
    PA_BLUETOOTH_FORM_FACTOR_HANDSFREE,
    PA_BLUETOOTH_FORM_FACTOR_MICROPHONE,
    PA_BLUETOOTH_FORM_FACTOR_SPEAKER,
    PA_BLUETOOTH_FORM_FACTOR_HEADPHONE,
    PA_BLUETOOTH_FORM_FACTOR_PORTABLE,
    PA_BLUETOOTH_FORM_FACTOR_CAR,
    PA_BLUETOOTH_FORM_FACTOR_HIFI,
    PA_BLUETOOTH_FORM_FACTOR_PHONE,
} pa_bluetooth_form_factor_t;

/* Run from main thread */
static pa_bluetooth_form_factor_t form_factor_from_class(uint32_t class_of_device) {
    unsigned major, minor;
    pa_bluetooth_form_factor_t r;

    static const pa_bluetooth_form_factor_t table[] = {
        [1] = PA_BLUETOOTH_FORM_FACTOR_HEADSET,
        [2] = PA_BLUETOOTH_FORM_FACTOR_HANDSFREE,
        [4] = PA_BLUETOOTH_FORM_FACTOR_MICROPHONE,
        [5] = PA_BLUETOOTH_FORM_FACTOR_SPEAKER,
        [6] = PA_BLUETOOTH_FORM_FACTOR_HEADPHONE,
        [7] = PA_BLUETOOTH_FORM_FACTOR_PORTABLE,
        [8] = PA_BLUETOOTH_FORM_FACTOR_CAR,
        [10] = PA_BLUETOOTH_FORM_FACTOR_HIFI
    };

    /*
     * See Bluetooth Assigned Numbers for Baseband
     * https://www.bluetooth.com/specifications/assigned-numbers/baseband/
     */
    major = (class_of_device >> 8) & 0x1F;
    minor = (class_of_device >> 2) & 0x3F;

    switch (major) {
        case 2:
            return PA_BLUETOOTH_FORM_FACTOR_PHONE;
        case 4:
            break;
        default:
            pa_log_debug("Unknown Bluetooth major device class %u", major);
            return PA_BLUETOOTH_FORM_FACTOR_UNKNOWN;
    }

    r = minor < PA_ELEMENTSOF(table) ? table[minor] : PA_BLUETOOTH_FORM_FACTOR_UNKNOWN;

    if (!r)
        pa_log_debug("Unknown Bluetooth minor device class %u", minor);

    return r;
}

/* Run from main thread */
static const char *form_factor_to_string(pa_bluetooth_form_factor_t ff) {
    switch (ff) {
        case PA_BLUETOOTH_FORM_FACTOR_UNKNOWN:
            return "unknown";
        case PA_BLUETOOTH_FORM_FACTOR_HEADSET:
            return "headset";
        case PA_BLUETOOTH_FORM_FACTOR_HANDSFREE:
            return "hands-free";
        case PA_BLUETOOTH_FORM_FACTOR_MICROPHONE:
            return "microphone";
        case PA_BLUETOOTH_FORM_FACTOR_SPEAKER:
            return "speaker";
        case PA_BLUETOOTH_FORM_FACTOR_HEADPHONE:
            return "headphone";
        case PA_BLUETOOTH_FORM_FACTOR_PORTABLE:
            return "portable";
        case PA_BLUETOOTH_FORM_FACTOR_CAR:
            return "car";
        case PA_BLUETOOTH_FORM_FACTOR_HIFI:
            return "hifi";
        case PA_BLUETOOTH_FORM_FACTOR_PHONE:
            return "phone";
    }

    pa_assert_not_reached();
}

/* Run from main thread */
static void connect_ports(struct userdata *u, void *new_data, pa_direction_t direction) {
    pa_device_port *port;

    if (direction == PA_DIRECTION_OUTPUT) {
        pa_sink_new_data *sink_new_data = new_data;

        pa_assert_se(port = pa_hashmap_get(u->card->ports, u->output_port_name));
        pa_assert_se(pa_hashmap_put(sink_new_data->ports, port->name, port) >= 0);
        pa_device_port_ref(port);
    } else {
        pa_source_new_data *source_new_data = new_data;

        pa_assert_se(port = pa_hashmap_get(u->card->ports, u->input_port_name));
        pa_assert_se(pa_hashmap_put(source_new_data->ports, port->name, port) >= 0);
        pa_device_port_ref(port);
    }
}

static bool bt_prepare_encoder_buffer(struct userdata *u)
{
    size_t encoded_size, reserved_size, encoded_frames;
    pa_assert(u);
    pa_assert(u->bt_codec);

    /* If socket write MTU is less than encoded frame size, there could be
     * up to one write MTU of data left in encoder buffer from previous round.
     *
     * Reserve space for at least 2 encoded frames to cover that.
     *
     * Note for A2DP codecs it is expected that size of encoded frame is less
     * than write link MTU. Therefore each encoded frame is sent out completely
     * and there is no used space in encoder buffer before next encoder call.
     *
     * For SCO socket all writes will be of MTU size to match payload length
     * of HCI packet. Depending on selected USB Alternate Setting the payload
     * length of HCI packet may exceed encoded frame size. For mSBC frame size
     * is 60 bytes, payload length of HCI packet in USB Alts 3 is 72 byte,
     * in USB Alts 5 it is 144 bytes.
     *
     * Reserve space for up to 1 + MTU / (encoded frame size) encoded frames
     * to cover that.
     *
     * Note for current linux kernel (up to 5.13.x at least) there is no way to
     * reliably detect socket MTU size. For now we just set SCO socket MTU to be
     * large enough to cover all known sizes (largest is USB ALts 5 with 144 bytes)
     * and adjust SCO write size to be equal to last SCO read size. This makes
     * write size less or equal to MTU size. Reserving the same number of encoded
     * frames to cover full MTU is still enough.
     * See also https://gitlab.freedesktop.org/pulseaudio/pulseaudio/-/merge_requests/254#note_779802
     */

    if (u->bt_codec->get_encoded_block_size)
        encoded_size = u->bt_codec->get_encoded_block_size(u->encoder_info, u->write_block_size);
    else
        encoded_size = u->write_block_size;

    encoded_frames = u->write_link_mtu / u->write_block_size + 1;

    if (encoded_frames < 2)
        encoded_frames = 2;

    reserved_size = encoded_frames * encoded_size;

    if (u->encoder_buffer_size < reserved_size) {
        u->encoder_buffer = pa_xrealloc(u->encoder_buffer, reserved_size);
        u->encoder_buffer_size = reserved_size;

        if (u->encoder_buffer_used > reserved_size) {
            u->encoder_buffer_used = 0;
        }
    }

    /* Report if there is still not enough space for new block */
    if (u->encoder_buffer_size < u->encoder_buffer_used + encoded_size)
        return false;

    return true;
}

/* Run from IO thread */
static int bt_write_buffer(struct userdata *u) {
    ssize_t written = 0;

    pa_assert(u);
    pa_assert(u->transport);
    pa_assert(u->bt_codec);

    written = u->transport->write(u->transport, u->stream_fd, u->encoder_buffer, u->encoder_buffer_used, u->write_link_mtu);

    if (written > 0) {
        /* calculate remainder */
        u->encoder_buffer_used -= written;

        /* move any remainder back to start of u->encoder_buffer */
        if (u->encoder_buffer_used)
            memmove(u->encoder_buffer, u->encoder_buffer + written, u->encoder_buffer_used);

        return 1;
    } else if (written == 0) {
        /* Not enough data in encoder buffer */
        return 0;
    } else {
        /* Reset encoder sequence number and buffer positions */
        u->bt_codec->reset(u->encoder_info);
        u->encoder_buffer_used = 0;
        return -1;
    }
}

/* Run from IO thread */
static int bt_process_render(struct userdata *u) {
    int ret;

    const uint8_t *ptr;
    size_t processed;
    size_t length;

    pa_assert(u);
    pa_assert(u->sink);
    pa_assert(u->bt_codec);

    if (!bt_prepare_encoder_buffer(u))
        return false;

    /* First, render some data */
    if (!u->write_memchunk.memblock)
        pa_sink_render_full(u->sink, u->write_block_size, &u->write_memchunk);

    pa_assert(u->write_memchunk.length == u->write_block_size);

    ptr = (const uint8_t *) pa_memblock_acquire_chunk(&u->write_memchunk);

    length = u->bt_codec->encode_buffer(u->encoder_info, u->write_index / pa_frame_size(&u->encoder_sample_spec),
            ptr, u->write_memchunk.length,
            u->encoder_buffer + u->encoder_buffer_used, u->encoder_buffer_size - u->encoder_buffer_used,
            &processed);

    pa_memblock_release(u->write_memchunk.memblock);

    if (processed != u->write_memchunk.length) {
        pa_log_error("Encoding error");
        pa_memblock_unref(u->write_memchunk.memblock);
        pa_memchunk_reset(&u->write_memchunk);
        return -1;
    }

    /* Encoder function of BT codec may provide empty buffer, in this case do
     * not post any empty buffer via BT socket. It may be because of codec
     * internal state, e.g. encoder is waiting for more samples so it can
     * provide encoded data. */

    if (PA_LIKELY(length)) {
        u->encoder_buffer_used += length;
        ret = 1;
    } else
        ret = 0;

    u->write_index += (uint64_t) u->write_memchunk.length;
    pa_memblock_unref(u->write_memchunk.memblock);
    pa_memchunk_reset(&u->write_memchunk);

    return ret;
}

static void bt_prepare_decoder_buffer(struct userdata *u) {
    pa_assert(u);

    if (u->decoder_buffer_size < u->read_link_mtu) {
        pa_xfree(u->decoder_buffer);
        u->decoder_buffer = pa_xmalloc(u->read_link_mtu);
    }

    /* Decoder buffer cannot be larger then link MTU, otherwise
     * decode method would produce larger output then read_block_size */
    u->decoder_buffer_size = u->read_link_mtu;
}

/* Run from IO thread */
static ssize_t bt_transport_read(pa_bluetooth_transport *t, int fd, void *buffer, size_t size, pa_usec_t *p_timestamp) {
    ssize_t received = 0;

    pa_assert(t);
    for (;;) {
        uint8_t aux[1024];
        struct iovec iov;
        struct cmsghdr *cm;
        struct msghdr m;
        bool found_tstamp = false;

        pa_zero(m);
        pa_zero(aux);
        pa_zero(iov);

        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = aux;
        m.msg_controllen = sizeof(aux);

        iov.iov_base = buffer;
        iov.iov_len = size;

        received = recvmsg(fd, &m, 0);

        if (received <= 0) {

            if (received < 0 && errno == EINTR)
                /* Retry right away if we got interrupted */
                continue;

            else if (received < 0 && errno == EAGAIN)
                /* Hmm, apparently the socket was not readable, give up for now. */
                return 0;

            pa_log_error("Failed to read data from socket: %s", received < 0 ? pa_cstrerror(errno) : "EOF");
            return -1;
        }

        pa_assert((size_t) received <= size);

        /* allow write side to find out size of last read packet */
        t->last_read_size = received;

        if (p_timestamp) {
            /* TODO: get timestamp from rtp */

            for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_TIMESTAMP) {
                    struct timeval *tv = (struct timeval*) CMSG_DATA(cm);
                    pa_rtclock_from_wallclock(tv);
                    *p_timestamp = pa_timeval_load(tv);
                    found_tstamp = true;
                    break;
                }
            }

            if (!found_tstamp) {
                PA_ONCE_BEGIN {
                    pa_log_warn("Couldn't find SO_TIMESTAMP data in auxiliary recvmsg() data!");
                } PA_ONCE_END;
                *p_timestamp = pa_rtclock_now();
            }
        }

        break;
    }

    return received;
}

/* Run from IO thread */
/* Read incoming data, decode it and post result (if any) to source output.
 * Returns number of bytes posted to source output. */
static int bt_process_push(struct userdata *u) {
    pa_usec_t tstamp;
    uint8_t *ptr;
    ssize_t received;
    size_t processed = 0;

    pa_assert(u);
    pa_assert(u->source);
    pa_assert(u->read_smoother);
    pa_assert(u->bt_codec);
    pa_assert(u->transport);

    bt_prepare_decoder_buffer(u);

    received = bt_transport_read(u->transport, u->stream_fd, u->decoder_buffer, u->decoder_buffer_size, &tstamp);

    if (received <= 0) {
        return received;
    }

    pa_memchunk memchunk;

    memchunk.memblock = pa_memblock_new(u->core->mempool, u->read_block_size);
    memchunk.index = memchunk.length = 0;

    ptr = pa_memblock_acquire(memchunk.memblock);
    memchunk.length = pa_memblock_get_length(memchunk.memblock);

    memchunk.length = u->bt_codec->decode_buffer(u->decoder_info, u->decoder_buffer, received, ptr, memchunk.length, &processed);

    pa_memblock_release(memchunk.memblock);

    if (processed != (size_t) received) {
        pa_log_error("Decoding error");
        pa_memblock_unref(memchunk.memblock);
        return -1;
    }

    u->read_index += (uint64_t) memchunk.length;
#ifdef USE_SMOOTHER_2
        pa_smoother_2_resume(u->read_smoother, tstamp);
        pa_smoother_2_put(u->read_smoother, tstamp, u->read_index);
#else
        pa_smoother_put(u->read_smoother, tstamp, pa_bytes_to_usec(u->read_index, &u->decoder_sample_spec));
        pa_smoother_resume(u->read_smoother, tstamp, true);
#endif

    /* Decoding of data may result in empty buffer, in this case
     * do not post empty audio samples. It may happen due to algorithmic
     * delay of audio codec. */
    if (PA_LIKELY(memchunk.length))
        pa_source_post(u->source, &memchunk);

    /* report decoded size */
    received = memchunk.length;

    pa_memblock_unref(memchunk.memblock);

    return received;
}

static void update_sink_buffer_size(struct userdata *u) {
    int old_bufsize;
    socklen_t len = sizeof(int);
    int ret;

    ret = getsockopt(u->stream_fd, SOL_SOCKET, SO_SNDBUF, &old_bufsize, &len);
    if (ret == -1) {
        pa_log_warn("Changing bluetooth buffer size: Failed to getsockopt(SO_SNDBUF): %s", pa_cstrerror(errno));
    } else {
        int new_bufsize;

        /* Set send buffer size as small as possible. The minimum value is 1024 according to the
         * socket man page. The data is written to the socket in chunks of write_block_size, so
         * there should at least be room for two chunks in the buffer. Generally, write_block_size
         * is larger than 512. If not, use the next multiple of write_block_size which is larger
         * than 1024. */
        new_bufsize = 2 * u->write_block_size;
        if (new_bufsize < 1024)
            new_bufsize = (1024 / u->write_block_size + 1) * u->write_block_size;

        /* The kernel internally doubles the buffer size that was set by setsockopt and getsockopt
         * returns the doubled value. */
        if (new_bufsize != old_bufsize / 2) {
            ret = setsockopt(u->stream_fd, SOL_SOCKET, SO_SNDBUF, &new_bufsize, len);
            if (ret == -1)
                pa_log_warn("Changing bluetooth buffer size: Failed to change from %d to %d: %s", old_bufsize / 2, new_bufsize, pa_cstrerror(errno));
            else
                pa_log_info("Changing bluetooth buffer size: Changed from %d to %d", old_bufsize / 2, new_bufsize);
        }
    }
}

static void teardown_stream(struct userdata *u) {
    if (u->rtpoll_item) {
        pa_rtpoll_item_free(u->rtpoll_item);
        u->rtpoll_item = NULL;
    }

    if (u->stream_fd >= 0) {
        pa_close(u->stream_fd);
        u->stream_fd = -1;
    }

    if (u->read_smoother) {
#ifdef USE_SMOOTHER_2
        pa_smoother_2_free(u->read_smoother);
#else
        pa_smoother_free(u->read_smoother);
#endif
        u->read_smoother = NULL;
    }

    if (u->write_memchunk.memblock) {
        pa_memblock_unref(u->write_memchunk.memblock);
        pa_memchunk_reset(&u->write_memchunk);
    }

    pa_log_debug("Audio stream torn down");
    u->stream_setup_done = false;
}

static int transport_acquire(struct userdata *u, bool optional) {
    pa_assert(u->transport);

    if (u->transport_acquired)
        return 0;

    pa_log_debug("Acquiring transport %s", u->transport->path);

    u->stream_fd = u->transport->acquire(u->transport, optional, &u->read_link_mtu, &u->write_link_mtu);
    if (u->stream_fd < 0)
        return u->stream_fd;

    /* transport_acquired must be set before calling
     * pa_bluetooth_transport_set_state() */
    u->transport_acquired = true;
    pa_log_info("Transport %s acquired: fd %d", u->transport->path, u->stream_fd);

    if (u->transport->state == PA_BLUETOOTH_TRANSPORT_STATE_IDLE) {
        if (pa_thread_mq_get() != NULL)
            pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->msg), BLUETOOTH_MESSAGE_SET_TRANSPORT_PLAYING, NULL, 0, NULL, NULL);
        else
            pa_bluetooth_transport_set_state(u->transport, PA_BLUETOOTH_TRANSPORT_STATE_PLAYING);
    }

    return 0;
}

static void transport_release(struct userdata *u) {
    pa_assert(u->transport);

    /* Ignore if already released */
    if (!u->transport_acquired)
        return;

    pa_log_debug("Releasing transport %s", u->transport->path);

    u->transport->release(u->transport);

    u->transport_acquired = false;

    teardown_stream(u);

    /* Set transport state to idle if this was not already done by the remote end closing
     * the file descriptor. Only do this when called from the I/O thread */
    if (pa_thread_mq_get() != NULL && u->transport->state == PA_BLUETOOTH_TRANSPORT_STATE_PLAYING)
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->msg), BLUETOOTH_MESSAGE_STREAM_FD_HUP, NULL, 0, NULL, NULL);
}

/* Run from I/O thread */
static void handle_sink_block_size_change(struct userdata *u) {
    pa_sink_set_max_request_within_thread(u->sink, u->write_block_size);
    pa_sink_set_fixed_latency_within_thread(u->sink,
                                            (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK ?
                                             FIXED_LATENCY_PLAYBACK_A2DP : FIXED_LATENCY_PLAYBACK_SCO) +
                                            pa_bytes_to_usec(u->write_block_size, &u->encoder_sample_spec));

    /* If there is still data in the memchunk, we have to discard it
     * because the write_block_size may have changed. */
    if (u->write_memchunk.memblock) {
        pa_memblock_unref(u->write_memchunk.memblock);
        pa_memchunk_reset(&u->write_memchunk);
    }

    update_sink_buffer_size(u);
}

/* Run from I/O thread */
static void transport_config_mtu(struct userdata *u) {
    pa_assert(u->bt_codec);

    if (u->encoder_info) {
        u->write_block_size = u->bt_codec->get_write_block_size(u->encoder_info, u->write_link_mtu);

        if (!pa_frame_aligned(u->write_block_size, &u->sink->sample_spec)) {
            pa_log_debug("Got invalid write MTU: %lu, rounding down", u->write_block_size);
            u->write_block_size = pa_frame_align(u->write_block_size, &u->sink->sample_spec);
        }
    }

    if (u->decoder_info) {
        u->read_block_size = u->bt_codec->get_read_block_size(u->decoder_info, u->read_link_mtu);

        if (!pa_frame_aligned(u->read_block_size, &u->source->sample_spec)) {
            pa_log_debug("Got invalid read MTU: %lu, rounding down", u->read_block_size);
            u->read_block_size = pa_frame_align(u->read_block_size, &u->source->sample_spec);
        }
    }

    if (u->sink)
        handle_sink_block_size_change(u);

    if (u->source)
        pa_source_set_fixed_latency_within_thread(u->source,
                                                  (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE ?
                                                   FIXED_LATENCY_RECORD_A2DP : FIXED_LATENCY_RECORD_SCO) +
                                                  pa_bytes_to_usec(u->read_block_size, &u->decoder_sample_spec));
}

/* Run from I/O thread */
static int setup_stream(struct userdata *u) {
    struct pollfd *pollfd;
    int one;

    pa_assert(u->stream_fd >= 0);

    /* return if stream is already set up */
    if (u->stream_setup_done)
        return 0;

    pa_log_info("Transport %s resuming", u->transport->path);

    pa_assert(u->bt_codec);

    if (u->encoder_info) {
        if (u->bt_codec->reset(u->encoder_info) < 0)
            return -1;
    }

    if (u->decoder_info) {
        if (u->bt_codec->reset(u->decoder_info) < 0)
            return -1;
    }

    transport_config_mtu(u);

    pa_make_fd_nonblock(u->stream_fd);
    pa_make_socket_low_delay(u->stream_fd);

    one = 1;
    if (setsockopt(u->stream_fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof(one)) < 0)
        pa_log_warn("Failed to enable SO_TIMESTAMP: %s", pa_cstrerror(errno));

    pa_log_debug("Stream properly set up, we're ready to roll!");

    u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    pollfd->fd = u->stream_fd;
    pollfd->events = pollfd->revents = 0;

    u->read_index = u->write_index = 0;
    u->started_at = 0;
    u->stream_setup_done = true;

    if (u->source)
#ifdef USE_SMOOTHER_2
        u->read_smoother = pa_smoother_2_new(5*PA_USEC_PER_SEC, pa_rtclock_now(), pa_frame_size(&u->decoder_sample_spec), u->decoder_sample_spec.rate);
#else
        u->read_smoother = pa_smoother_new(PA_USEC_PER_SEC, 2*PA_USEC_PER_SEC, true, true, 10, pa_rtclock_now(), true);
#endif

    return 0;
}

/* Called from I/O thread, returns true if the transport was acquired or
 * a connection was requested successfully. */
static bool setup_transport_and_stream(struct userdata *u) {
    int transport_error;

    transport_error = transport_acquire(u, false);
    if (transport_error < 0) {
        if (transport_error != -EAGAIN)
            return false;
    } else {
        if (setup_stream(u) < 0)
            return false;
    }
    return true;
}

/* Run from main thread */
static pa_hook_result_t sink_source_volume_changed_cb(void *hook_data, void *call_data, void *slot_data) {
    struct userdata *u = slot_data;
    const pa_cvolume *new_volume = NULL;
    pa_volume_t volume;
    pa_bluetooth_transport_set_volume_cb notify_volume_change;

    /* In the HS/HF role, notify the AG of a change in speaker/microphone gain.
     * In the AG role the command to change HW volume on the remote is already
     * sent by the hardware callback (if the peer supports it and the sink
     * or source set_volume callback is attached. Otherwise nothing is sent).
     */
    pa_assert(pa_bluetooth_profile_should_attenuate_volume(u->profile));

    if (u->sink == call_data) {
        new_volume = pa_sink_get_volume(u->sink, false);
        notify_volume_change = u->transport->set_sink_volume;
    } else if (u->source == call_data) {
        new_volume = pa_source_get_volume(u->source, false);
        notify_volume_change = u->transport->set_source_volume;
    } else {
        return PA_HOOK_OK;
    }

    /* Volume control/notifications are optional */
    if (!notify_volume_change)
        return PA_HOOK_OK;

    volume = pa_cvolume_max(new_volume);

    notify_volume_change(u->transport, volume);

    return PA_HOOK_OK;
}

/* Run from IO thread */
static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    pa_assert(u->source == PA_SOURCE(o));
    pa_assert(u->transport);

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
#ifndef USE_SMOOTHER_2
            int64_t wi, ri;
#endif

            if (u->read_smoother) {
#ifdef USE_SMOOTHER_2
                *((int64_t*) data) = u->source->thread_info.fixed_latency - pa_smoother_2_get_delay(u->read_smoother, pa_rtclock_now(), u->read_index);
#else
                wi = pa_smoother_get(u->read_smoother, pa_rtclock_now());
                ri = pa_bytes_to_usec(u->read_index, &u->decoder_sample_spec);

                *((int64_t*) data) = u->source->thread_info.fixed_latency + wi - ri;
#endif
            } else
                *((int64_t*) data) = 0;

            return 0;
        }

        case PA_SOURCE_MESSAGE_SETUP_STREAM:
            /* Skip stream setup if stream_fd has been invalidated.
               This can occur if the stream has already been set up and
               then immediately received POLLHUP. If the stream has
               already been set up earlier, then this setup_stream()
               call is redundant anyway, but currently the code
               is such that this kind of unnecessary setup_stream()
               calls can happen. */
            if (u->stream_fd < 0)
                pa_log_debug("Skip source stream setup while closing");
            else
                setup_stream(u);
            return 0;

    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

/* Called from the IO thread. */
static int source_set_state_in_io_thread_cb(pa_source *s, pa_source_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    switch (new_state) {

        case PA_SOURCE_SUSPENDED:
            /* Ignore if transition is PA_SOURCE_INIT->PA_SOURCE_SUSPENDED */
            if (!PA_SOURCE_IS_OPENED(s->thread_info.state))
                break;

            /* Stop the device if the sink is suspended as well */
            if (!u->sink || u->sink->state == PA_SINK_SUSPENDED)
                transport_release(u);

            if (u->read_smoother)
#ifdef USE_SMOOTHER_2
                pa_smoother_2_pause(u->read_smoother, pa_rtclock_now());
#else
                pa_smoother_pause(u->read_smoother, pa_rtclock_now());
#endif
            break;

        case PA_SOURCE_IDLE:
        case PA_SOURCE_RUNNING:
            if (s->thread_info.state != PA_SOURCE_SUSPENDED)
                break;

            /* Resume the device if the sink was suspended as well */
            if (!u->sink || !PA_SINK_IS_OPENED(u->sink->thread_info.state))
                if (!setup_transport_and_stream(u))
                    return -1;

            /* We don't resume the smoother here. Instead we
             * wait until the first packet arrives */

            break;

        case PA_SOURCE_UNLINKED:
        case PA_SOURCE_INIT:
        case PA_SOURCE_INVALID_STATE:
            break;
    }

    return 0;
}

/* Run from main thread */
static void source_set_volume_cb(pa_source *s) {
    pa_volume_t volume;
    struct userdata *u;

    pa_assert(s);
    pa_assert(s->core);

    u = s->userdata;

    pa_assert(u);
    pa_assert(u->source == s);
    pa_assert(!pa_bluetooth_profile_should_attenuate_volume(u->profile));
    pa_assert(u->transport);
    pa_assert(u->transport->set_source_volume);

    /* In the AG role, send a command to change microphone gain on the HS/HF */
    volume = u->transport->set_source_volume(u->transport, pa_cvolume_max(&s->real_volume));

    pa_cvolume_set(&s->real_volume, u->decoder_sample_spec.channels, volume);
}

/* Run from main thread */
static void source_setup_volume_callback(pa_source *s) {
    struct userdata *u;

    pa_assert(s);
    pa_assert(s->core);

    u = s->userdata;
    pa_assert(u);
    pa_assert(u->source == s);
    pa_assert(u->transport);

    if (pa_bluetooth_profile_is_a2dp(u->profile) && !u->transport->device->avrcp_absolute_volume)
        return;

    /* Do not use hardware volume controls for backchannel of A2DP sink */
    if (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK) {
        pa_assert_fp(u->transport->bt_codec && u->transport->bt_codec->support_backchannel);
        return;
    }

    /* Remote volume control has to be supported for the callback to make sense,
     * otherwise this source should continue performing attenuation in software
     * without HW_VOLUME_CTL.
     * If the peer is an AG however backend-native unconditionally provides this
     * function, PA in the role of HS/HF is responsible for signalling support
     * by emitting an initial volume command.
     * For A2DP bluez-util also unconditionally provides this function to keep
     * the peer informed about volume changes.
     */
    if (!u->transport->set_source_volume)
        return;

    if (pa_bluetooth_profile_should_attenuate_volume(u->profile)) {
        if (u->source_volume_changed_slot)
            return;

        pa_log_debug("%s: Attaching volume hook to notify peer of changes", s->name);

        u->source_volume_changed_slot = pa_hook_connect(&s->core->hooks[PA_CORE_HOOK_SOURCE_VOLUME_CHANGED],
                                                        PA_HOOK_NORMAL, sink_source_volume_changed_cb, u);

        /* Send initial volume to peer, signalling support for volume control */
        u->transport->set_source_volume(u->transport, pa_cvolume_max(&s->real_volume));
    } else {
        /* It is yet unknown how (if at all) volume is synchronized for bidirectional
         * A2DP codecs.  Disallow attaching callbacks (and using HFP n_volume_steps)
         * below to a pa_source if the peer is in A2DP_SINK role.  This assert should
         * be replaced with the proper logic when bidirectional codecs are implemented.
         */
        pa_assert(u->profile != PA_BLUETOOTH_PROFILE_A2DP_SINK);

        if (s->set_volume == source_set_volume_cb)
            return;

        pa_log_debug("%s: Resetting software volume for hardware attenuation by peer", s->name);

        /* Reset local attenuation */
        pa_source_set_soft_volume(s, NULL);

        pa_source_set_set_volume_callback(s, source_set_volume_cb);
        s->n_volume_steps = HSP_MAX_GAIN + 1;
    }
}

/* Run from main thread */
static int add_source(struct userdata *u) {
    pa_source_new_data data;

    pa_assert(u->transport);

    pa_source_new_data_init(&data);
    data.module = u->module;
    data.card = u->card;
    data.driver = __FILE__;
    data.name = pa_sprintf_malloc("bluez_source.%s.%s", u->device->address, pa_bluetooth_profile_to_string(u->profile));
    data.namereg_fail = false;
    pa_proplist_sets(data.proplist, "bluetooth.protocol", pa_bluetooth_profile_to_string(u->profile));
    if (u->bt_codec)
        pa_proplist_sets(data.proplist, PA_PROP_BLUETOOTH_CODEC, u->bt_codec->name);
    pa_source_new_data_set_sample_spec(&data, &u->decoder_sample_spec);
    if (u->profile == PA_BLUETOOTH_PROFILE_HSP_HS
        || u->profile == PA_BLUETOOTH_PROFILE_HFP_HF)
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_INTENDED_ROLES, "phone");

    connect_ports(u, &data, PA_DIRECTION_INPUT);

    if (!u->transport_acquired)
        switch (u->profile) {
            case PA_BLUETOOTH_PROFILE_A2DP_SINK:
                if (u->bt_codec && u->bt_codec->support_backchannel)
                    data.suspend_cause = PA_SUSPEND_USER;
                else
                    pa_assert_not_reached();
                break;
            case PA_BLUETOOTH_PROFILE_A2DP_SOURCE:
            case PA_BLUETOOTH_PROFILE_HFP_AG:
            case PA_BLUETOOTH_PROFILE_HSP_AG:
                data.suspend_cause = PA_SUSPEND_USER;
                break;
            case PA_BLUETOOTH_PROFILE_HSP_HS:
            case PA_BLUETOOTH_PROFILE_HFP_HF:
                /* u->stream_fd contains the error returned by the last transport_acquire()
                 * EAGAIN means we are waiting for a NewConnection signal */
                if (u->stream_fd == -EAGAIN)
                    data.suspend_cause = PA_SUSPEND_USER;
                else
                    pa_assert_not_reached();
                break;
            case PA_BLUETOOTH_PROFILE_OFF:
                pa_assert_not_reached();
                break;
        }

    u->source = pa_source_new(u->core, &data, PA_SOURCE_HARDWARE|PA_SOURCE_LATENCY);
    pa_source_new_data_done(&data);
    if (!u->source) {
        pa_log_error("Failed to create source");
        return -1;
    }

    u->source->userdata = u;
    u->source->parent.process_msg = source_process_msg;
    u->source->set_state_in_io_thread = source_set_state_in_io_thread_cb;

    source_setup_volume_callback(u->source);

    return 0;
}

/* Run from IO thread */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    pa_assert(u->sink == PA_SINK(o));
    pa_assert(u->transport);

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            int64_t wi, ri, delay = 0;

            if (u->read_smoother) {
#ifdef USE_SMOOTHER_2
                /* This is only used for SCO where encoder and decoder sample specs are
                 * equal and output timing is based on the source. Therefore we can pass
                 * the write index without conversion. */
                delay = pa_smoother_2_get_delay(u->read_smoother, pa_rtclock_now(), u->write_index + u->write_block_size);
#else
                ri = pa_smoother_get(u->read_smoother, pa_rtclock_now());
                wi = pa_bytes_to_usec(u->write_index + u->write_block_size, &u->encoder_sample_spec);
                delay = wi - ri;
#endif
            } else if (u->started_at) {
                ri = pa_rtclock_now() - u->started_at;
                wi = pa_bytes_to_usec(u->write_index, &u->encoder_sample_spec);
                delay = wi - ri;
            }

            *((int64_t*) data) = u->sink->thread_info.fixed_latency + delay;

            return 0;
        }

        case PA_SINK_MESSAGE_SETUP_STREAM:
            /* Skip stream setup if stream_fd has been invalidated.
               This can occur if the stream has already been set up and
               then immediately received POLLHUP. If the stream has
               already been set up earlier, then this setup_stream()
               call is redundant anyway, but currently the code
               is such that this kind of unnecessary setup_stream()
               calls can happen. */
            if (u->stream_fd < 0)
                pa_log_debug("Skip sink stream setup while closing");
            else
                setup_stream(u);
            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    switch (new_state) {

        case PA_SINK_SUSPENDED:
            /* Ignore if transition is PA_SINK_INIT->PA_SINK_SUSPENDED */
            if (!PA_SINK_IS_OPENED(s->thread_info.state))
                break;

            /* Stop the device if the source is suspended as well */
            if (!u->source || u->source->state == PA_SOURCE_SUSPENDED)
                /* We deliberately ignore whether stopping
                 * actually worked. Since the stream_fd is
                 * closed it doesn't really matter */
                transport_release(u);

            break;

        case PA_SINK_IDLE:
        case PA_SINK_RUNNING:
            if (s->thread_info.state != PA_SINK_SUSPENDED)
                break;

            /* Resume the device if the source was suspended as well */
            if (!u->source || !PA_SOURCE_IS_OPENED(u->source->thread_info.state))
                if (!setup_transport_and_stream(u))
                    return -1;

            break;

        case PA_SINK_UNLINKED:
        case PA_SINK_INIT:
        case PA_SINK_INVALID_STATE:
            break;
    }

    return 0;
}

/* Run from main thread */
static void sink_set_volume_cb(pa_sink *s) {
    pa_volume_t volume;
    struct userdata *u;

    pa_assert(s);
    pa_assert(s->core);

    u = s->userdata;

    pa_assert(u);
    pa_assert(u->sink == s);
    pa_assert(!pa_bluetooth_profile_should_attenuate_volume(u->profile));
    pa_assert(u->transport);
    pa_assert(u->transport->set_sink_volume);

    /* In the AG role, send a command to change speaker gain on the HS/HF */
    volume = u->transport->set_sink_volume(u->transport, pa_cvolume_max(&s->real_volume));

    pa_cvolume_set(&s->real_volume, u->encoder_sample_spec.channels, volume);
}

/* Run from main thread */
static void sink_setup_volume_callback(pa_sink *s) {
    struct userdata *u;

    pa_assert(s);
    pa_assert(s->core);

    u = s->userdata;
    pa_assert(u);
    pa_assert(u->sink == s);
    pa_assert(u->transport);

    if (pa_bluetooth_profile_is_a2dp(u->profile) && !u->transport->device->avrcp_absolute_volume)
        return;

    /* Do not use hardware volume controls for backchannel of A2DP source */
    if (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE) {
        pa_assert_fp(u->transport->bt_codec && u->transport->bt_codec->support_backchannel);
        return;
    }

    /* Remote volume control has to be supported for the callback to make sense,
     * otherwise this sink should continue performing attenuation in software
     * without HW_VOLUME_CTL.
     * If the peer is an AG however backend-native unconditionally provides this
     * function, PA in the role of HS/HF is responsible for signalling support
     * by emitting an initial volume command.
     */
    if (!u->transport->set_sink_volume)
        return;

    if (pa_bluetooth_profile_should_attenuate_volume(u->profile)) {
        /* It is yet unknown how (if at all) volume is synchronized for bidirectional
         * A2DP codecs.  Disallow attaching hooks to a pa_sink if the peer is in
         * A2DP_SOURCE role.  This assert should be replaced with the proper logic
         * when bidirectional codecs are implemented.
         */
        pa_assert(u->profile != PA_BLUETOOTH_PROFILE_A2DP_SOURCE);

        if (u->sink_volume_changed_slot)
            return;

        pa_log_debug("%s: Attaching volume hook to notify peer of changes", s->name);

        u->sink_volume_changed_slot = pa_hook_connect(&s->core->hooks[PA_CORE_HOOK_SINK_VOLUME_CHANGED],
                                                      PA_HOOK_NORMAL, sink_source_volume_changed_cb, u);

        /* Send initial volume to peer, signalling support for volume control */
        u->transport->set_sink_volume(u->transport, pa_cvolume_max(&s->real_volume));
    } else {
        if (s->set_volume == sink_set_volume_cb)
            return;

        pa_log_debug("%s: Resetting software volume for hardware attenuation by peer", s->name);

        /* Reset local attenuation */
        pa_sink_set_soft_volume(s, NULL);

        pa_sink_set_set_volume_callback(s, sink_set_volume_cb);

        if (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK)
            s->n_volume_steps = A2DP_MAX_GAIN + 1;
        else
            s->n_volume_steps = HSP_MAX_GAIN + 1;
    }
}

/* Run from main thread */
static int add_sink(struct userdata *u) {
    pa_sink_new_data data;

    pa_assert(u->transport);

    pa_sink_new_data_init(&data);
    data.module = u->module;
    data.card = u->card;
    data.driver = __FILE__;
    data.name = pa_sprintf_malloc("bluez_sink.%s.%s", u->device->address, pa_bluetooth_profile_to_string(u->profile));
    data.namereg_fail = false;
    pa_proplist_sets(data.proplist, "bluetooth.protocol", pa_bluetooth_profile_to_string(u->profile));
    if (u->bt_codec)
        pa_proplist_sets(data.proplist, PA_PROP_BLUETOOTH_CODEC, u->bt_codec->name);
    pa_sink_new_data_set_sample_spec(&data, &u->encoder_sample_spec);
    if (u->profile == PA_BLUETOOTH_PROFILE_HSP_HS
        || u->profile == PA_BLUETOOTH_PROFILE_HFP_HF)
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_INTENDED_ROLES, "phone");

    connect_ports(u, &data, PA_DIRECTION_OUTPUT);

    if (!u->transport_acquired)
        switch (u->profile) {
            case PA_BLUETOOTH_PROFILE_HFP_AG:
            case PA_BLUETOOTH_PROFILE_HSP_AG:
                data.suspend_cause = PA_SUSPEND_USER;
                break;
            case PA_BLUETOOTH_PROFILE_HSP_HS:
            case PA_BLUETOOTH_PROFILE_HFP_HF:
                /* u->stream_fd contains the error returned by the last transport_acquire()
                 * EAGAIN means we are waiting for a NewConnection signal */
                if (u->stream_fd == -EAGAIN)
                    data.suspend_cause = PA_SUSPEND_USER;
                else
                    pa_assert_not_reached();
                break;
            case PA_BLUETOOTH_PROFILE_A2DP_SINK:
                /* Profile switch should have failed */
            case PA_BLUETOOTH_PROFILE_A2DP_SOURCE:
            case PA_BLUETOOTH_PROFILE_OFF:
                pa_assert_not_reached();
                break;
        }

    u->sink = pa_sink_new(u->core, &data, PA_SINK_HARDWARE|PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);
    if (!u->sink) {
        pa_log_error("Failed to create sink");
        return -1;
    }

    u->sink->userdata = u;
    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;

    sink_setup_volume_callback(u->sink);

    return 0;
}

/* Run from main thread */
static pa_direction_t get_profile_direction(pa_bluetooth_profile_t p) {
    static const pa_direction_t profile_direction[] = {
        [PA_BLUETOOTH_PROFILE_A2DP_SINK] = PA_DIRECTION_OUTPUT,
        [PA_BLUETOOTH_PROFILE_A2DP_SOURCE] = PA_DIRECTION_INPUT,
        [PA_BLUETOOTH_PROFILE_HSP_HS] = PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT,
        [PA_BLUETOOTH_PROFILE_HSP_AG] = PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT,
        [PA_BLUETOOTH_PROFILE_HFP_HF] = PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT,
        [PA_BLUETOOTH_PROFILE_HFP_AG] = PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT,
        [PA_BLUETOOTH_PROFILE_OFF] = 0
    };

    return profile_direction[p];
}

/* Run from main thread */
static int transport_config(struct userdata *u) {
    bool reverse_backchannel;
    pa_assert(u);
    pa_assert(u->transport);
    pa_assert(!u->bt_codec);
    pa_assert(!u->encoder_info);
    pa_assert(!u->decoder_info);

    u->bt_codec = u->transport->bt_codec;
    pa_assert(u->bt_codec);

    /* reset encoder buffer contents */
    u->encoder_buffer_used = 0;

    /* forward encoding direction */
    reverse_backchannel = u->bt_codec->support_backchannel && !(get_profile_direction(u->profile) & PA_DIRECTION_OUTPUT);

    if ((get_profile_direction(u->profile) & PA_DIRECTION_OUTPUT) || u->bt_codec->support_backchannel) {
        u->encoder_info = u->bt_codec->init(true, reverse_backchannel, u->transport->config, u->transport->config_size, &u->encoder_sample_spec, u->core);

        if (!u->encoder_info)
            return -1;
    }

    if ((get_profile_direction(u->profile) & PA_DIRECTION_INPUT) || u->bt_codec->support_backchannel) {
        u->decoder_info = u->bt_codec->init(false, reverse_backchannel, u->transport->config, u->transport->config_size, &u->decoder_sample_spec, u->core);

        if (!u->decoder_info) {
            if (u->encoder_info) {
                u->bt_codec->deinit(u->encoder_info);
                u->encoder_info = NULL;
            }
            return -1;
        }
    }

    return 0;
}

/* Run from main thread */
static int setup_transport(struct userdata *u) {
    pa_bluetooth_transport *t;

    pa_assert(u);
    pa_assert(!u->transport);
    pa_assert(u->profile != PA_BLUETOOTH_PROFILE_OFF);

    /* check if profile has a transport */
    t = u->device->transports[u->profile];
    if (!t || t->state <= PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED) {
        pa_log_warn("Profile %s has no transport", pa_bluetooth_profile_to_string(u->profile));
        return -1;
    }

    u->transport = t;

    if (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE || u->profile == PA_BLUETOOTH_PROFILE_HFP_AG || u->profile == PA_BLUETOOTH_PROFILE_HSP_AG)
        transport_acquire(u, true); /* In case of error, the sink/sources will be created suspended */
    else {
        int transport_error;

        transport_error = transport_acquire(u, false);
        if (transport_error < 0 && transport_error != -EAGAIN)
            return -1; /* We need to fail here until the interactions with module-suspend-on-idle and alike get improved */
    }

    return transport_config(u);
}

/* Run from main thread */
static int init_profile(struct userdata *u) {
    int r = 0;
    pa_assert(u);
    pa_assert(u->profile != PA_BLUETOOTH_PROFILE_OFF);

    r = setup_transport(u);
    if (r == -EINPROGRESS)
        return 0;
    else if (r < 0)
        return -1;

    pa_assert(u->transport);

    if ((get_profile_direction(u->profile) & PA_DIRECTION_OUTPUT) || u->bt_codec->support_backchannel)
        if (add_sink(u) < 0)
            r = -1;

    if ((get_profile_direction(u->profile) & PA_DIRECTION_INPUT) || u->bt_codec->support_backchannel)
        if (add_source(u) < 0)
            r = -1;

    return r;
}

static int bt_render_block(struct userdata *u) {
    int n_rendered;

    if (u->write_index <= 0)
        u->started_at = pa_rtclock_now();

    n_rendered = bt_process_render(u);

    if (n_rendered < 0)
        n_rendered = -1;

    return n_rendered;
}

/* I/O thread function */
static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    unsigned blocks_to_write = 0;
    unsigned bytes_to_write = 0;
    struct timeval tv_last_output_rate_change;

    pa_assert(u);
    pa_assert(u->transport);

    pa_log_debug("IO Thread starting up");

    if (u->core->realtime_scheduling)
        pa_thread_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    /* Setup the stream only if the transport was already acquired */
    if (u->transport_acquired)
        setup_stream(u);

    pa_gettimeofday(&tv_last_output_rate_change);

    for (;;) {
        struct pollfd *pollfd;
        int ret;
        bool disable_timer = true;
        bool writable = false;
        bool have_source = u->source ? PA_SOURCE_IS_LINKED(u->source->thread_info.state) : false;
        bool have_sink = u->sink ? PA_SINK_IS_LINKED(u->sink->thread_info.state) : false;

        pollfd = u->rtpoll_item ? pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL) : NULL;

        /* Check for stream error or close */
        if (pollfd && (pollfd->revents & ~(POLLOUT|POLLIN))) {
            pa_log_info("FD error: %s%s%s%s",
                        pollfd->revents & POLLERR ? "POLLERR " :"",
                        pollfd->revents & POLLHUP ? "POLLHUP " :"",
                        pollfd->revents & POLLPRI ? "POLLPRI " :"",
                        pollfd->revents & POLLNVAL ? "POLLNVAL " :"");

            if (pollfd->revents & POLLHUP) {
                pollfd = NULL;
                teardown_stream(u);
                blocks_to_write = 0;
                bytes_to_write = 0;
                pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->msg), BLUETOOTH_MESSAGE_STREAM_FD_HUP, NULL, 0, NULL, NULL);
            } else
                goto fail;
        }

        /* If there is a pollfd, the stream is set up and we need to do something */
        if (pollfd) {

            /* Handle source if present */
            if (have_source) {

                /* We should send two blocks to the device before we expect a response. */
                if (have_sink && u->write_index == 0 && u->read_index <= 0)
                    blocks_to_write = 2;

                /* If we got woken up by POLLIN let's do some reading */
                if (pollfd->revents & POLLIN) {
                    int n_read;

                    n_read = bt_process_push(u);

                    if (n_read < 0)
                        goto fail;

                    if (have_sink && n_read > 0) {
                        /* We just read something, so we are supposed to write something, too
                         *
                         * If source and sink sample specifications are not equal,
                         * expected write size needs to be adjusted accordingly.
                         */
                        if (pa_sample_spec_equal(&u->encoder_sample_spec, &u->decoder_sample_spec))
                            bytes_to_write += n_read;
                        else
                            bytes_to_write += pa_usec_to_bytes(pa_bytes_to_usec(n_read, &u->decoder_sample_spec), &u->encoder_sample_spec);
                        blocks_to_write += bytes_to_write / u->write_block_size;
                        bytes_to_write = bytes_to_write % u->write_block_size;
                    }
                }
            }

            /* Handle sink if present */
            if (have_sink) {

                /* Process rewinds */
                if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
                    pa_sink_process_rewind(u->sink, 0);

                /* Test if the stream is writable */
                if (pollfd->revents & POLLOUT)
                    writable = true;

                /* If we have a source, we let the source determine the timing
                 * for the sink unless peer has not sent any data yet */
                if (have_source && u->read_index > 0) {

                    /* If the stream is writable, send some data if necessary */
                    if (writable) {
                        int result;

                        if (blocks_to_write > 0) {
                            result = bt_render_block(u);
                            if (result < 0)
                                goto fail;
                            blocks_to_write -= result;
                        }

                        result = bt_write_buffer(u);

                        if (result < 0)
                            goto fail;

                        if (result)
                            writable = false;
                    }

                    /* writable controls whether we set POLLOUT when polling - we set it to
                     * false to enable POLLOUT. If there are more blocks to write, we want to
                     * be woken up immediately when the socket becomes writable. If there
                     * aren't currently any more blocks to write, then we'll have to wait
                     * until we've received more data, so in that case we only want to set
                     * POLLIN. Note that when we are woken up the next time, POLLOUT won't be
                     * set in revents even if the socket has meanwhile become writable, which
                     * may seem bad, but in that case we'll set POLLOUT in the subsequent
                     * poll, and the poll will return immediately, so our writes won't be
                     * delayed. */
                    if (blocks_to_write > 0)
                        writable = false;

                /* There is no source, we have to use the system clock for timing */
                } else {
                    bool have_written = false;
                    pa_usec_t time_passed = 0;
                    pa_usec_t audio_sent = 0;

                    if (u->started_at) {
                        time_passed = pa_rtclock_now() - u->started_at;
                        audio_sent = pa_bytes_to_usec(u->write_index, &u->encoder_sample_spec);
                    }

                    /* A new block needs to be sent. */
                    if (audio_sent <= time_passed) {
                        size_t bytes_to_send = pa_usec_to_bytes(time_passed - audio_sent, &u->encoder_sample_spec);

                        /* There are more than two blocks that need to be written. It seems that
                         * the socket has not been accepting data fast enough (could be due to
                         * hiccups in the wireless transmission). We need to discard everything
                         * older than two block sizes to keep the latency from growing. */
                        if (bytes_to_send > 2 * u->write_block_size) {
                            uint64_t skip_bytes;
                            pa_memchunk tmp;
                            size_t max_render_size = pa_frame_align(pa_mempool_block_size_max(u->core->mempool), &u->encoder_sample_spec);
                            pa_usec_t skip_usec;

                            skip_bytes = bytes_to_send - 2 * u->write_block_size;
                            skip_usec = pa_bytes_to_usec(skip_bytes, &u->encoder_sample_spec);

                            pa_log_debug("Skipping %llu us (= %llu bytes) in audio stream",
                                        (unsigned long long) skip_usec,
                                        (unsigned long long) skip_bytes);

                            while (skip_bytes > 0) {
                                size_t bytes_to_render;

                                if (skip_bytes > max_render_size)
                                    bytes_to_render = max_render_size;
                                else
                                    bytes_to_render = skip_bytes;

                                pa_sink_render_full(u->sink, bytes_to_render, &tmp);
                                pa_memblock_unref(tmp.memblock);
                                u->write_index += bytes_to_render;
                                skip_bytes -= bytes_to_render;
                            }

                            if (u->write_index > 0 && (get_profile_direction(u->profile) & PA_DIRECTION_OUTPUT || u->bt_codec->support_backchannel)) {
                                if (u->bt_codec->reduce_encoder_bitrate) {
                                    size_t new_write_block_size = u->bt_codec->reduce_encoder_bitrate(u->encoder_info, u->write_link_mtu);
                                    if (new_write_block_size) {
                                        u->write_block_size = new_write_block_size;
                                        handle_sink_block_size_change(u);
                                    }
                                    pa_gettimeofday(&tv_last_output_rate_change);
                                }
                            }
                        }

                        blocks_to_write = 1;
                    }

                    /* If the stream is writable, send some data if necessary */
                    if (writable) {
                        int result;

                        if (blocks_to_write > 0) {
                            int result = bt_render_block(u);
                            if (result < 0)
                                goto fail;
                            blocks_to_write -= result;
                        }

                        result = bt_write_buffer(u);

                        if (result < 0)
                            goto fail;

                        if (result) {
                            if (have_source && u->read_index <= 0) {
                                /* We have a source but peer has not sent any data yet, log this */
                                if (pa_log_ratelimit(PA_LOG_DEBUG))
                                    pa_log_debug("Still no data received from source, sent one more block to sink");
                            }

                            writable = false;
                            have_written = true;
                        }
                    }

                    /* If nothing was written during this iteration, either the stream
                     * is not writable or there was no write pending. Set up a timer that
                     * will wake up the thread when the next data needs to be written. */
                    if (!have_written) {
                        pa_usec_t sleep_for;
                        pa_usec_t next_write_at;

                        if (writable) {
                            /* There was no write pending on this iteration of the loop.
                             * Let's estimate when we need to wake up next */
                            next_write_at = pa_bytes_to_usec(u->write_index, &u->encoder_sample_spec);
                            sleep_for = time_passed < next_write_at ? next_write_at - time_passed : 0;
                            /* pa_log("Sleeping for %lu; time passed %lu, next write at %lu", (unsigned long) sleep_for, (unsigned long) time_passed, (unsigned long)next_write_at); */

                            if ((get_profile_direction(u->profile) & PA_DIRECTION_OUTPUT || u->bt_codec->support_backchannel) && u->write_memchunk.memblock == NULL) {
                                /* bt_write_buffer() is keeping up with input, try increasing bitrate */
                                if (u->bt_codec->increase_encoder_bitrate
                                    && pa_timeval_age(&tv_last_output_rate_change) >= u->device->output_rate_refresh_interval_ms * PA_USEC_PER_MSEC) {
                                    size_t new_write_block_size = u->bt_codec->increase_encoder_bitrate(u->encoder_info, u->write_link_mtu);
                                    if (new_write_block_size) {
                                        u->write_block_size = new_write_block_size;
                                        handle_sink_block_size_change(u);
                                    }
                                    pa_gettimeofday(&tv_last_output_rate_change);
                                }
                            }
                        } else
                            /* We could not write because the stream was not ready. Let's try
                             * again in 500 ms and drop audio if we still can't write. The
                             * thread will also be woken up when we can write again. */
                            sleep_for = PA_USEC_PER_MSEC * 500;

                        pa_rtpoll_set_timer_relative(u->rtpoll, sleep_for);
                        disable_timer = false;
                    }
                }
            }

            /* Set events to wake up the thread */
            pollfd->events = (short) (((have_sink && !writable) ? POLLOUT : 0) | (have_source ? POLLIN : 0));

        }

        if (disable_timer)
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0) {
            pa_log_debug("pa_rtpoll_run failed with: %d", ret);
            goto fail;
        }

        if (ret == 0) {
            pa_log_debug("IO thread shutdown requested, stopping cleanly");
            transport_release(u);
            goto finish;
        }
    }

fail:
    /* If this was no regular exit from the loop we have to continue processing messages until we receive PA_MESSAGE_SHUTDOWN */
    pa_log_debug("IO thread failed");
    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u->msg), BLUETOOTH_MESSAGE_IO_THREAD_FAILED, NULL, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("IO thread shutting down");
}

/* Run from main thread */
static int start_thread(struct userdata *u) {
    pa_assert(u);
    pa_assert(!u->thread);
    pa_assert(!u->rtpoll);
    pa_assert(!u->rtpoll_item);

    u->rtpoll = pa_rtpoll_new();

    if (pa_thread_mq_init(&u->thread_mq, u->core->mainloop, u->rtpoll) < 0) {
        pa_log("pa_thread_mq_init() failed.");
        return -1;
    }

    if (!(u->thread = pa_thread_new("bluetooth", thread_func, u))) {
        pa_log_error("Failed to create IO thread");
        return -1;
    }

    if (u->sink) {
        pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
        pa_sink_set_rtpoll(u->sink, u->rtpoll);

        /* If we are in the headset role, the sink should not become default
         * unless there is no other sound device available. */
        if (u->profile == PA_BLUETOOTH_PROFILE_HFP_AG || u->profile == PA_BLUETOOTH_PROFILE_HSP_AG)
            u->sink->priority = 1500;

        pa_sink_put(u->sink);

        if (u->sink->set_volume)
            u->sink->set_volume(u->sink);
    }

    if (u->source) {
        pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
        pa_source_set_rtpoll(u->source, u->rtpoll);

        /* If we are in the headset role or the device is an a2dp source,
         * the source should not become default unless there is no other
         * sound device available. */
        if (u->profile == PA_BLUETOOTH_PROFILE_HFP_AG || u->profile == PA_BLUETOOTH_PROFILE_HSP_AG || u->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE)
            u->source->priority = 1500;

        pa_source_put(u->source);

        if (u->source->set_volume)
            u->source->set_volume(u->source);
    }

    if (u->sink || u->source)
        if (u->bt_codec)
            pa_proplist_sets(u->card->proplist, PA_PROP_BLUETOOTH_CODEC, u->bt_codec->name);

    /* Now that everything is set up we are ready to check for the Volume property.
     * Sometimes its initial "change" notification arrives too early when the sink
     * is not available or still in UNLINKED state; check it again here to know if
     * our sink peer supports Absolute Volume; in that case we should not perform
     * any attenuation but delegate all set_volume calls to the peer through this
     * Volume property.
     *
     * Note that this works the other way around if the peer is in source profile:
     * we are rendering audio and hence responsible for applying attenuation.  The
     * set_volume callback is always registered, and Volume is always passed to
     * BlueZ unconditionally.  BlueZ only sends a notification to the peer if it
     * registered a notification request for absolute volume previously.
     */
    if (u->transport && u->sink)
        pa_bluetooth_transport_load_a2dp_sink_volume(u->transport);

    return 0;
}

/* Run from main thread */
static void stop_thread(struct userdata *u) {
    pa_assert(u);

    if (u->sink || u->source)
        pa_proplist_unset(u->card->proplist, PA_PROP_BLUETOOTH_CODEC);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
        u->thread = NULL;
    }

    if (u->rtpoll_item) {
        pa_rtpoll_item_free(u->rtpoll_item);
        u->rtpoll_item = NULL;
    }

    if (u->rtpoll) {
        pa_rtpoll_free(u->rtpoll);
        u->rtpoll = NULL;
        pa_thread_mq_done(&u->thread_mq);
    }

    if (u->transport) {
        transport_release(u);
        u->transport = NULL;
    }

    if (u->sink_volume_changed_slot) {
        pa_hook_slot_free(u->sink_volume_changed_slot);
        u->sink_volume_changed_slot = NULL;
    }

    if (u->source_volume_changed_slot) {
        pa_hook_slot_free(u->source_volume_changed_slot);
        u->source_volume_changed_slot = NULL;
    }

    if (u->sink) {
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }

    if (u->source) {
        pa_source_unref(u->source);
        u->source = NULL;
    }

    if (u->read_smoother) {
#ifdef USE_SMOOTHER_2
        pa_smoother_2_free(u->read_smoother);
#else
        pa_smoother_free(u->read_smoother);
#endif
        u->read_smoother = NULL;
    }

    if (u->bt_codec) {
        if (u->encoder_info) {
            u->bt_codec->deinit(u->encoder_info);
            u->encoder_info = NULL;
        }

        if (u->decoder_info) {
            u->bt_codec->deinit(u->decoder_info);
            u->decoder_info = NULL;
        }

        u->bt_codec = NULL;
    }

    if (u->encoder_buffer) {
        pa_xfree(u->encoder_buffer);
        u->encoder_buffer = NULL;
    }

    u->encoder_buffer_size = 0;
    u->encoder_buffer_used = 0;

    if (u->decoder_buffer) {
        pa_xfree(u->decoder_buffer);
        u->decoder_buffer = NULL;
    }

    u->decoder_buffer_size = 0;
}

/* Run from main thread */
static pa_available_t get_port_availability(struct userdata *u, pa_direction_t direction) {
    pa_available_t result = PA_AVAILABLE_NO;
    unsigned i;

    pa_assert(u);
    pa_assert(u->device);

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++) {
        pa_bluetooth_transport *transport;

        if (!(transport = u->device->transports[i]))
            continue;

        if (!(get_profile_direction(i) & direction || (transport->bt_codec && transport->bt_codec->support_backchannel)))
            continue;

        switch(transport->state) {
            case PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED:
                continue;

            case PA_BLUETOOTH_TRANSPORT_STATE_IDLE:
                if (result == PA_AVAILABLE_NO)
                    result = PA_AVAILABLE_UNKNOWN;

                break;

            case PA_BLUETOOTH_TRANSPORT_STATE_PLAYING:
                return PA_AVAILABLE_YES;
        }
    }

    return result;
}

/* Run from main thread */
static pa_available_t transport_state_to_availability(pa_bluetooth_transport_state_t state) {
    switch (state) {
        case PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED:
            return PA_AVAILABLE_NO;
        case PA_BLUETOOTH_TRANSPORT_STATE_PLAYING:
            return PA_AVAILABLE_YES;
        default:
            return PA_AVAILABLE_UNKNOWN;
    }
}

/* Run from main thread */
static void create_card_ports(struct userdata *u, pa_hashmap *ports) {
    pa_device_port *port;
    pa_device_port_new_data port_data;
    pa_device_port_type_t input_type, output_type;
    const char *name_prefix, *input_description, *output_description;

    pa_assert(u);
    pa_assert(ports);
    pa_assert(u->device);

    name_prefix = "unknown";
    input_description = _("Bluetooth Input");
    output_description = _("Bluetooth Output");
    input_type = output_type = PA_DEVICE_PORT_TYPE_BLUETOOTH;

    switch (form_factor_from_class(u->device->class_of_device)) {
        case PA_BLUETOOTH_FORM_FACTOR_HEADSET:
            name_prefix = "headset";
            input_description = output_description = _("Headset");
            input_type = output_type = PA_DEVICE_PORT_TYPE_HEADSET;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_HANDSFREE:
            name_prefix = "handsfree";
            input_description = output_description = _("Handsfree");
            input_type = output_type = PA_DEVICE_PORT_TYPE_HANDSFREE;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_MICROPHONE:
            name_prefix = "microphone";
            input_description = _("Microphone");
            output_description = _("Bluetooth Output");
            input_type = PA_DEVICE_PORT_TYPE_MIC;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_SPEAKER:
            name_prefix = "speaker";
            input_description = _("Bluetooth Input");
            output_description = _("Speaker");
            output_type = PA_DEVICE_PORT_TYPE_SPEAKER;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_HEADPHONE:
            name_prefix = "headphone";
            input_description = _("Bluetooth Input");
            output_description = _("Headphone");
            output_type = PA_DEVICE_PORT_TYPE_HEADPHONES;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_PORTABLE:
            name_prefix = "portable";
            input_description = output_description = _("Portable");
            input_type = output_type = PA_DEVICE_PORT_TYPE_PORTABLE;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_CAR:
            name_prefix = "car";
            input_description = output_description = _("Car");
            input_type = output_type = PA_DEVICE_PORT_TYPE_CAR;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_HIFI:
            name_prefix = "hifi";
            input_description = output_description = _("HiFi");
            input_type = output_type = PA_DEVICE_PORT_TYPE_HIFI;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_PHONE:
            name_prefix = "phone";
            input_description = output_description = _("Phone");
            input_type = output_type = PA_DEVICE_PORT_TYPE_PHONE;
            break;

        case PA_BLUETOOTH_FORM_FACTOR_UNKNOWN:
            break;
    }

    u->output_port_name = pa_sprintf_malloc("%s-output", name_prefix);
    pa_device_port_new_data_init(&port_data);
    pa_device_port_new_data_set_name(&port_data, u->output_port_name);
    pa_device_port_new_data_set_description(&port_data, output_description);
    pa_device_port_new_data_set_direction(&port_data, PA_DIRECTION_OUTPUT);
    pa_device_port_new_data_set_type(&port_data, output_type);
    pa_device_port_new_data_set_available(&port_data, get_port_availability(u, PA_DIRECTION_OUTPUT));
    pa_assert_se(port = pa_device_port_new(u->core, &port_data, 0));
    pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);
    pa_device_port_new_data_done(&port_data);

    u->input_port_name = pa_sprintf_malloc("%s-input", name_prefix);
    pa_device_port_new_data_init(&port_data);
    pa_device_port_new_data_set_name(&port_data, u->input_port_name);
    pa_device_port_new_data_set_description(&port_data, input_description);
    pa_device_port_new_data_set_direction(&port_data, PA_DIRECTION_INPUT);
    pa_device_port_new_data_set_type(&port_data, input_type);
    pa_device_port_new_data_set_available(&port_data, get_port_availability(u, PA_DIRECTION_INPUT));
    pa_assert_se(port = pa_device_port_new(u->core, &port_data, 0));
    pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);
    pa_device_port_new_data_done(&port_data);
}

/* Run from main thread */
static pa_card_profile *create_card_profile(struct userdata *u, pa_bluetooth_profile_t profile, pa_hashmap *ports) {
    pa_device_port *input_port, *output_port;
    const char *name;
    pa_card_profile *cp = NULL;
    pa_bluetooth_profile_t *p;

    pa_assert(u->input_port_name);
    pa_assert(u->output_port_name);
    pa_assert_se(input_port = pa_hashmap_get(ports, u->input_port_name));
    pa_assert_se(output_port = pa_hashmap_get(ports, u->output_port_name));

    name = pa_bluetooth_profile_to_string(profile);

    switch (profile) {
    case PA_BLUETOOTH_PROFILE_A2DP_SINK:
        cp = pa_card_profile_new(name, _("High Fidelity Playback (A2DP Sink)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 40;
        cp->n_sinks = 1;
        cp->n_sources = 0;
        cp->max_sink_channels = 2;
        cp->max_source_channels = 0;
        pa_hashmap_put(output_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        break;

    case PA_BLUETOOTH_PROFILE_A2DP_SOURCE:
        cp = pa_card_profile_new(name, _("High Fidelity Capture (A2DP Source)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 20;
        cp->n_sinks = 0;
        cp->n_sources = 1;
        cp->max_sink_channels = 0;
        cp->max_source_channels = 2;
        pa_hashmap_put(input_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        break;

    case PA_BLUETOOTH_PROFILE_HSP_HS:
        cp = pa_card_profile_new(name, _("Headset Head Unit (HSP)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 30;
        cp->n_sinks = 1;
        cp->n_sources = 1;
        cp->max_sink_channels = 1;
        cp->max_source_channels = 1;
        pa_hashmap_put(input_port->profiles, cp->name, cp);
        pa_hashmap_put(output_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        break;

    case PA_BLUETOOTH_PROFILE_HSP_AG:
        cp = pa_card_profile_new(name, _("Headset Audio Gateway (HSP)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 10;
        cp->n_sinks = 1;
        cp->n_sources = 1;
        cp->max_sink_channels = 1;
        cp->max_source_channels = 1;
        pa_hashmap_put(input_port->profiles, cp->name, cp);
        pa_hashmap_put(output_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        break;

    case PA_BLUETOOTH_PROFILE_HFP_HF:
         cp = pa_card_profile_new(name, _("Handsfree Head Unit (HFP)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 30;
        cp->n_sinks = 1;
        cp->n_sources = 1;
        cp->max_sink_channels = 1;
        cp->max_source_channels = 1;
        pa_hashmap_put(input_port->profiles, cp->name, cp);
        pa_hashmap_put(output_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        break;

    case PA_BLUETOOTH_PROFILE_HFP_AG:
        cp = pa_card_profile_new(name, _("Handsfree Audio Gateway (HFP)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 10;
        cp->n_sinks = 1;
        cp->n_sources = 1;
        cp->max_sink_channels = 1;
        cp->max_source_channels = 1;
        pa_hashmap_put(input_port->profiles, cp->name, cp);
        pa_hashmap_put(output_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        break;

    case PA_BLUETOOTH_PROFILE_OFF:
        pa_assert_not_reached();
    }

    *p = profile;

    if (u->device->transports[*p])
        cp->available = transport_state_to_availability(u->device->transports[*p]->state);
    else
        cp->available = PA_AVAILABLE_NO;

    return cp;
}

/* Run from main thread */
static int set_profile_cb(pa_card *c, pa_card_profile *new_profile) {
    struct userdata *u;
    pa_bluetooth_profile_t *p;

    pa_assert(c);
    pa_assert(new_profile);
    pa_assert_se(u = c->userdata);

    p = PA_CARD_PROFILE_DATA(new_profile);

    if (*p != PA_BLUETOOTH_PROFILE_OFF) {
        const pa_bluetooth_device *d = u->device;

        if (!d->transports[*p] || d->transports[*p]->state <= PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED) {
            pa_log_warn("Refused to switch profile to %s: Not connected", new_profile->name);
            return -PA_ERR_IO;
        }
    }

    stop_thread(u);

    u->profile = *p;

    if (u->profile != PA_BLUETOOTH_PROFILE_OFF)
        if (init_profile(u) < 0)
            goto off;

    if (u->sink || u->source)
        if (start_thread(u) < 0)
            goto off;

    return 0;

off:
    stop_thread(u);

    pa_assert_se(pa_card_set_profile(u->card, pa_hashmap_get(u->card->profiles, "off"), false) >= 0);

    return -PA_ERR_IO;
}

static int uuid_to_profile(const char *uuid, pa_bluetooth_profile_t *_r) {
    if (pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SINK))
        *_r = PA_BLUETOOTH_PROFILE_A2DP_SINK;
    else if (pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SOURCE))
        *_r = PA_BLUETOOTH_PROFILE_A2DP_SOURCE;
    else if (pa_bluetooth_uuid_is_hsp_hs(uuid))
        *_r = PA_BLUETOOTH_PROFILE_HSP_HS;
    else if (pa_streq(uuid, PA_BLUETOOTH_UUID_HFP_HF))
        *_r = PA_BLUETOOTH_PROFILE_HFP_HF;
    else if (pa_streq(uuid, PA_BLUETOOTH_UUID_HSP_AG))
        *_r = PA_BLUETOOTH_PROFILE_HSP_AG;
    else if (pa_streq(uuid, PA_BLUETOOTH_UUID_HFP_AG))
        *_r = PA_BLUETOOTH_PROFILE_HFP_AG;
    else
        return -PA_ERR_INVALID;

    return 0;
}

/* Run from main thread */
static int add_card(struct userdata *u) {
    const pa_bluetooth_device *d;
    pa_card_new_data data;
    char *alias;
    pa_bluetooth_form_factor_t ff;
    pa_card_profile *cp;
    pa_bluetooth_profile_t *p;
    const char *uuid;
    void *state;

    pa_assert(u);
    pa_assert(u->device);

    d = u->device;

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;

    alias = pa_utf8_filter(d->alias);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, alias);
    pa_xfree(alias);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, d->address);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "bluez");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_BUS, "bluetooth");

    if ((ff = form_factor_from_class(d->class_of_device)) != PA_BLUETOOTH_FORM_FACTOR_UNKNOWN)
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_FORM_FACTOR, form_factor_to_string(ff));

    pa_proplist_sets(data.proplist, "bluez.path", d->path);
    pa_proplist_setf(data.proplist, "bluez.class", "0x%06x", d->class_of_device);
    pa_proplist_sets(data.proplist, "bluez.alias", d->alias);
    data.name = pa_sprintf_malloc("bluez_card.%s", d->address);
    data.namereg_fail = false;

    if (d->has_battery_level) {
        // See device_battery_level_changed_cb
        uint8_t level = d->battery_level;
        pa_proplist_setf(data.proplist, "bluetooth.battery", "%d%%", level);
    }

    create_card_ports(u, data.ports);

    PA_HASHMAP_FOREACH(uuid, d->uuids, state) {
        pa_bluetooth_profile_t profile;

        if (uuid_to_profile(uuid, &profile) < 0)
            continue;

        pa_log_debug("Trying to create profile %s (%s) for device %s (%s)",
                     pa_bluetooth_profile_to_string(profile), uuid, d->alias, d->address);

        if (pa_hashmap_get(data.profiles, pa_bluetooth_profile_to_string(profile))) {
            pa_log_debug("%s already exists", pa_bluetooth_profile_to_string(profile));
            continue;
        }

        if (!pa_bluetooth_device_supports_profile(d, profile)) {
            pa_log_debug("%s is not supported by the device or adapter", pa_bluetooth_profile_to_string(profile));
            continue;
        }

        cp = create_card_profile(u, profile, data.ports);
        pa_hashmap_put(data.profiles, cp->name, cp);
    }

    pa_assert(!pa_hashmap_isempty(data.profiles));

    cp = pa_card_profile_new("off", _("Off"), sizeof(pa_bluetooth_profile_t));
    cp->available = PA_AVAILABLE_YES;
    p = PA_CARD_PROFILE_DATA(cp);
    *p = PA_BLUETOOTH_PROFILE_OFF;
    pa_hashmap_put(data.profiles, cp->name, cp);

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);
    if (!u->card) {
        pa_log("Failed to allocate card.");
        return -1;
    }

    u->card->userdata = u;
    u->card->set_profile = set_profile_cb;
    pa_card_choose_initial_profile(u->card);
    pa_card_put(u->card);

    p = PA_CARD_PROFILE_DATA(u->card->active_profile);
    u->profile = *p;

    return 0;
}

/* Run from main thread */
static void handle_transport_state_change(struct userdata *u, struct pa_bluetooth_transport *t) {
    bool acquire = false;
    bool release = false;
    pa_card_profile *cp;
    pa_device_port *port;
    pa_available_t oldavail;

    pa_assert(u);
    pa_assert(t);
    pa_assert_se(cp = pa_hashmap_get(u->card->profiles, pa_bluetooth_profile_to_string(t->profile)));

    oldavail = cp->available;
    /*
     * If codec switching is in progress, transport state change should not
     * make profile unavailable.
     */
    if (!t->device->codec_switching_in_progress)
        pa_card_profile_set_available(cp, transport_state_to_availability(t->state));

    /* Update port availability */
    pa_assert_se(port = pa_hashmap_get(u->card->ports, u->output_port_name));
    pa_device_port_set_available(port, get_port_availability(u, PA_DIRECTION_OUTPUT));
    pa_assert_se(port = pa_hashmap_get(u->card->ports, u->input_port_name));
    pa_device_port_set_available(port, get_port_availability(u, PA_DIRECTION_INPUT));

    /* Acquire or release transport as needed */
    acquire = (t->state == PA_BLUETOOTH_TRANSPORT_STATE_PLAYING && u->profile == t->profile);
    release = (oldavail != PA_AVAILABLE_NO && t->state != PA_BLUETOOTH_TRANSPORT_STATE_PLAYING && u->profile == t->profile);

    if (acquire && transport_acquire(u, true) >= 0) {
        if (u->source) {
            pa_log_debug("Resuming source %s because its transport state changed to playing", u->source->name);

            /* When the ofono backend resumes source or sink when in the audio gateway role, the
             * state of source or sink may already be RUNNING before the transport is acquired via
             * hf_audio_agent_new_connection(), so the pa_source_suspend() call will not lead to a
             * state change message. In this case we explicitly need to signal the I/O thread to
             * set up the stream. */
            if (PA_SOURCE_IS_OPENED(u->source->state))
                pa_asyncmsgq_send(u->source->asyncmsgq, PA_MSGOBJECT(u->source), PA_SOURCE_MESSAGE_SETUP_STREAM, NULL, 0, NULL);

            /* We remove the IDLE suspend cause, because otherwise
             * module-loopback doesn't uncork its streams. FIXME: Messing with
             * the IDLE suspend cause here is wrong, the correct way to handle
             * this would probably be to uncork the loopback streams not only
             * when the other end is unsuspended, but also when the other end's
             * suspend cause changes to IDLE only (currently there's no
             * notification mechanism for suspend cause changes, though). */
            pa_source_suspend(u->source, false, PA_SUSPEND_IDLE|PA_SUSPEND_USER);
        }

        if (u->sink) {
            pa_log_debug("Resuming sink %s because its transport state changed to playing", u->sink->name);

            /* Same comment as above */
            if (PA_SINK_IS_OPENED(u->sink->state))
                pa_asyncmsgq_send(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), PA_SINK_MESSAGE_SETUP_STREAM, NULL, 0, NULL);

            /* FIXME: See the previous comment. */
            pa_sink_suspend(u->sink, false, PA_SUSPEND_IDLE|PA_SUSPEND_USER);
        }
    }

    if (release && u->transport_acquired) {
        /* FIXME: this release is racy, since the audio stream might have
         * been set up again in the meantime (but not processed yet by PA).
         * BlueZ should probably release the transport automatically, and in
         * that case we would just mark the transport as released */

        /* Remote side closed the stream so we consider it PA_SUSPEND_USER */
        if (u->source) {
            pa_log_debug("Suspending source %s because the remote end closed the stream", u->source->name);
            pa_source_suspend(u->source, true, PA_SUSPEND_USER);
        }

        if (u->sink) {
            pa_log_debug("Suspending sink %s because the remote end closed the stream", u->sink->name);
            pa_sink_suspend(u->sink, true, PA_SUSPEND_USER);
        }
    }
}

/* Run from main thread */
static pa_hook_result_t device_connection_changed_cb(pa_bluetooth_discovery *y, const pa_bluetooth_device *d, struct userdata *u) {
    pa_assert(d);
    pa_assert(u);

    if (d != u->device || pa_bluetooth_device_any_transport_connected(d) || d->codec_switching_in_progress)
        return PA_HOOK_OK;

    pa_log_debug("Unloading module for device %s", d->path);
    pa_module_unload(u->module, true);

    return PA_HOOK_OK;
}

static pa_hook_result_t device_battery_level_changed_cb(pa_bluetooth_discovery *y, const pa_bluetooth_device *d, struct userdata *u) {
    uint8_t level;

    pa_assert(d);
    pa_assert(u);

    if (d != u->device)
        return PA_HOOK_OK;

    if (d->has_battery_level) {
        level = d->battery_level;
        pa_proplist_setf(u->card->proplist, "bluetooth.battery", "%d%%", level);
    } else {
        pa_proplist_unset(u->card->proplist, "bluetooth.battery");
    }

    return PA_HOOK_OK;
}

/* Run from main thread */
static pa_hook_result_t transport_state_changed_cb(pa_bluetooth_discovery *y, pa_bluetooth_transport *t, struct userdata *u) {
    pa_assert(t);
    pa_assert(u);

    if (t == u->transport && t->state <= PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED)
        pa_assert_se(pa_card_set_profile(u->card, pa_hashmap_get(u->card->profiles, "off"), false) >= 0);

    if (t->device == u->device)
        handle_transport_state_change(u, t);

    return PA_HOOK_OK;
}

static pa_hook_result_t transport_sink_volume_changed_cb(pa_bluetooth_discovery *y, pa_bluetooth_transport *t, struct userdata *u) {
    pa_volume_t volume;
    pa_cvolume v;

    pa_assert(t);
    pa_assert(u);

    if (t != u->transport)
      return PA_HOOK_OK;

    volume = t->sink_volume;

    if (!u->sink) {
        pa_log_warn("Received peer transport volume change without connected sink");
        return PA_HOOK_OK;
    }

    sink_setup_volume_callback(u->sink);

    pa_cvolume_set(&v, u->encoder_sample_spec.channels, volume);
    if (pa_bluetooth_profile_should_attenuate_volume(t->profile))
        pa_sink_set_volume(u->sink, &v, true, true);
    else
        pa_sink_volume_changed(u->sink, &v);

    return PA_HOOK_OK;
}

static pa_hook_result_t transport_source_volume_changed_cb(pa_bluetooth_discovery *y, pa_bluetooth_transport *t, struct userdata *u) {
    pa_volume_t volume;
    pa_cvolume v;

    pa_assert(t);
    pa_assert(u);

    if (t != u->transport)
      return PA_HOOK_OK;

    volume = t->source_volume;

    if (!u->source) {
        pa_log_warn("Received peer transport volume change without connected source");
        return PA_HOOK_OK;
    }

    source_setup_volume_callback(u->source);

    pa_cvolume_set(&v, u->decoder_sample_spec.channels, volume);

    if (pa_bluetooth_profile_should_attenuate_volume(t->profile))
        pa_source_set_volume(u->source, &v, true, true);
    else
        pa_source_volume_changed(u->source, &v);

    return PA_HOOK_OK;
}

static char* make_message_handler_path(const char *name) {
    return pa_sprintf_malloc("/card/%s/bluez", name);
}

static void switch_codec_cb_handler(bool success, pa_bluetooth_profile_t profile, void *userdata)
{
    struct userdata *u = (struct userdata *) userdata;

    if (!success)
        goto off;

    u->profile = profile;

    if (init_profile(u) < 0) {
        pa_log_info("Failed to initialise profile after codec switching");
        goto off;
    }

    if (u->sink || u->source)
        if (start_thread(u) < 0) {
            pa_log_info("Failed to start thread after codec switching");
            goto off;
        }

    pa_log_info("Codec successfully switched to %s with profile: %s",
            u->bt_codec->name, pa_bluetooth_profile_to_string(u->profile));

    return;

off:
    pa_assert_se(pa_card_set_profile(u->card, pa_hashmap_get(u->card->profiles, "off"), false) >= 0);
}

static char *list_codecs(struct userdata *u) {
    const pa_a2dp_codec_capabilities *a2dp_capabilities;
    const pa_a2dp_codec_id *key;
    pa_hashmap *a2dp_endpoints;
    pa_json_encoder *encoder;
    unsigned int i;
    bool is_a2dp_sink;
    void *state;

    encoder = pa_json_encoder_new();

    pa_json_encoder_begin_element_array(encoder);

    if (pa_bluetooth_profile_is_a2dp(u->profile)) {
        is_a2dp_sink = u->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK;

        a2dp_endpoints = is_a2dp_sink ? u->device->a2dp_sink_endpoints : u->device->a2dp_source_endpoints;

        PA_HASHMAP_FOREACH_KV(key, a2dp_capabilities, a2dp_endpoints, state) {
            for (i = 0; i < pa_bluetooth_a2dp_endpoint_conf_count(); i++) {
                const pa_a2dp_endpoint_conf *endpoint_conf;

                endpoint_conf = pa_bluetooth_a2dp_endpoint_conf_iter(i);

                if (memcmp(key, &endpoint_conf->id, sizeof(pa_a2dp_codec_id)) == 0) {
                    if (endpoint_conf->can_be_supported(is_a2dp_sink)) {
                        pa_json_encoder_begin_element_object(encoder);

                        pa_json_encoder_add_member_string(encoder, "name", endpoint_conf->bt_codec.name);
                        pa_json_encoder_add_member_string(encoder, "description", endpoint_conf->bt_codec.description);

                        pa_json_encoder_end_object(encoder);
                    }
                }
            }
        }
    } else {
        /* find out active codec selection from device profile */
        for (i = 0; i < pa_bluetooth_hf_codec_count(); i++) {
            const pa_bt_codec *hf_codec;

            hf_codec = pa_bluetooth_hf_codec_iter(i);

            if (true) {
                pa_json_encoder_begin_element_object(encoder);

                pa_json_encoder_add_member_string(encoder, "name", hf_codec->name);
                pa_json_encoder_add_member_string(encoder, "description", hf_codec->description);

                pa_json_encoder_end_object(encoder);
            }
        }
    }

    pa_json_encoder_end_array(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

static int bluez5_device_message_handler(const char *object_path, const char *message, const pa_json_object *parameters, char **response, void *userdata) {
    char *message_handler_path;
    pa_hashmap *capabilities_hashmap;
    pa_bluetooth_profile_t profile;
    const pa_a2dp_endpoint_conf *endpoint_conf;
    const char *codec_name;
    struct userdata *u = userdata;
    bool is_a2dp_sink;

    pa_assert(u);
    pa_assert(message);
    pa_assert(response);

    message_handler_path = make_message_handler_path(u->card->name);

    if (!object_path || !pa_streq(object_path, message_handler_path)) {
        pa_xfree(message_handler_path);
        return -PA_ERR_NOENTITY;
    }

    pa_xfree(message_handler_path);

    if (u->device->codec_switching_in_progress) {
        pa_log_info("Codec switching operation already in progress");
        return -PA_ERR_INVALID;
    }

    if (!u->device->adapter->application_registered) {
        pa_log_info("Old BlueZ version was detected, only SBC codec supported.");
        return -PA_ERR_NOTIMPLEMENTED;
    }

    if (u->profile == PA_BLUETOOTH_PROFILE_OFF) {
        pa_log_info("Bluetooth profile is off. Message cannot be handled.");
        return -PA_ERR_INVALID;
    }

    if (pa_streq(message, "switch-codec")) {
        if (u->profile != PA_BLUETOOTH_PROFILE_A2DP_SINK &&
            u->profile != PA_BLUETOOTH_PROFILE_A2DP_SOURCE) {
            pa_log_info("Switching codecs only allowed for A2DP sink or source");
            return -PA_ERR_INVALID;
        }

        if (!parameters) {
            pa_log_info("Codec switching operation requires codec name string parameter");
            return -PA_ERR_INVALID;
        }

        if (pa_json_object_get_type(parameters) != PA_JSON_TYPE_STRING) {
            pa_log_info("Codec name object parameter must be a string");
            return -PA_ERR_INVALID;
        }

        codec_name = pa_json_object_get_string(parameters);

        if (u->bt_codec && pa_streq(codec_name, u->bt_codec->name)) {
            pa_log_info("Requested codec is currently selected codec");
            return -PA_ERR_INVALID;
        }

        endpoint_conf = pa_bluetooth_get_a2dp_endpoint_conf(codec_name);
        if (endpoint_conf == NULL) {
            pa_log_info("Invalid codec %s specified for switching", codec_name);
            return -PA_ERR_INVALID;
        }

        is_a2dp_sink = u->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK;

        if (!endpoint_conf->can_be_supported(is_a2dp_sink)) {
            pa_log_info("Codec not found on system");
            return -PA_ERR_NOTSUPPORTED;
        }

        /*
         * We need to check if we have valid sink or source endpoints which
         * were registered during the negotiation process. If we do, then we
         * check if the specified codec is present among the codecs supported
         * by the remote endpoint.
         */
        if (pa_hashmap_isempty(is_a2dp_sink ? u->device->a2dp_sink_endpoints : u->device->a2dp_source_endpoints)) {
            pa_log_info("No device endpoints found. Codec switching not allowed.");
            return -PA_ERR_INVALID;
        }

        capabilities_hashmap = pa_hashmap_get(is_a2dp_sink ? u->device->a2dp_sink_endpoints : u->device->a2dp_source_endpoints, &endpoint_conf->id);
        if (!capabilities_hashmap) {
            pa_log_info("No remote endpoint found for %s codec. Codec not supported by remote endpoint.",
                    endpoint_conf->bt_codec.name);
            return -PA_ERR_INVALID;
        }

        pa_log_info("Initiating codec switching process to %s", endpoint_conf->bt_codec.name);

        /*
         * The current profile needs to be saved before we stop the thread and
         * initiate the switch. u->profile will be changed in other places
         * depending on the state of transport and port availability.
         */
        profile = u->profile;

        stop_thread(u);

        if (!pa_bluetooth_device_switch_codec(u->device, profile, capabilities_hashmap, endpoint_conf, switch_codec_cb_handler, userdata)
                && !u->device->codec_switching_in_progress)
            goto profile_off;

        return PA_OK;
    } else if (pa_streq(message, "list-codecs")) {
        *response = list_codecs(u);
        return PA_OK;
    } else if (pa_streq(message, "get-codec")) {
        pa_json_encoder *encoder;
        encoder = pa_json_encoder_new();

        if (u->bt_codec)
            pa_json_encoder_add_element_string(encoder, u->bt_codec->name);
        else
            pa_json_encoder_add_element_null(encoder);

        *response = pa_json_encoder_to_string_free(encoder);

        return PA_OK;
    }


    return -PA_ERR_NOTIMPLEMENTED;

profile_off:
    pa_assert_se(pa_card_set_profile(u->card, pa_hashmap_get(u->card->profiles, "off"), false) >= 0);

    return -PA_ERR_IO;
}

/* Run from main thread context */
static int device_process_msg(pa_msgobject *obj, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct bluetooth_msg *m = BLUETOOTH_MSG(obj);
    struct userdata *u = m->card->userdata;

    switch (code) {
        case BLUETOOTH_MESSAGE_IO_THREAD_FAILED:
            if (m->card->module->unload_requested)
                break;

            pa_log_debug("Switching the profile to off due to IO thread failure.");
            pa_assert_se(pa_card_set_profile(m->card, pa_hashmap_get(m->card->profiles, "off"), false) >= 0);
            break;
        case BLUETOOTH_MESSAGE_STREAM_FD_HUP:
            if (u->transport->state > PA_BLUETOOTH_TRANSPORT_STATE_IDLE)
                pa_bluetooth_transport_set_state(u->transport, PA_BLUETOOTH_TRANSPORT_STATE_IDLE);
            break;
        case BLUETOOTH_MESSAGE_SET_TRANSPORT_PLAYING:
            /* transport_acquired needs to be checked here, because a message could have been
             * pending when the profile was switched. If the new transport has been acquired
             * correctly, the call below will have no effect because the transport state is
             * already PLAYING. If transport_acquire() failed for the new profile, the transport
             * state should not be changed. If the transport has been released for other reasons
             * (I/O thread shutdown), transport_acquired will also be false. */
            if (u->transport_acquired)
                pa_bluetooth_transport_set_state(u->transport, PA_BLUETOOTH_TRANSPORT_STATE_PLAYING);
            break;
    }

    return 0;
}

/* Run from main thread */
static pa_hook_result_t a2dp_source_output_fixate_hook_callback(pa_core *c, pa_source_output_new_data *new_data, struct userdata *u) {
    double volume_factor_dB;
    pa_cvolume cv;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);

    /* When transport is released, there is no decoder and no codec */
    if (!u->bt_codec || !u->decoder_info)
        return PA_HOOK_OK;

    if (!u->bt_codec->get_source_output_volume_factor_dB)
        return PA_HOOK_OK;

    volume_factor_dB = u->bt_codec->get_source_output_volume_factor_dB(u->decoder_info);

    pa_cvolume_set(&cv, u->decoder_sample_spec.channels, pa_sw_volume_from_dB(volume_factor_dB));
    pa_source_output_new_data_apply_volume_factor_source(new_data, &cv);

    return PA_HOOK_OK;
}

int pa__init(pa_module* m) {
    struct userdata *u;
    const char *path;
    pa_modargs *ma;
    bool autodetect_mtu, avrcp_absolute_volume;
    char *message_handler_path;
    uint32_t output_rate_refresh_interval_ms;

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;
    u->message_handler_registered = false;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        goto fail_free_modargs;
    }

    if (!(path = pa_modargs_get_value(ma, "path", NULL))) {
        pa_log_error("Failed to get device path from module arguments");
        goto fail_free_modargs;
    }

    if ((u->discovery = pa_shared_get(u->core, "bluetooth-discovery")))
        pa_bluetooth_discovery_ref(u->discovery);
    else {
        pa_log_error("module-bluez5-discover doesn't seem to be loaded, refusing to load module-bluez5-device");
        goto fail_free_modargs;
    }

    if (!(u->device = pa_bluetooth_discovery_get_device_by_path(u->discovery, path))) {
        pa_log_error("%s is unknown", path);
        goto fail_free_modargs;
    }

    autodetect_mtu = false;
    if (pa_modargs_get_value_boolean(ma, "autodetect_mtu", &autodetect_mtu) < 0) {
        pa_log("Invalid boolean value for autodetect_mtu parameter");
        goto fail_free_modargs;
    }

    u->device->autodetect_mtu = autodetect_mtu;

    output_rate_refresh_interval_ms = DEFAULT_OUTPUT_RATE_REFRESH_INTERVAL_MS;
    if (pa_modargs_get_value_u32(ma, "output_rate_refresh_interval_ms", &output_rate_refresh_interval_ms) < 0) {
        pa_log("Invalid value for output_rate_refresh_interval parameter.");
        goto fail_free_modargs;
    }

    u->device->output_rate_refresh_interval_ms = output_rate_refresh_interval_ms;

    avrcp_absolute_volume = true;
    if (pa_modargs_get_value_boolean(ma, "avrcp_absolute_volume", &avrcp_absolute_volume) < 0) {
        pa_log("Invalid boolean value for avrcp_absolute_volume parameter");
        goto fail_free_modargs;
    }

    u->device->avrcp_absolute_volume = avrcp_absolute_volume;

    pa_modargs_free(ma);

    u->device_connection_changed_slot =
        pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery, PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED),
                        PA_HOOK_NORMAL, (pa_hook_cb_t) device_connection_changed_cb, u);

    u->device_battery_level_changed_slot =
        pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery, PA_BLUETOOTH_HOOK_DEVICE_BATTERY_LEVEL_CHANGED),
                        PA_HOOK_NORMAL, (pa_hook_cb_t) device_battery_level_changed_cb, u);

    u->transport_state_changed_slot =
        pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery, PA_BLUETOOTH_HOOK_TRANSPORT_STATE_CHANGED),
                        PA_HOOK_NORMAL, (pa_hook_cb_t) transport_state_changed_cb, u);

    u->transport_sink_volume_changed_slot =
        pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery, PA_BLUETOOTH_HOOK_TRANSPORT_SINK_VOLUME_CHANGED), PA_HOOK_NORMAL, (pa_hook_cb_t) transport_sink_volume_changed_cb, u);

    u->transport_source_volume_changed_slot =
        pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery, PA_BLUETOOTH_HOOK_TRANSPORT_SOURCE_VOLUME_CHANGED), PA_HOOK_NORMAL, (pa_hook_cb_t) transport_source_volume_changed_cb, u);

    u->source_output_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) a2dp_source_output_fixate_hook_callback, u);

    if (add_card(u) < 0)
        goto fail;

    if (!(u->msg = pa_msgobject_new(bluetooth_msg)))
        goto fail;

    u->msg->parent.process_msg = device_process_msg;
    u->msg->card = u->card;
    u->stream_setup_done = false;

    if (u->profile != PA_BLUETOOTH_PROFILE_OFF)
        if (init_profile(u) < 0)
            goto off;

    if (u->sink || u->source)
        if (start_thread(u) < 0)
            goto off;

    message_handler_path = make_message_handler_path(u->card->name);
    pa_message_handler_register(m->core, message_handler_path, "Bluez5 device message handler",
            bluez5_device_message_handler, (void *) u);
    pa_log_info("Bluez5 device message handler registered at path: %s", message_handler_path);
    pa_xfree(message_handler_path);
    u->message_handler_registered = true;

    return 0;

off:
    stop_thread(u);

    pa_assert_se(pa_card_set_profile(u->card, pa_hashmap_get(u->card->profiles, "off"), false) >= 0);

    return 0;

fail_free_modargs:

    if (ma)
        pa_modargs_free(ma);

fail:

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    char *message_handler_path;
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->message_handler_registered) {
        message_handler_path = make_message_handler_path(u->card->name);
        pa_message_handler_unregister(m->core, message_handler_path);
        pa_xfree(message_handler_path);
    }

    stop_thread(u);

    if (u->source_output_new_hook_slot)
        pa_hook_slot_free(u->source_output_new_hook_slot);

    if (u->device_connection_changed_slot)
        pa_hook_slot_free(u->device_connection_changed_slot);

    if (u->device_battery_level_changed_slot)
        pa_hook_slot_free(u->device_battery_level_changed_slot);

    if (u->transport_state_changed_slot)
        pa_hook_slot_free(u->transport_state_changed_slot);

    if (u->transport_sink_volume_changed_slot)
        pa_hook_slot_free(u->transport_sink_volume_changed_slot);

    if (u->transport_source_volume_changed_slot)
        pa_hook_slot_free(u->transport_source_volume_changed_slot);

    if (u->encoder_buffer)
        pa_xfree(u->encoder_buffer);

    if (u->decoder_buffer)
        pa_xfree(u->decoder_buffer);

    if (u->msg)
        pa_xfree(u->msg);

    if (u->card)
        pa_card_free(u->card);

    if (u->discovery)
        pa_bluetooth_discovery_unref(u->discovery);

    pa_xfree(u->output_port_name);
    pa_xfree(u->input_port_name);

    pa_xfree(u);
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return (u->sink ? pa_sink_linked_by(u->sink) : 0) + (u->source ? pa_source_linked_by(u->source) : 0);
}
