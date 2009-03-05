/***
  This file is part of PulseAudio.

  Copyright 2008 Joao Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <arpa/inet.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/sample.h>
#include <pulse/i18n.h>

#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/time-smoother.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/namereg.h>

#include <modules/dbus-util.h>

#include "module-bluetooth-device-symdef.h"
#include "ipc.h"
#include "sbc.h"
#include "rtp.h"
#include "bluetooth-util.h"

#define MAX_BITPOOL 64
#define MIN_BITPOOL 2U
#define SOL_SCO 17
#define SCO_TXBUFS 0x03
#define SCO_RXBUFS 0x04

PA_MODULE_AUTHOR("Joao Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Bluetooth audio sink and source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "name=<name for the card/sink/source, to be prefixed> "
        "card_name=<name for the card> "
        "sink_name=<name for the sink> "
        "source_name=<name for the source> "
        "address=<address of the device> "
        "profile=<a2dp|hsp> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "path=<device object path> "
        "sco_sink=<SCO over PCM sink name> "
        "sco_source=<SCO over PCM source name>");

static const char* const valid_modargs[] = {
    "name",
    "card_name",
    "sink_name",
    "source_name",
    "address",
    "profile",
    "rate",
    "channels",
    "path",
    "sco_sink",
    "sco_source",
    NULL
};

struct a2dp_info {
    sbc_capabilities_t sbc_capabilities;
    sbc_t sbc;                           /* Codec data */
    pa_bool_t sbc_initialized;           /* Keep track if the encoder is initialized */
    size_t codesize;                     /* SBC codesize */

    void* buffer;                        /* Codec transfer buffer */
    size_t buffer_size;                  /* Size of the buffer */

    uint16_t seq_num;                    /* Cumulative packet sequence */
};

struct hsp_info {
    pcm_capabilities_t pcm_capabilities;
    pa_sink *sco_sink;
    pa_source *sco_source;
    pa_hook_slot *sink_state_changed_slot;
    pa_hook_slot *source_state_changed_slot;
};

enum profile {
    PROFILE_A2DP,
    PROFILE_HSP,
    PROFILE_OFF
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_card *card;
    pa_sink *sink;
    pa_source *source;

    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    pa_thread *thread;

    uint64_t read_index, write_index;
    pa_usec_t started_at;
    pa_smoother *read_smoother;

    pa_memchunk write_memchunk;

    pa_sample_spec sample_spec, requested_sample_spec;

    int service_fd;
    int stream_fd;

    size_t link_mtu;
    size_t block_size;

    struct a2dp_info a2dp;
    struct hsp_info hsp;
    pa_dbus_connection *connection;

    enum profile profile;

    pa_modargs *modargs;

    pa_bluetooth_device *device;

    int stream_write_type, stream_read_type;
    int service_write_type, service_read_type;
};

static int init_bt(struct userdata *u);
static int init_profile(struct userdata *u);

static int service_send(struct userdata *u, const bt_audio_msg_header_t *msg) {
    ssize_t r;

    pa_assert(u);
    pa_assert(u->service_fd >= 0);
    pa_assert(msg);
    pa_assert(msg->length > 0);

    pa_log_debug("Sending %s -> %s",
                 pa_strnull(bt_audio_strtype(msg->type)),
                 pa_strnull(bt_audio_strname(msg->name)));

    if ((r = pa_loop_write(u->service_fd, msg, msg->length, &u->service_write_type)) == (ssize_t) msg->length)
        return 0;

    if (r < 0)
        pa_log_error("Error sending data to audio service: %s", pa_cstrerror(errno));
    else
        pa_log_error("Short write()");

    return -1;
}

static int service_recv(struct userdata *u, bt_audio_msg_header_t *msg, size_t room) {
    ssize_t r;

    pa_assert(u);
    pa_assert(u->service_fd >= 0);
    pa_assert(msg);

    if (room <= 0)
        room = BT_SUGGESTED_BUFFER_SIZE;

    pa_log_debug("Trying to receive message from audio service...");

    /* First, read the header */
    if ((r = pa_loop_read(u->service_fd, msg, sizeof(*msg), &u->service_read_type)) != sizeof(*msg))
        goto read_fail;

    if (msg->length < sizeof(*msg)) {
        pa_log_error("Invalid message size.");
        return -1;
    }

    /* Secondly, read the payload */
    if (msg->length > sizeof(*msg)) {

        size_t remains = msg->length - sizeof(*msg);

        if ((r = pa_loop_read(u->service_fd,
                              (uint8_t*) msg + sizeof(*msg),
                              remains,
                              &u->service_read_type)) != (ssize_t) remains)
            goto read_fail;
    }

    pa_log_debug("Received %s <- %s",
                 pa_strnull(bt_audio_strtype(msg->type)),
                 pa_strnull(bt_audio_strname(msg->name)));

    return 0;

read_fail:

    if (r < 0)
        pa_log_error("Error receiving data from audio service: %s", pa_cstrerror(errno));
    else
        pa_log_error("Short read()");

    return -1;
}

static ssize_t service_expect(struct userdata*u, bt_audio_msg_header_t *rsp, size_t room, uint8_t expected_name, size_t expected_size) {
    int r;

    pa_assert(u);
    pa_assert(u->service_fd >= 0);
    pa_assert(rsp);

    if ((r = service_recv(u, rsp, room)) < 0)
        return r;

    if ((rsp->type != BT_INDICATION && rsp->type != BT_RESPONSE) ||
        rsp->name != expected_name ||
        (expected_size > 0 && rsp->length != expected_size)) {

        if (rsp->type == BT_ERROR && rsp->length == sizeof(bt_audio_error_t))
            pa_log_error("Received error condition: %s", pa_cstrerror(((bt_audio_error_t*) rsp)->posix_errno));
        else
            pa_log_error("Bogus message %s received while %s was expected",
                         pa_strnull(bt_audio_strname(rsp->name)),
                         pa_strnull(bt_audio_strname(expected_name)));
        return -1;
    }

    return 0;
}

static int parse_caps(struct userdata *u, const struct bt_get_capabilities_rsp *rsp) {
    uint16_t bytes_left;
    const codec_capabilities_t *codec;

    pa_assert(u);
    pa_assert(rsp);

    bytes_left = rsp->h.length - sizeof(*rsp);

    if (bytes_left < sizeof(codec_capabilities_t)) {
        pa_log_error("Packet too small to store codec information.");
        return -1;
    }

    codec = (codec_capabilities_t *) rsp->data; /** ALIGNMENT? **/

    pa_log_debug("Payload size is %lu %lu", (unsigned long) bytes_left, (unsigned long) sizeof(*codec));

    if ((u->profile == PROFILE_A2DP && codec->transport != BT_CAPABILITIES_TRANSPORT_A2DP) ||
        (u->profile == PROFILE_HSP && codec->transport != BT_CAPABILITIES_TRANSPORT_SCO)) {
        pa_log_error("Got capabilities for wrong codec.");
        return -1;
    }

    if (u->profile == PROFILE_HSP) {

        if (bytes_left <= 0 || codec->length != sizeof(u->hsp.pcm_capabilities))
            return -1;

        pa_assert(codec->type == BT_HFP_CODEC_PCM);

        memcpy(&u->hsp.pcm_capabilities, codec, sizeof(u->hsp.pcm_capabilities));

    } else if (u->profile == PROFILE_A2DP) {

        while (bytes_left > 0) {
            if (codec->type == BT_A2DP_CODEC_SBC)
                break;

            bytes_left -= codec->length;
            codec = (const codec_capabilities_t*) ((const uint8_t*) codec + codec->length);
        }

        if (bytes_left <= 0 || codec->length != sizeof(u->a2dp.sbc_capabilities))
            return -1;

        pa_assert(codec->type == BT_A2DP_CODEC_SBC);

        memcpy(&u->a2dp.sbc_capabilities, codec, sizeof(u->a2dp.sbc_capabilities));
    }

    return 0;
}

static int get_caps(struct userdata *u) {
    union {
        struct bt_get_capabilities_req getcaps_req;
        struct bt_get_capabilities_rsp getcaps_rsp;
        bt_audio_error_t error;
        uint8_t buf[BT_SUGGESTED_BUFFER_SIZE];
    } msg;

    pa_assert(u);

    memset(&msg, 0, sizeof(msg));
    msg.getcaps_req.h.type = BT_REQUEST;
    msg.getcaps_req.h.name = BT_GET_CAPABILITIES;
    msg.getcaps_req.h.length = sizeof(msg.getcaps_req);

    pa_strlcpy(msg.getcaps_req.device, u->device->address, sizeof(msg.getcaps_req.device));
    if (u->profile == PROFILE_A2DP)
        msg.getcaps_req.transport = BT_CAPABILITIES_TRANSPORT_A2DP;
    else {
        pa_assert(u->profile == PROFILE_HSP);
        msg.getcaps_req.transport = BT_CAPABILITIES_TRANSPORT_SCO;
    }
    msg.getcaps_req.flags = BT_FLAG_AUTOCONNECT;

    if (service_send(u, &msg.getcaps_req.h) < 0)
        return -1;

    if (service_expect(u, &msg.getcaps_rsp.h, sizeof(msg), BT_GET_CAPABILITIES, 0) < 0)
        return -1;

    return parse_caps(u, &msg.getcaps_rsp);
}

static uint8_t a2dp_default_bitpool(uint8_t freq, uint8_t mode) {

    switch (freq) {
        case BT_SBC_SAMPLING_FREQ_16000:
        case BT_SBC_SAMPLING_FREQ_32000:
            return 53;

        case BT_SBC_SAMPLING_FREQ_44100:

            switch (mode) {
                case BT_A2DP_CHANNEL_MODE_MONO:
                case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
                    return 31;

                case BT_A2DP_CHANNEL_MODE_STEREO:
                case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
                    return 53;

                default:
                    pa_log_warn("Invalid channel mode %u", mode);
                    return 53;
            }

        case BT_SBC_SAMPLING_FREQ_48000:

            switch (mode) {
                case BT_A2DP_CHANNEL_MODE_MONO:
                case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
                    return 29;

                case BT_A2DP_CHANNEL_MODE_STEREO:
                case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
                    return 51;

                default:
                    pa_log_warn("Invalid channel mode %u", mode);
                    return 51;
            }

        default:
            pa_log_warn("Invalid sampling freq %u", freq);
            return 53;
    }
}

static int setup_a2dp(struct userdata *u) {
    sbc_capabilities_t *cap;
    int i;

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } freq_table[] = {
        { 16000U, BT_SBC_SAMPLING_FREQ_16000 },
        { 32000U, BT_SBC_SAMPLING_FREQ_32000 },
        { 44100U, BT_SBC_SAMPLING_FREQ_44100 },
        { 48000U, BT_SBC_SAMPLING_FREQ_48000 }
    };

    pa_assert(u);
    pa_assert(u->profile == PROFILE_A2DP);

    cap = &u->a2dp.sbc_capabilities;

    /* Find the lowest freq that is at least as high as the requested
     * sampling rate */
    for (i = 0; (unsigned) i < PA_ELEMENTSOF(freq_table); i++)
        if (freq_table[i].rate >= u->sample_spec.rate && (cap->frequency & freq_table[i].cap)) {
            u->sample_spec.rate = freq_table[i].rate;
            cap->frequency = freq_table[i].cap;
            break;
        }

    if ((unsigned) i >= PA_ELEMENTSOF(freq_table)) {
        for (; i >= 0; i--) {
            if (cap->frequency & freq_table[i].cap) {
                u->sample_spec.rate = freq_table[i].rate;
                cap->frequency = freq_table[i].cap;
                break;
            }
        }

        if (i < 0) {
            pa_log("Not suitable sample rate");
            return -1;
        }
    }

    if (u->sample_spec.channels <= 1) {
        if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_MONO) {
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_MONO;
            u->sample_spec.channels = 1;
        } else
            u->sample_spec.channels = 2;
    }

    if (u->sample_spec.channels >= 2) {
        u->sample_spec.channels = 2;

        if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_JOINT_STEREO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
        else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_STEREO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_STEREO;
        else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL;
        else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_MONO) {
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_MONO;
            u->sample_spec.channels = 1;
        } else {
            pa_log("No supported channel modes");
            return -1;
        }
    }

    if (cap->block_length & BT_A2DP_BLOCK_LENGTH_16)
        cap->block_length = BT_A2DP_BLOCK_LENGTH_16;
    else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_12)
        cap->block_length = BT_A2DP_BLOCK_LENGTH_12;
    else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_8)
        cap->block_length = BT_A2DP_BLOCK_LENGTH_8;
    else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_4)
        cap->block_length = BT_A2DP_BLOCK_LENGTH_4;
    else {
        pa_log_error("No supported block lengths");
        return -1;
    }

    if (cap->subbands & BT_A2DP_SUBBANDS_8)
        cap->subbands = BT_A2DP_SUBBANDS_8;
    else if (cap->subbands & BT_A2DP_SUBBANDS_4)
        cap->subbands = BT_A2DP_SUBBANDS_4;
    else {
        pa_log_error("No supported subbands");
        return -1;
    }

    if (cap->allocation_method & BT_A2DP_ALLOCATION_LOUDNESS)
        cap->allocation_method = BT_A2DP_ALLOCATION_LOUDNESS;
    else if (cap->allocation_method & BT_A2DP_ALLOCATION_SNR)
        cap->allocation_method = BT_A2DP_ALLOCATION_SNR;

    cap->min_bitpool = (uint8_t) PA_MAX(MIN_BITPOOL, cap->min_bitpool);
    cap->max_bitpool = (uint8_t) PA_MIN(a2dp_default_bitpool(cap->frequency, cap->channel_mode), cap->max_bitpool);

    return 0;
}

static void setup_sbc(struct a2dp_info *a2dp) {
    sbc_capabilities_t *active_capabilities;

    pa_assert(a2dp);

    active_capabilities = &a2dp->sbc_capabilities;

    if (a2dp->sbc_initialized)
        sbc_reinit(&a2dp->sbc, 0);
    else
        sbc_init(&a2dp->sbc, 0);
    a2dp->sbc_initialized = TRUE;

    switch (active_capabilities->frequency) {
        case BT_SBC_SAMPLING_FREQ_16000:
            a2dp->sbc.frequency = SBC_FREQ_16000;
            break;
        case BT_SBC_SAMPLING_FREQ_32000:
            a2dp->sbc.frequency = SBC_FREQ_32000;
            break;
        case BT_SBC_SAMPLING_FREQ_44100:
            a2dp->sbc.frequency = SBC_FREQ_44100;
            break;
        case BT_SBC_SAMPLING_FREQ_48000:
            a2dp->sbc.frequency = SBC_FREQ_48000;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (active_capabilities->channel_mode) {
        case BT_A2DP_CHANNEL_MODE_MONO:
            a2dp->sbc.mode = SBC_MODE_MONO;
            break;
        case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
            a2dp->sbc.mode = SBC_MODE_DUAL_CHANNEL;
            break;
        case BT_A2DP_CHANNEL_MODE_STEREO:
            a2dp->sbc.mode = SBC_MODE_STEREO;
            break;
        case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
            a2dp->sbc.mode = SBC_MODE_JOINT_STEREO;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (active_capabilities->allocation_method) {
        case BT_A2DP_ALLOCATION_SNR:
            a2dp->sbc.allocation = SBC_AM_SNR;
            break;
        case BT_A2DP_ALLOCATION_LOUDNESS:
            a2dp->sbc.allocation = SBC_AM_LOUDNESS;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (active_capabilities->subbands) {
        case BT_A2DP_SUBBANDS_4:
            a2dp->sbc.subbands = SBC_SB_4;
            break;
        case BT_A2DP_SUBBANDS_8:
            a2dp->sbc.subbands = SBC_SB_8;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (active_capabilities->block_length) {
        case BT_A2DP_BLOCK_LENGTH_4:
            a2dp->sbc.blocks = SBC_BLK_4;
            break;
        case BT_A2DP_BLOCK_LENGTH_8:
            a2dp->sbc.blocks = SBC_BLK_8;
            break;
        case BT_A2DP_BLOCK_LENGTH_12:
            a2dp->sbc.blocks = SBC_BLK_12;
            break;
        case BT_A2DP_BLOCK_LENGTH_16:
            a2dp->sbc.blocks = SBC_BLK_16;
            break;
        default:
            pa_assert_not_reached();
    }

    a2dp->sbc.bitpool = active_capabilities->max_bitpool;
    a2dp->codesize = (uint16_t) sbc_get_codesize(&a2dp->sbc);
}

static int set_conf(struct userdata *u) {
    union {
        struct bt_set_configuration_req setconf_req;
        struct bt_set_configuration_rsp setconf_rsp;
        bt_audio_error_t error;
        uint8_t buf[BT_SUGGESTED_BUFFER_SIZE];
    } msg;

    if (u->profile == PROFILE_A2DP ) {
        u->sample_spec.format = PA_SAMPLE_S16LE;

        if (setup_a2dp(u) < 0)
            return -1;
    } else {
        pa_assert(u->profile == PROFILE_HSP);

        u->sample_spec.format = PA_SAMPLE_S16LE;
        u->sample_spec.channels = 1;
        u->sample_spec.rate = 8000;
    }

    memset(&msg, 0, sizeof(msg));
    msg.setconf_req.h.type = BT_REQUEST;
    msg.setconf_req.h.name = BT_SET_CONFIGURATION;
    msg.setconf_req.h.length = sizeof(msg.setconf_req);

    pa_strlcpy(msg.setconf_req.device, u->device->address, sizeof(msg.setconf_req.device));
    msg.setconf_req.access_mode = u->profile == PROFILE_A2DP ? BT_CAPABILITIES_ACCESS_MODE_WRITE : BT_CAPABILITIES_ACCESS_MODE_READWRITE;

    msg.setconf_req.codec.transport = u->profile == PROFILE_A2DP ? BT_CAPABILITIES_TRANSPORT_A2DP : BT_CAPABILITIES_TRANSPORT_SCO;

    if (u->profile == PROFILE_A2DP) {
        memcpy(&msg.setconf_req.codec, &u->a2dp.sbc_capabilities, sizeof(u->a2dp.sbc_capabilities));
        msg.setconf_req.h.length += msg.setconf_req.codec.length - sizeof(msg.setconf_req.codec);
    }

    if (service_send(u, &msg.setconf_req.h) < 0)
        return -1;

    if (service_expect(u, &msg.setconf_rsp.h, sizeof(msg), BT_SET_CONFIGURATION, sizeof(msg.setconf_rsp)) < 0)
        return -1;

    if ((u->profile == PROFILE_A2DP && msg.setconf_rsp.transport != BT_CAPABILITIES_TRANSPORT_A2DP) ||
        (u->profile == PROFILE_HSP && msg.setconf_rsp.transport != BT_CAPABILITIES_TRANSPORT_SCO)) {
        pa_log("Transport doesn't match what we requested.");
        return -1;
    }

    if ((u->profile == PROFILE_A2DP && msg.setconf_rsp.access_mode != BT_CAPABILITIES_ACCESS_MODE_WRITE) ||
        (u->profile == PROFILE_HSP && msg.setconf_rsp.access_mode != BT_CAPABILITIES_ACCESS_MODE_READWRITE)) {
        pa_log("Access mode doesn't match what we requested.");
        return -1;
    }

    u->link_mtu = msg.setconf_rsp.link_mtu;

    /* setup SBC encoder now we agree on parameters */
    if (u->profile == PROFILE_A2DP) {
        setup_sbc(&u->a2dp);
        u->block_size = u->a2dp.codesize;
        pa_log_info("SBC parameters:\n\tallocation=%u\n\tsubbands=%u\n\tblocks=%u\n\tbitpool=%u\n",
                    u->a2dp.sbc.allocation, u->a2dp.sbc.subbands, u->a2dp.sbc.blocks, u->a2dp.sbc.bitpool);
    } else
        u->block_size = u->link_mtu;

    return 0;
}

/* from IO thread */
static int start_stream_fd(struct userdata *u) {
    union {
        bt_audio_msg_header_t rsp;
        struct bt_start_stream_req start_req;
        struct bt_start_stream_rsp start_rsp;
        struct bt_new_stream_ind streamfd_ind;
        bt_audio_error_t error;
        uint8_t buf[BT_SUGGESTED_BUFFER_SIZE];
    } msg;
    struct pollfd *pollfd;

    pa_assert(u);
    pa_assert(u->rtpoll);
    pa_assert(!u->rtpoll_item);
    pa_assert(u->stream_fd < 0);

    memset(msg.buf, 0, BT_SUGGESTED_BUFFER_SIZE);
    msg.start_req.h.type = BT_REQUEST;
    msg.start_req.h.name = BT_START_STREAM;
    msg.start_req.h.length = sizeof(msg.start_req);

    if (service_send(u, &msg.start_req.h) < 0)
        return -1;

    if (service_expect(u, &msg.rsp, sizeof(msg), BT_START_STREAM, sizeof(msg.start_rsp)) < 0)
        return -1;

    if (service_expect(u, &msg.rsp, sizeof(msg), BT_NEW_STREAM, sizeof(msg.streamfd_ind)) < 0)
        return -1;

    if ((u->stream_fd = bt_audio_service_get_data_fd(u->service_fd)) < 0) {
        pa_log("Failed to get stream fd from audio service.");
        return -1;
    }

/*     setsockopt(u->stream_fd, SOL_SCO, SCO_TXBUFS, &period_count, sizeof(period_count)); */
/*     setsockopt(u->stream_fd, SOL_SCO, SCO_SNDBUF, &period_count, sizeof(period_count)); */

    pa_make_fd_nonblock(u->stream_fd);
    pa_make_socket_low_delay(u->stream_fd);

    u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    pollfd->fd = u->stream_fd;
    pollfd->events = pollfd->revents = 0;

    return 0;
}

/* from IO thread */
static int stop_stream_fd(struct userdata *u) {
    union {
        bt_audio_msg_header_t rsp;
        struct bt_stop_stream_req start_req;
        struct bt_stop_stream_rsp start_rsp;
        bt_audio_error_t error;
        uint8_t buf[BT_SUGGESTED_BUFFER_SIZE];
    } msg;
    int r = 0;

    pa_assert(u);
    pa_assert(u->rtpoll);
    pa_assert(u->rtpoll_item);
    pa_assert(u->stream_fd >= 0);

    pa_rtpoll_item_free(u->rtpoll_item);
    u->rtpoll_item = NULL;

    memset(msg.buf, 0, BT_SUGGESTED_BUFFER_SIZE);
    msg.start_req.h.type = BT_REQUEST;
    msg.start_req.h.name = BT_STOP_STREAM;
    msg.start_req.h.length = sizeof(msg.start_req);

    if (service_send(u, &msg.start_req.h) < 0 ||
        service_expect(u, &msg.rsp, sizeof(msg), BT_STOP_STREAM, sizeof(msg.start_rsp)) < 0)
        r = -1;

    pa_close(u->stream_fd);
    u->stream_fd = -1;

    return r;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;
    pa_bool_t failed = FALSE;
    int r;

    pa_assert(u->sink == PA_SINK(o));

    pa_log_debug("got message: %d", code);
    switch (code) {

        case PA_SINK_MESSAGE_SET_STATE:

            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {

                case PA_SINK_SUSPENDED:
                    pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

                    /* Stop the device if the source is suspended as well */
                    if (!u->source || u->source->state == PA_SOURCE_SUSPENDED)
                        /* We deliberately ignore whether stopping
                         * actually worked. Since the stream_fd is
                         * closed it doesn't really matter */
                        stop_stream_fd(u);

                    break;

                case PA_SINK_IDLE:
                case PA_SINK_RUNNING:
                    if (u->sink->thread_info.state != PA_SINK_SUSPENDED)
                        break;

                    /* Resume the device if the source was suspended as well */
                    if (!u->source || u->source->state == PA_SOURCE_SUSPENDED)
                        if (start_stream_fd(u) < 0)
                            failed = TRUE;

                    u->started_at = pa_rtclock_usec();
                    break;

                case PA_SINK_UNLINKED:
                case PA_SINK_INIT:
                case PA_SINK_INVALID_STATE:
                    ;
            }
            break;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            *((pa_usec_t*) data) = 0;
            return 0;
        }
    }

    r = pa_sink_process_msg(o, code, data, offset, chunk);

    return (r < 0 || !failed) ? r : -1;
}

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;
    pa_bool_t failed = FALSE;
    int r;

    pa_assert(u->source == PA_SOURCE(o));

    pa_log_debug("got message: %d", code);
    switch (code) {

        case PA_SOURCE_MESSAGE_SET_STATE:

            switch ((pa_source_state_t) PA_PTR_TO_UINT(data)) {

                case PA_SOURCE_SUSPENDED:
                    pa_assert(PA_SOURCE_IS_OPENED(u->source->thread_info.state));

                    /* Stop the device if the sink is suspended as well */
                    if (!u->sink || u->sink->state == PA_SINK_SUSPENDED)
                        stop_stream_fd(u);

                    pa_smoother_pause(u->read_smoother, pa_rtclock_usec());
                    break;

                case PA_SOURCE_IDLE:
                case PA_SOURCE_RUNNING:
                    if (u->source->thread_info.state != PA_SOURCE_SUSPENDED)
                        break;

                    /* Resume the device if the sink was suspended as well */
                    if (!u->sink || u->sink->thread_info.state == PA_SINK_SUSPENDED)
                        if (start_stream_fd(u) < 0)
                            failed = TRUE;

                    pa_smoother_resume(u->read_smoother, pa_rtclock_usec());
                    break;

                case PA_SOURCE_UNLINKED:
                case PA_SOURCE_INIT:
                case PA_SOURCE_INVALID_STATE:
                    ;
            }
            break;

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            *((pa_usec_t*) data) = 0;
            return 0;
        }

    }

    r = pa_source_process_msg(o, code, data, offset, chunk);

    return (r < 0 || !failed) ? r : -1;
}

static int hsp_process_render(struct userdata *u) {
    int ret = 0;
    pa_memchunk memchunk;

    pa_assert(u);
    pa_assert(u->profile == PROFILE_HSP);
    pa_assert(u->sink);

    pa_sink_render_full(u->sink, u->block_size, &memchunk);

    for (;;) {
        ssize_t l;
        const void *p;

        p = (const uint8_t*) pa_memblock_acquire(memchunk.memblock) + memchunk.index;
        l = pa_write(u->stream_fd, p, memchunk.length, &u->stream_write_type);
        pa_memblock_release(memchunk.memblock);

        pa_log_debug("Memblock written to socket: %lli bytes", (long long) l);

        pa_assert(l != 0);

        if (l < 0) {
            if (errno == EINTR)
                continue;
            else {
                pa_log_error("Failed to write data to SCO socket: %s", pa_cstrerror(errno));
                ret = -1;
                break;
            }
        } else {
            pa_assert((size_t) l <= memchunk.length);

            memchunk.index += (size_t) l;
            memchunk.length -= (size_t) l;

            u->write_index += (uint64_t) l;

            if (memchunk.length <= 0)
                break;
        }
    }

    pa_memblock_unref(memchunk.memblock);

    return ret;
}

static int hsp_process_push(struct userdata *u) {
    int ret = 0;
    pa_memchunk memchunk;

    pa_assert(u);
    pa_assert(u->profile == PROFILE_HSP);
    pa_assert(u->source);

    memchunk.memblock = pa_memblock_new(u->core->mempool, u->block_size);
    memchunk.index = memchunk.length = 0;

    for (;;) {
        ssize_t l;
        void *p;

        p = pa_memblock_acquire(memchunk.memblock);
        l = pa_read(u->stream_fd, p, pa_memblock_get_length(memchunk.memblock), &u->stream_read_type);
        pa_memblock_release(memchunk.memblock);

        if (l <= 0) {
            if (l < 0 && errno == EINTR)
                continue;
            else {
                pa_log_error("Failed to read data from SCO socket: %s", l < 0 ? pa_cstrerror(errno) : "EOF");
                ret = -1;
                break;
            }
        } else {
            memchunk.length = (size_t) l;
            u->read_index += (uint64_t) l;

            pa_source_post(u->source, &memchunk);
            break;
        }
    }

    pa_memblock_unref(memchunk.memblock);

    return ret;
}

static int a2dp_process_render(struct userdata *u) {
    size_t frame_size;
    struct a2dp_info *a2dp;
    struct rtp_header *header;
    struct rtp_payload *payload;
    size_t left;
    void *d;
    const void *p;
    unsigned frame_count;
    int written;
    uint64_t writing_at;

    pa_assert(u);
    pa_assert(u->profile == PROFILE_A2DP);
    pa_assert(u->sink);

    a2dp = &u->a2dp;

    if (a2dp->buffer_size < u->link_mtu) {
        a2dp->buffer_size = 2*u->link_mtu;
        pa_xfree(a2dp->buffer);
        a2dp->buffer = pa_xmalloc(a2dp->buffer_size);
    }

    header = (struct rtp_header*) a2dp->buffer;
    payload = (struct rtp_payload*) ((uint8_t*) a2dp->buffer + sizeof(*header));
    d = (uint8_t*) a2dp->buffer + sizeof(*header) + sizeof(*payload);
    left = a2dp->buffer_size - sizeof(*header) - sizeof(*payload);

    frame_size = sbc_get_frame_length(&a2dp->sbc);
    frame_count = 0;

    writing_at = u->write_index;

    do {
        int encoded;

        if (!u->write_memchunk.memblock)
            pa_sink_render_full(u->sink, u->block_size, &u->write_memchunk);

        p = (const uint8_t*) pa_memblock_acquire(u->write_memchunk.memblock) + u->write_memchunk.index;
        encoded = sbc_encode(&a2dp->sbc,
                             (void*) p, u->write_memchunk.length,
                             d, left,
                             &written);

        PA_ONCE_BEGIN {
            pa_log_debug("Using SBC encoder implementation: %s", pa_strnull(sbc_get_implementation_info(&a2dp->sbc)));
        } PA_ONCE_END;

        pa_memblock_release(u->write_memchunk.memblock);

        if (encoded <= 0) {
            pa_log_error("SBC encoding error (%d)", encoded);
            return -1;
        }

        pa_assert(written >= 0);

        pa_assert((size_t) encoded <= u->write_memchunk.length);
        pa_assert((size_t) written <= left);

/*         pa_log_debug("SBC: encoded: %d; written: %d", encoded, written); */

        u->write_memchunk.index += encoded;
        u->write_memchunk.length -= encoded;

        if (u->write_memchunk.length <= 0) {
            pa_memblock_unref(u->write_memchunk.memblock);
            pa_memchunk_reset(&u->write_memchunk);
        }

        u->write_index += encoded;

        d = (uint8_t*) d + written;
        left -= written;

        frame_count++;

    } while ((uint8_t*) d - (uint8_t*) a2dp->buffer + written < (ptrdiff_t) u->link_mtu);

    /* write it to the fifo */
    memset(a2dp->buffer, 0, sizeof(*header) + sizeof(*payload));
    payload->frame_count = frame_count;
    header->v = 2;
    header->pt = 1;
    header->sequence_number = htons(a2dp->seq_num++);
    header->timestamp = htonl(writing_at / frame_size);
    header->ssrc = htonl(1);

    p = a2dp->buffer;
    left = (uint8_t*) d - (uint8_t*) a2dp->buffer;

    for (;;) {
        ssize_t l;

        l = pa_write(u->stream_fd, p, left, &u->stream_write_type);
/*         pa_log_debug("write: requested %lu bytes; written %li bytes; mtu=%li", (unsigned long) left, (long) l, (unsigned long) u->link_mtu); */

        pa_assert(l != 0);

        if (l < 0) {
            if (errno == EINTR)
                continue;
            else {
                pa_log_error("Failed to write data to socket: %s", pa_cstrerror(errno));
                return -1;
            }
        } else {
            pa_assert((size_t) l <= left);

            d = (uint8_t*) d + l;
            left -= l;

            if (left <= 0)
                break;
        }
    }

    return 0;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    pa_bool_t do_write = FALSE, writable = FALSE;

    pa_assert(u);

    pa_log_debug("IO Thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    if (start_stream_fd(u) < 0)
        goto fail;

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    pa_smoother_set_time_offset(u->read_smoother, pa_rtclock_usec());

    for (;;) {
        struct pollfd *pollfd;
        int ret;
        pa_bool_t disable_timer = TRUE;

        pollfd = u->rtpoll_item ? pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL) : NULL;

        if (u->source && PA_SOURCE_IS_LINKED(u->source->thread_info.state)) {

            if (pollfd && (pollfd->revents & POLLIN)) {

                if (hsp_process_push(u) < 0)
                    goto fail;

                /* We just read something, so we are supposed to write something, too */
                do_write = TRUE;
            }
        }

        if (u->sink && PA_SINK_IS_LINKED(u->sink->thread_info.state)) {

            if (u->sink->thread_info.rewind_requested)
                pa_sink_process_rewind(u->sink, 0);

            if (pollfd) {
                if (pollfd->revents & POLLOUT)
                    writable = TRUE;

                if ((!u->source || !PA_SOURCE_IS_LINKED(u->source->thread_info.state)) && !do_write && writable) {
                    pa_usec_t time_passed;
                    uint64_t should_have_written;

                    /* Hmm, there is no input stream we could synchronize
                     * to. So let's do things by time */

                    time_passed = pa_rtclock_usec() - u->started_at;
                    should_have_written = pa_usec_to_bytes(time_passed, &u->sink->sample_spec);

                    do_write = u->write_index <= should_have_written ;
/*                 pa_log_debug("Time has come: %s", pa_yes_no(do_write)); */
                }

                if (writable && do_write) {

                    if (u->profile == PROFILE_A2DP) {
                        if (a2dp_process_render(u) < 0)
                            goto fail;
                    } else {
                        if (hsp_process_render(u) < 0)
                            goto fail;
                    }

                    do_write = FALSE;
                    writable = FALSE;
                }

                if ((!u->source || !PA_SOURCE_IS_LINKED(u->source->thread_info.state)) && !do_write) {
                    pa_usec_t time_passed, next_write_at, sleep_for;

                    /* Hmm, there is no input stream we could synchronize
                     * to. So let's estimate when we need to wake up the latest */

                    time_passed = pa_rtclock_usec() - u->started_at;
                    next_write_at = pa_bytes_to_usec(u->write_index, &u->sink->sample_spec);
                    sleep_for = time_passed < next_write_at ? next_write_at - time_passed : 0;

/*                 pa_log("Sleeping for %lu; time passed %lu, next write at %lu", (unsigned long) sleep_for, (unsigned long) time_passed, (unsigned long)next_write_at); */

                    pa_rtpoll_set_timer_relative(u->rtpoll, sleep_for);
                    disable_timer = FALSE;
                }
            }
        }

        if (disable_timer)
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Hmm, nothing to do. Let's sleep */
        if (pollfd)
            pollfd->events = (short) (((u->sink && PA_SINK_IS_OPENED(u->sink->thread_info.state) && !writable) ? POLLOUT : 0) |
                                      (u->source && PA_SOURCE_IS_OPENED(u->source->thread_info.state) ? POLLIN : 0));

        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        pollfd = u->rtpoll_item ? pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL) : NULL;

        if (pollfd && (pollfd->revents & ~(POLLOUT|POLLIN))) {
            pa_log_error("FD error.");
            goto fail;
        }
    }

fail:
    /* If this was no regular exit from the loop we have to continue processing messages until we receive PA_MESSAGE_SHUTDOWN */
    pa_log_debug("IO thread failed");
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("IO thread shutting down");
}

/* static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *msg, void *userdata) { */
/*     DBusMessageIter arg_i; */
/*     DBusError err; */
/*     const char *value; */
/*     struct userdata *u; */

/*     pa_assert(bus); */
/*     pa_assert(msg); */
/*     pa_assert(userdata); */
/*     u = userdata; */

/*     pa_log_debug("dbus: interface=%s, path=%s, member=%s\n", */
/*                  dbus_message_get_interface(msg), */
/*                  dbus_message_get_path(msg), */
/*                  dbus_message_get_member(msg)); */

/*     dbus_error_init(&err); */

/*    if (!dbus_message_has_path(msg, u->path)) */
/*        goto done; */

/*     if (dbus_message_is_signal(msg, "org.bluez.Headset", "PropertyChanged") || */
/*         dbus_message_is_signal(msg, "org.bluez.AudioSink", "PropertyChanged")) { */

/*         struct device *d; */
/*         const char *profile; */
/*         DBusMessageIter variant_i; */
/*         dbus_uint16_t gain; */

/*         if (!dbus_message_iter_init(msg, &arg_i)) { */
/*             pa_log("dbus: message has no parameters"); */
/*             goto done; */
/*         } */

/*         if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_STRING) { */
/*             pa_log("Property name not a string."); */
/*             goto done; */
/*         } */

/*         dbus_message_iter_get_basic(&arg_i, &value); */

/*         if (!dbus_message_iter_next(&arg_i)) { */
/*             pa_log("Property value missing"); */
/*             goto done; */
/*         } */

/*         if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_VARIANT) { */
/*             pa_log("Property value not a variant."); */
/*             goto done; */
/*         } */

/*         dbus_message_iter_recurse(&arg_i, &variant_i); */

/*         if (dbus_message_iter_get_arg_type(&variant_i) != DBUS_TYPE_UINT16) { */
/*             dbus_message_iter_get_basic(&variant_i, &gain); */

/*             if (pa_streq(value, "SpeakerGain")) { */
/*                 pa_log("spk gain: %d", gain); */
/*                 pa_cvolume_set(&u->sink->virtual_volume, 1, (pa_volume_t) (gain * PA_VOLUME_NORM / 15)); */
/*                 pa_subscription_post(u->sink->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, u->sink->index); */
/*             } else { */
/*                 pa_log("mic gain: %d", gain); */
/*                 if (!u->source) */
/*                     goto done; */

/*                 pa_cvolume_set(&u->source->virtual_volume, 1, (pa_volume_t) (gain * PA_VOLUME_NORM / 15)); */
/*                 pa_subscription_post(u->source->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, u->source->index); */
/*             } */
/*         } */
/*     } */

/* done: */
/*     dbus_error_free(&err); */
/*     return DBUS_HANDLER_RESULT_NOT_YET_HANDLED; */
/* } */

/* static int sink_get_volume_cb(pa_sink *s) { */
/*     struct userdata *u = s->userdata; */
/*     pa_assert(u); */

/*     /\* refresh? *\/ */

/*     return 0; */
/* } */

/* static int source_get_volume_cb(pa_source *s) { */
/*     struct userdata *u = s->userdata; */
/*     pa_assert(u); */

/*     /\* refresh? *\/ */

/*     return 0; */
/* } */

/* static int sink_set_volume_cb(pa_sink *s) { */
/*     DBusError e; */
/*     DBusMessage *m, *r; */
/*     DBusMessageIter it, itvar; */
/*     dbus_uint16_t vol; */
/*     const char *spkgain = "SpeakerGain"; */
/*     struct userdata *u = s->userdata; */
/*     pa_assert(u); */

/*     dbus_error_init(&e); */

/*     vol = ((float) pa_cvolume_max(&s->virtual_volume) / PA_VOLUME_NORM) * 15; */
/*     pa_log_debug("set headset volume: %d", vol); */

/*     pa_assert_se(m = dbus_message_new_method_call("org.bluez", u->path, "org.bluez.Headset", "SetProperty")); */
/*     dbus_message_iter_init_append(m, &it); */
/*     dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &spkgain); */
/*     dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_UINT16_AS_STRING, &itvar); */
/*     dbus_message_iter_append_basic(&itvar, DBUS_TYPE_UINT16, &vol); */
/*     dbus_message_iter_close_container(&it, &itvar); */

/*     r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->conn), m, -1, &e); */

/* finish: */
/*     if (m) */
/*         dbus_message_unref(m); */
/*     if (r) */
/*         dbus_message_unref(r); */

/*     dbus_error_free(&e); */

/*     return 0; */
/* } */

/* static int source_set_volume_cb(pa_source *s) { */
/*     dbus_uint16_t vol; */
/*     struct userdata *u = s->userdata; */
/*     pa_assert(u); */

/*     vol = ((float)pa_cvolume_max(&s->virtual_volume) / PA_VOLUME_NORM) * 15; */

/*     pa_log_debug("set headset mic volume: %d (not implemented yet)", vol); */

/*     return 0; */
/* } */

static char *get_name(const char *type, pa_modargs *ma, const char *device_id, pa_bool_t *namereg_fail) {
    char *t;
    const char *n;

    pa_assert(type);
    pa_assert(ma);
    pa_assert(device_id);
    pa_assert(namereg_fail);

    t = pa_sprintf_malloc("%s_name", type);
    n = pa_modargs_get_value(ma, t, NULL);
    pa_xfree(t);

    if (n) {
        *namereg_fail = TRUE;
        return pa_xstrdup(n);
    }

    if ((n = pa_modargs_get_value(ma, "name", NULL)))
        *namereg_fail = TRUE;
    else {
        n = device_id;
        *namereg_fail = FALSE;
    }

    return pa_sprintf_malloc("bluez_%s.%s", type, n);
}

#define USE_SCO_OVER_PCM(u) (u->profile == PROFILE_HSP && (u->hsp.sco_sink && u->hsp.sco_source))

static void sco_over_pcm_state_update(struct userdata *u) {
    pa_assert(u);
    pa_assert(USE_SCO_OVER_PCM(u));

    if (PA_SINK_IS_OPENED(pa_sink_get_state(u->hsp.sco_sink)) ||
        PA_SOURCE_IS_OPENED(pa_source_get_state(u->hsp.sco_source))) {

        if (u->service_fd >= 0)
            return;

        pa_log_debug("Resuming SCO over PCM");
        if ((init_bt(u) < 0) || (init_profile(u) < 0))
            pa_log("Can't resume SCO over PCM");

    } else {

        if (u->service_fd < 0)
            return;

        pa_log_debug("Closing SCO over PCM");
        pa_close(u->service_fd);
        u->service_fd = -1;
    }
}

static pa_hook_result_t sink_state_changed_cb(pa_core *c, pa_sink *s, struct userdata *u) {
    pa_assert(c);
    pa_sink_assert_ref(s);
    pa_assert(u);

    if (s != u->hsp.sco_sink)
        return PA_HOOK_OK;

    sco_over_pcm_state_update(u);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_state_changed_cb(pa_core *c, pa_source *s, struct userdata *u) {
    pa_assert(c);
    pa_source_assert_ref(s);
    pa_assert(u);

    if (s != u->hsp.sco_source)
        return PA_HOOK_OK;

    sco_over_pcm_state_update(u);

    return PA_HOOK_OK;
}

static int add_sink(struct userdata *u) {

    if (USE_SCO_OVER_PCM(u)) {
        pa_proplist *p;

        u->sink = u->hsp.sco_sink;
        p = pa_proplist_new();
        pa_proplist_sets(p, "bluetooth.protocol", "sco");
        pa_proplist_update(u->sink->proplist, PA_UPDATE_MERGE, p);
        pa_proplist_free(p);

        if (!u->hsp.sink_state_changed_slot)
            u->hsp.sink_state_changed_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_state_changed_cb, u);

    } else {
        pa_sink_new_data data;
        pa_bool_t b;

        pa_sink_new_data_init(&data);
        data.driver = __FILE__;
        data.module = u->module;
        pa_sink_new_data_set_sample_spec(&data, &u->sample_spec);
        pa_proplist_sets(data.proplist, "bluetooth.protocol", u->profile == PROFILE_A2DP ? "a2dp" : "sco");
        data.card = u->card;
        data.name = get_name("sink", u->modargs, u->device->address, &b);
        data.namereg_fail = b;

        u->sink = pa_sink_new(u->core, &data, PA_SINK_HARDWARE|PA_SINK_LATENCY);
        pa_sink_new_data_done(&data);

        if (!u->sink) {
            pa_log_error("Failed to create sink");
            return -1;
        }

        u->sink->userdata = u;
        u->sink->parent.process_msg = sink_process_msg;
    }

/*     u->sink->get_volume = sink_get_volume_cb; */
/*     u->sink->set_volume = sink_set_volume_cb; */

    return 0;
}

static int add_source(struct userdata *u) {
    pa_proplist *p;

    if (USE_SCO_OVER_PCM(u)) {
        u->source = u->hsp.sco_source;
        p = pa_proplist_new();
        pa_proplist_sets(p, "bluetooth.protocol", "sco");
        pa_proplist_update(u->source->proplist, PA_UPDATE_MERGE, p);
        pa_proplist_free(p);

        if (!u->hsp.source_state_changed_slot)
            u->hsp.source_state_changed_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) source_state_changed_cb, u);

    } else {
        pa_source_new_data data;
        pa_bool_t b;

        pa_source_new_data_init(&data);
        data.driver = __FILE__;
        data.module = u->module;
        pa_source_new_data_set_sample_spec(&data, &u->sample_spec);
        pa_proplist_sets(data.proplist, "bluetooth.protocol", u->profile == PROFILE_A2DP ? "a2dp" : "sco");
        data.card = u->card;
        data.name = get_name("source", u->modargs, u->device->address, &b);
        data.namereg_fail = b;

        u->source = pa_source_new(u->core, &data, PA_SOURCE_HARDWARE|PA_SOURCE_LATENCY);
        pa_source_new_data_done(&data);

        if (!u->source) {
            pa_log_error("Failed to create source");
            return -1;
        }

        u->source->userdata = u;
        u->source->parent.process_msg = source_process_msg;
    }

/*     u->source->get_volume = source_get_volume_cb; */
/*     u->source->set_volume = source_set_volume_cb; */

    p = pa_proplist_new();
    pa_proplist_sets(p, "bluetooth.nrec", pa_yes_no(u->hsp.pcm_capabilities.flags & BT_PCM_FLAG_NREC));
    pa_proplist_update(u->source->proplist, PA_UPDATE_MERGE, p);
    pa_proplist_free(p);

    return 0;
}

static void shutdown_bt(struct userdata *u) {
    pa_assert(u);

    if (u->stream_fd >= 0) {
        pa_close(u->stream_fd);
        u->stream_fd = -1;
    }

    if (u->service_fd >= 0) {
        pa_close(u->service_fd);
        u->service_fd = -1;
    }
}

static int init_bt(struct userdata *u) {
    pa_assert(u);

    shutdown_bt(u);

    u->stream_write_type = u->stream_read_type = 0;
    u->service_write_type = u->service_write_type = 0;

    if ((u->service_fd = bt_audio_service_open()) < 0) {
        pa_log_error("Couldn't connect to bluetooth audio service");
        return -1;
    }

    pa_log_debug("Connected to the bluetooth audio service");

    return 0;
}

static int setup_bt(struct userdata *u) {
    pa_assert(u);

    if (get_caps(u) < 0)
        return -1;

    pa_log_debug("Got device capabilities");

    if (set_conf(u) < 0)
        return -1;

    pa_log_debug("Connection to the device configured");

    if (USE_SCO_OVER_PCM(u)) {
        pa_log_debug("Configured to use SCO over PCM");
        return 0;
    }

    pa_log_debug("Got the stream socket");

    return 0;
}

static int init_profile(struct userdata *u) {
    int r = 0;
    pa_assert(u);
    pa_assert(u->profile != PROFILE_OFF);

    if (setup_bt(u) < 0)
        return -1;

    if (u->profile == PROFILE_A2DP ||
        u->profile == PROFILE_HSP)
        if (add_sink(u) < 0)
            r = -1;

    if (u->profile == PROFILE_HSP)
        if (add_source(u) < 0)
            r = -1;

    return r;
}

static void stop_thread(struct userdata *u) {
    pa_assert(u);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
        u->thread = NULL;
    }

    if (u->rtpoll_item) {
        pa_rtpoll_item_free(u->rtpoll_item);
        u->rtpoll_item = NULL;
    }

    if (u->hsp.sink_state_changed_slot) {
        pa_hook_slot_free(u->hsp.sink_state_changed_slot);
        u->hsp.sink_state_changed_slot = NULL;
    }

    if (u->hsp.source_state_changed_slot) {
        pa_hook_slot_free(u->hsp.source_state_changed_slot);
        u->hsp.source_state_changed_slot = NULL;
    }

    if (u->sink) {
        pa_sink_unref(u->sink);
        u->sink = NULL;
    }

    if (u->source) {
        pa_source_unref(u->source);
        u->source = NULL;
    }

    if (u->rtpoll) {
        pa_thread_mq_done(&u->thread_mq);

        pa_rtpoll_free(u->rtpoll);
        u->rtpoll = NULL;
    }
}

static int start_thread(struct userdata *u) {
    pa_assert(u);
    pa_assert(!u->thread);
    pa_assert(!u->rtpoll);
    pa_assert(!u->rtpoll_item);

    if (USE_SCO_OVER_PCM(u)) {
        pa_sink_ref(u->sink);
        pa_source_ref(u->source);
        return 0;
    }

    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, u->core->mainloop, u->rtpoll);

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log_error("Failed to create IO thread");
        stop_thread(u);
        return -1;
    }

    if (u->sink) {
        pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
        pa_sink_set_rtpoll(u->sink, u->rtpoll);
        pa_sink_put(u->sink);
    }

    if (u->source) {
        pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
        pa_source_set_rtpoll(u->source, u->rtpoll);
        pa_source_put(u->source);
    }

    return 0;
}

static int card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    struct userdata *u;
    enum profile *d;
    pa_queue *inputs = NULL, *outputs = NULL;

    pa_assert(c);
    pa_assert(new_profile);
    pa_assert_se(u = c->userdata);

    d = PA_CARD_PROFILE_DATA(new_profile);

    if (u->sink) {
        inputs = pa_sink_move_all_start(u->sink);
        if (!USE_SCO_OVER_PCM(u))
            pa_sink_unlink(u->sink);
    }

    if (u->source) {
        outputs = pa_source_move_all_start(u->source);
        if (!USE_SCO_OVER_PCM(u))
            pa_source_unlink(u->source);
    }

    stop_thread(u);
    shutdown_bt(u);

    if (u->write_memchunk.memblock) {
        pa_memblock_unref(u->write_memchunk.memblock);
        pa_memchunk_reset(&u->write_memchunk);
    }

    u->profile = *d;
    u->sample_spec = u->requested_sample_spec;

    init_bt(u);

    if (u->profile != PROFILE_OFF)
        init_profile(u);

    if (u->sink || u->source)
        start_thread(u);

    if (inputs) {
        if (u->sink)
            pa_sink_move_all_finish(u->sink, inputs, FALSE);
        else
            pa_sink_move_all_fail(inputs);
    }

    if (outputs) {
        if (u->source)
            pa_source_move_all_finish(u->source, outputs, FALSE);
        else
            pa_source_move_all_fail(outputs);
    }

    return 0;
}

static int add_card(struct userdata *u, const char * default_profile) {
    pa_card_new_data data;
    pa_bool_t b;
    pa_card_profile *p;
    enum profile *d;
    const char *ff;
    char *n;

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;

    n = pa_bluetooth_cleanup_name(u->device->name);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, n);
    pa_xfree(n);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->device->address);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "bluez");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_BUS, "bluetooth");
    if ((ff = pa_bluetooth_get_form_factor(u->device->class)))
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_FORM_FACTOR, ff);
    pa_proplist_sets(data.proplist, "bluez.path", u->device->path);
    pa_proplist_setf(data.proplist, "bluez.class", "0x%06x", (unsigned) u->device->class);
    pa_proplist_sets(data.proplist, "bluez.name", u->device->name);
    data.name = get_name("card", u->modargs, u->device->address, &b);
    data.namereg_fail = b;

    data.profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    if (u->device->audio_sink_info_valid > 0) {
        p = pa_card_profile_new("a2dp", _("High Fidelity Playback (A2DP)"), sizeof(enum profile));
        p->priority = 10;
        p->n_sinks = 1;
        p->n_sources = 0;
        p->max_sink_channels = 2;
        p->max_source_channels = 0;

        d = PA_CARD_PROFILE_DATA(p);
        *d = PROFILE_A2DP;

        pa_hashmap_put(data.profiles, p->name, p);
    }

    if (u->device->headset_info_valid > 0) {
        p = pa_card_profile_new("hsp", _("Telephony Duplex (HSP/HFP)"), sizeof(enum profile));
        p->priority = 20;
        p->n_sinks = 1;
        p->n_sources = 1;
        p->max_sink_channels = 1;
        p->max_source_channels = 1;

        d = PA_CARD_PROFILE_DATA(p);
        *d = PROFILE_HSP;

        pa_hashmap_put(data.profiles, p->name, p);
    }

    pa_assert(!pa_hashmap_isempty(data.profiles));

    p = pa_card_profile_new("off", _("Off"), sizeof(enum profile));
    d = PA_CARD_PROFILE_DATA(p);
    *d = PROFILE_OFF;
    pa_hashmap_put(data.profiles, p->name, p);

    if (default_profile) {
        if (pa_hashmap_get(data.profiles, default_profile))
            pa_card_new_data_set_profile(&data, default_profile);
        else
            pa_log_warn("Profile '%s' not valid or not supported by device.", default_profile);
    }

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log("Failed to allocate card.");
        return -1;
    }

    u->card->userdata = u;
    u->card->set_profile = card_set_profile;

    d = PA_CARD_PROFILE_DATA(u->card->active_profile);
    u->profile = *d;

    return 0;
}

static int setup_dbus(struct userdata *u) {
    DBusError error;

    dbus_error_init(&error);

    u->connection = pa_dbus_bus_get(u->core, DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error) || (!u->connection)) {
        pa_log("Failed to get D-Bus connection: %s", error.message);
        dbus_error_free(&error);
        return -1;
    }

    return 0;
}

static int find_device(struct userdata *u, const char *address, const char *path) {
    pa_assert(u);

    if (!address && !path) {
        pa_log_error("Failed to get device address/path from module arguments.");
        return -1;
    }

    if (path) {
        if (!(u->device = pa_bluetooth_get_device(pa_dbus_connection_get(u->connection), path))) {
            pa_log_error("%s is not a valid BlueZ audio device.", path);
            return -1;
        }

        if (address && !(pa_streq(u->device->address, address))) {
            pa_log_error("Passed path %s and address %s don't match.", path, address);
            return -1;
        }
    } else {
        if (!(u->device = pa_bluetooth_find_device(pa_dbus_connection_get(u->connection), address))) {
            pa_log_error("%s is not known.", address);
            return -1;
        }
    }

    return 0;
}

int pa__init(pa_module* m) {
    pa_modargs *ma;
    uint32_t channels;
    struct userdata *u;
    const char *address, *path;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;
    u->service_fd = -1;
    u->stream_fd = -1;
    u->read_smoother = pa_smoother_new(PA_USEC_PER_SEC, PA_USEC_PER_SEC*2, TRUE, 10);
    u->sample_spec = m->core->default_sample_spec;
    u->modargs = ma;

    if (pa_modargs_get_value(ma, "sco_sink", NULL) &&
        !(u->hsp.sco_sink = pa_namereg_get(m->core, pa_modargs_get_value(ma, "sco_sink", NULL), PA_NAMEREG_SINK))) {
        pa_log("SCO sink not found");
        goto fail;
    }

    if (pa_modargs_get_value(ma, "sco_source", NULL) &&
        !(u->hsp.sco_source = pa_namereg_get(m->core, pa_modargs_get_value(ma, "sco_source", NULL), PA_NAMEREG_SOURCE))) {
        pa_log("SCO source not found");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "rate", &u->sample_spec.rate) < 0 ||
        u->sample_spec.rate <= 0 || u->sample_spec.rate > PA_RATE_MAX) {
        pa_log_error("Failed to get rate from module arguments");
        goto fail;
    }

    channels = u->sample_spec.channels;
    if (pa_modargs_get_value_u32(ma, "channels", &channels) < 0 ||
        channels <= 0 || channels > PA_CHANNELS_MAX) {
        pa_log_error("Failed to get channels from module arguments");
        goto fail;
    }
    u->sample_spec.channels = (uint8_t) channels;
    u->requested_sample_spec = u->sample_spec;

    if (setup_dbus(u) < 0)
        goto fail;

    address = pa_modargs_get_value(ma, "address", NULL);
    path = pa_modargs_get_value(ma, "path", NULL);

    if (find_device(u, address, path) < 0)
        goto fail;

    pa_assert(u->device);

    /* Add the card structure. This will also initialize the default profile */
    if (add_card(u, pa_modargs_get_value(ma, "profile", NULL)) < 0)
        goto fail;

    /* Connect to the BT service and query capabilities */
    if (init_bt(u) < 0)
        goto fail;

    if (u->profile != PROFILE_OFF)
        if (init_profile(u) < 0)
            goto fail;

/*     if (u->path) { */
/*         DBusError err; */
/*         dbus_error_init(&err); */
/*         char *t; */


/*         if (!dbus_connection_add_filter(pa_dbus_connection_get(u->conn), filter_cb, u, NULL)) { */
/*             pa_log_error("Failed to add filter function"); */
/*             goto fail; */
/*         } */

/*         if (u->transport == BT_CAPABILITIES_TRANSPORT_SCO || */
/*             u->transport == BT_CAPABILITIES_TRANSPORT_ANY) { */
/*             t = pa_sprintf_malloc("type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged',path='%s'", u->path); */
/*             dbus_bus_add_match(pa_dbus_connection_get(u->conn), t, &err); */
/*             pa_xfree(t); */

/*             if (dbus_error_is_set(&err)) { */
/*                 pa_log_error("Unable to subscribe to org.bluez.Headset signals: %s: %s", err.name, err.message); */
/*                 goto fail; */
/*             } */
/*         } */

/*         if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP || */
/*             u->transport == BT_CAPABILITIES_TRANSPORT_ANY) { */
/*             t = pa_sprintf_malloc("type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged',path='%s'", u->path); */
/*             dbus_bus_add_match(pa_dbus_connection_get(u->conn), t, &err); */
/*             pa_xfree(t); */

/*             if (dbus_error_is_set(&err)) { */
/*                 pa_log_error("Unable to subscribe to org.bluez.AudioSink signals: %s: %s", err.name, err.message); */
/*                 goto fail; */
/*             } */
/*         } */
/*     } */

    if (u->sink || u->source)
        if (start_thread(u) < 0)
            goto fail;

    return 0;

fail:
    pa__done(m);
    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return
        (u->sink ? pa_sink_linked_by(u->sink) : 0) +
        (u->source ? pa_source_linked_by(u->source) : 0);
}

void pa__done(pa_module *m) {
    struct userdata *u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink && !USE_SCO_OVER_PCM(u))
        pa_sink_unlink(u->sink);

    if (u->source && !USE_SCO_OVER_PCM(u))
        pa_source_unlink(u->source);

    stop_thread(u);

    if (u->connection) {
/*         DBusError error; */
/*         char *t; */

/*         if (u->transport == BT_CAPABILITIES_TRANSPORT_SCO || */
/*             u->transport == BT_CAPABILITIES_TRANSPORT_ANY) { */

/*             t = pa_sprintf_malloc("type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged',path='%s'", u->path); */
/*             dbus_error_init(&error); */
/*             dbus_bus_remove_match(pa_dbus_connection_get(u->conn), t, &error); */
/*             dbus_error_free(&error); */
/*             pa_xfree(t); */
/*         } */

/*         if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP || */
/*             u->transport == BT_CAPABILITIES_TRANSPORT_ANY) { */

/*             t = pa_sprintf_malloc("type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged',path='%s'", u->path); */
/*             dbus_error_init(&error); */
/*             dbus_bus_remove_match(pa_dbus_connection_get(u->conn), t, &error); */
/*             dbus_error_free(&error); */
/*             pa_xfree(t); */
/*         } */

/*         dbus_connection_remove_filter(pa_dbus_connection_get(u->conn), filter_cb, u); */
        pa_dbus_connection_unref(u->connection);
    }

    if (u->card)
        pa_card_free(u->card);

    if (u->read_smoother)
        pa_smoother_free(u->read_smoother);

    shutdown_bt(u);

    if (u->device)
        pa_bluetooth_device_free(u->device);

    if (u->write_memchunk.memblock)
        pa_memblock_unref(u->write_memchunk.memblock);

    if (u->a2dp.buffer)
        pa_xfree(u->a2dp.buffer);

    sbc_finish(&u->a2dp.sbc);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    pa_xfree(u);
}
