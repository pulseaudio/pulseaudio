/***
    This file is part of PulseAudio.

    Copyright 2008 Joao Paulo Rechi Vita

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, write to the Free Software
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

#include "../dbus-util.h"
#include "module-bluetooth-device-symdef.h"
#include "ipc.h"
#include "sbc.h"
#include "rtp.h"

#define DEFAULT_SINK_NAME "bluetooth_sink"
#define BUFFER_SIZE 2048
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
        "sink_name=<name of the device> "
        "address=<address of the device> "
        "profile=<a2dp|hsp>");

struct bt_a2dp {
    sbc_capabilities_t sbc_capabilities;
    sbc_t sbc;                           /* Codec data */
    pa_bool_t sbc_initialized;           /* Keep track if the encoder is initialized */
    size_t codesize;                     /* SBC codesize */
    unsigned samples;                    /* Number of encoded samples */
    uint8_t buffer[BUFFER_SIZE];         /* Codec transfer buffer */
    size_t count;                        /* Codec transfer buffer counter */

    unsigned total_samples;              /* Cumulative number of codec samples */
    uint16_t seq_num;                    /* Cumulative packet sequence */
    unsigned frame_count;                /* Current frames in buffer*/
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    pa_thread *thread;

    uint64_t offset;
    pa_smoother *smoother;

    char *name;
    char *addr;
    char *profile;
    pa_sample_spec ss;

    int audioservice_fd;
    int stream_fd;

    uint8_t transport;
    char *strtransport;
    size_t link_mtu;
    size_t block_size;
    pa_usec_t latency;

    struct bt_a2dp a2dp;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "address",
    "profile",
    "rate",
    "channels",
    NULL
};

static int bt_audioservice_send(int sk, const bt_audio_msg_header_t *msg) {
    int e;
    pa_log_debug("sending %s", bt_audio_strmsg(msg->msg_type));
    if (send(sk, msg, BT_AUDIO_IPC_PACKET_SIZE, 0) > 0)
        e = 0;
    else {
        e = -errno;
        pa_log_error("Error sending data to audio service: %s(%d)", pa_cstrerror(errno), errno);
    }
    return e;
}

static int bt_audioservice_recv(int sk, bt_audio_msg_header_t *inmsg) {
    int e;
    const char *type;

    pa_log_debug("trying to receive msg from audio service...");
    if (recv(sk, inmsg, BT_AUDIO_IPC_PACKET_SIZE, 0) > 0) {
        type = bt_audio_strmsg(inmsg->msg_type);
        if (type) {
            pa_log_debug("Received %s", type);
            e = 0;
        }
        else {
            e = -EINVAL;
            pa_log_error("Bogus message type %d received from audio service", inmsg->msg_type);
        }
    }
    else {
        e = -errno;
        pa_log_error("Error receiving data from audio service: %s(%d)", pa_cstrerror(errno), errno);
    }

    return e;
}

static int bt_audioservice_expect(int sk, bt_audio_msg_header_t *rsp_hdr, int expected_type) {
    int e = bt_audioservice_recv(sk, rsp_hdr);
    if (e == 0) {
        if (rsp_hdr->msg_type != expected_type) {
            e = -EINVAL;
            pa_log_error("Bogus message %s received while %s was expected", bt_audio_strmsg(rsp_hdr->msg_type),
                    bt_audio_strmsg(expected_type));
        }
    }
    return e;
}

static int bt_getcaps(struct userdata *u) {
    int e;
    union {
        bt_audio_rsp_msg_header_t rsp_hdr;
        struct bt_getcapabilities_req getcaps_req;
        struct bt_getcapabilities_rsp getcaps_rsp;
        uint8_t buf[BT_AUDIO_IPC_PACKET_SIZE];
    } msg;

    memset(msg.buf, 0, BT_AUDIO_IPC_PACKET_SIZE);
    msg.getcaps_req.h.msg_type = BT_GETCAPABILITIES_REQ;
    strncpy(msg.getcaps_req.device, u->addr, 18);
    if (strcasecmp(u->profile, "a2dp") == 0)
        msg.getcaps_req.transport = BT_CAPABILITIES_TRANSPORT_A2DP;
    else if (strcasecmp(u->profile, "hsp") == 0)
        msg.getcaps_req.transport = BT_CAPABILITIES_TRANSPORT_SCO;
    else {
        pa_log_error("Invalid profile argument: %s", u->profile);
        return -1;
    }
    msg.getcaps_req.flags = BT_FLAG_AUTOCONNECT;

    e = bt_audioservice_send(u->audioservice_fd, &msg.getcaps_req.h);
    if (e < 0) {
        pa_log_error("Failed to send GETCAPABILITIES_REQ");
        return e;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &msg.rsp_hdr.msg_h, BT_GETCAPABILITIES_RSP);
    if (e < 0) {
        pa_log_error("Failed to expect for GETCAPABILITIES_RSP");
        return e;
    }
    if (msg.rsp_hdr.posix_errno != 0) {
        pa_log_error("BT_GETCAPABILITIES failed : %s (%d)", pa_cstrerror(msg.rsp_hdr.posix_errno), msg.rsp_hdr.posix_errno);
        return -msg.rsp_hdr.posix_errno;
    }

    if ((u->transport = msg.getcaps_rsp.transport) == BT_CAPABILITIES_TRANSPORT_A2DP)
        u->a2dp.sbc_capabilities = msg.getcaps_rsp.sbc_capabilities;

    return 0;
}

static uint8_t default_bitpool(uint8_t freq, uint8_t mode) {
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

static int bt_a2dp_init(struct userdata *u) {
    sbc_capabilities_t *cap = &u->a2dp.sbc_capabilities;
    uint8_t max_bitpool, min_bitpool;
    unsigned i;

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } freq_table[] = {
        { 16000U, BT_SBC_SAMPLING_FREQ_16000 },
        { 32000U, BT_SBC_SAMPLING_FREQ_32000 },
        { 44100U, BT_SBC_SAMPLING_FREQ_44100 },
        { 48000U, BT_SBC_SAMPLING_FREQ_48000 }
    };

    /* Find the lowest freq that is at least as high as the requested
     * sampling rate */
    for (i = 0; i < PA_ELEMENTSOF(freq_table); i++)
        if (freq_table[i].rate >= u->ss.rate || i == PA_ELEMENTSOF(freq_table)-1 ) {
            u->ss.rate = freq_table[i].rate;
            cap->frequency = freq_table[i].cap;
            break;
        }

    if (u->ss.channels >= 2) {
        if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_JOINT_STEREO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
        else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_STEREO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_STEREO;
        else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL;

        u->ss.channels = 2;
    } else {
        if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_MONO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_MONO;
    }

    if (!cap->channel_mode) {
        pa_log_error("No supported channel modes");
        return -1;
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

    min_bitpool = (uint8_t) PA_MAX(MIN_BITPOOL, cap->min_bitpool);
    max_bitpool = (uint8_t) PA_MIN(default_bitpool(cap->frequency, cap->channel_mode), cap->max_bitpool);

    cap->min_bitpool = (uint8_t) min_bitpool;
    cap->max_bitpool = (uint8_t) max_bitpool;

    return 0;
}

static void bt_a2dp_setup(struct bt_a2dp *a2dp) {
    sbc_capabilities_t active_capabilities = a2dp->sbc_capabilities;

    if (a2dp->sbc_initialized)
        sbc_reinit(&a2dp->sbc, 0);
    else
        sbc_init(&a2dp->sbc, 0);
    a2dp->sbc_initialized = TRUE;

    if (active_capabilities.frequency & BT_SBC_SAMPLING_FREQ_16000)
        a2dp->sbc.frequency = SBC_FREQ_16000;

    if (active_capabilities.frequency & BT_SBC_SAMPLING_FREQ_32000)
        a2dp->sbc.frequency = SBC_FREQ_32000;

    if (active_capabilities.frequency & BT_SBC_SAMPLING_FREQ_44100)
        a2dp->sbc.frequency = SBC_FREQ_44100;

    if (active_capabilities.frequency & BT_SBC_SAMPLING_FREQ_48000)
        a2dp->sbc.frequency = SBC_FREQ_48000;

    if (active_capabilities.channel_mode & BT_A2DP_CHANNEL_MODE_MONO)
        a2dp->sbc.mode = SBC_MODE_MONO;

    if (active_capabilities.channel_mode & BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL)
        a2dp->sbc.mode = SBC_MODE_DUAL_CHANNEL;

    if (active_capabilities.channel_mode & BT_A2DP_CHANNEL_MODE_STEREO)
        a2dp->sbc.mode = SBC_MODE_STEREO;

    if (active_capabilities.channel_mode & BT_A2DP_CHANNEL_MODE_JOINT_STEREO)
        a2dp->sbc.mode = SBC_MODE_JOINT_STEREO;

    a2dp->sbc.allocation = (uint8_t) (active_capabilities.allocation_method == BT_A2DP_ALLOCATION_SNR ? SBC_AM_SNR : SBC_AM_LOUDNESS);

    switch (active_capabilities.subbands) {
        case BT_A2DP_SUBBANDS_4:
            a2dp->sbc.subbands = SBC_SB_4;
            break;
        case BT_A2DP_SUBBANDS_8:
            a2dp->sbc.subbands = SBC_SB_8;
            break;
    }

    switch (active_capabilities.block_length) {
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
    }

    a2dp->sbc.bitpool = active_capabilities.max_bitpool;
    a2dp->codesize = (uint16_t) sbc_get_codesize(&a2dp->sbc);
    a2dp->count = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
}

static int bt_setconf(struct userdata *u) {
    int e;
    union {
        bt_audio_rsp_msg_header_t rsp_hdr;
        struct bt_setconfiguration_req setconf_req;
        struct bt_setconfiguration_rsp setconf_rsp;
        uint8_t buf[BT_AUDIO_IPC_PACKET_SIZE];
    } msg;

    if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP) {
        e = bt_a2dp_init(u);
        if (e < 0) {
            pa_log_error("a2dp_init error");
            return e;
        }
        u->ss.format = PA_SAMPLE_S16LE;
    }
    else
        u->ss.format = PA_SAMPLE_U8;

    memset(msg.buf, 0, BT_AUDIO_IPC_PACKET_SIZE);
    msg.setconf_req.h.msg_type = BT_SETCONFIGURATION_REQ;
    strncpy(msg.setconf_req.device, u->addr, 18);
    msg.setconf_req.transport = u->transport;
    if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP)
        msg.setconf_req.sbc_capabilities = u->a2dp.sbc_capabilities;
    msg.setconf_req.access_mode = BT_CAPABILITIES_ACCESS_MODE_WRITE;

    e = bt_audioservice_send(u->audioservice_fd, &msg.setconf_req.h);
    if (e < 0) {
        pa_log_error("Failed to send BT_SETCONFIGURATION_REQ");
        return e;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &msg.rsp_hdr.msg_h, BT_SETCONFIGURATION_RSP);
    if (e < 0) {
        pa_log_error("Failed to expect BT_SETCONFIGURATION_RSP");
        return e;
    }

    if (msg.rsp_hdr.posix_errno != 0) {
        pa_log_error("BT_SETCONFIGURATION failed : %s(%d)", pa_cstrerror(msg.rsp_hdr.posix_errno), msg.rsp_hdr.posix_errno);
        return -msg.rsp_hdr.posix_errno;
    }

    u->transport = msg.setconf_rsp.transport;
    u->strtransport = (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP ? pa_xstrdup("A2DP") : pa_xstrdup("SCO"));
    u->link_mtu = msg.setconf_rsp.link_mtu;

    /* setup SBC encoder now we agree on parameters */
    if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP) {
        bt_a2dp_setup(&u->a2dp);
        u->block_size = u->a2dp.codesize;
        pa_log_info("sbc parameters:\n\tallocation=%u\n\tsubbands=%u\n\tblocks=%u\n\tbitpool=%u\n",
                u->a2dp.sbc.allocation, u->a2dp.sbc.subbands, u->a2dp.sbc.blocks, u->a2dp.sbc.bitpool);
    }
    else
        u->block_size = u->link_mtu;

    return 0;
}

static int bt_getstreamfd(struct userdata *u) {
    int e;
//    uint32_t period_count = io->buffer_size / io->period_size;
    union {
        bt_audio_rsp_msg_header_t rsp_hdr;
        struct bt_streamstart_req start_req;
        struct bt_streamfd_ind streamfd_ind;
        uint8_t buf[BT_AUDIO_IPC_PACKET_SIZE];
    } msg;

    memset(msg.buf, 0, BT_AUDIO_IPC_PACKET_SIZE);
    msg.start_req.h.msg_type = BT_STREAMSTART_REQ;

    e = bt_audioservice_send(u->audioservice_fd, &msg.start_req.h);
    if (e < 0) {
        pa_log_error("Failed to send BT_STREAMSTART_REQ");
        return e;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &msg.rsp_hdr.msg_h, BT_STREAMSTART_RSP);
    if (e < 0) {
        pa_log_error("Failed to expect BT_STREAMSTART_RSP");
        return e;
    }

    if (msg.rsp_hdr.posix_errno != 0) {
        pa_log_error("BT_START failed : %s(%d)", pa_cstrerror(msg.rsp_hdr.posix_errno), msg.rsp_hdr.posix_errno);
        return -msg.rsp_hdr.posix_errno;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &msg.streamfd_ind.h, BT_STREAMFD_IND);
    if (e < 0) {
        pa_log_error("Failed to expect BT_STREAMFD_IND");
        return e;
    }

    if (u->stream_fd >= 0)
        pa_close(u->stream_fd);

    u->stream_fd = bt_audio_service_get_data_fd(u->audioservice_fd);
    if (u->stream_fd < 0) {
        pa_log_error("Failed to get data fd: %s (%d)",pa_cstrerror(errno), errno);
        return -errno;
    }

//   if (setsockopt(u->stream_fd, SOL_SCO, SCO_TXBUFS, &period_count, sizeof(period_count)) == 0)
//       return 0;
//   if (setsockopt(u->stream_fd, SOL_SCO, SO_SNDBUF, &period_count, sizeof(period_count)) == 0)
//       return 0;
//   /* FIXME : handle error codes */
    pa_make_fd_nonblock(u->stream_fd);
//    pa_make_socket_low_delay(u->stream_fd);

    return 0;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    pa_log_debug("got message: %d", code);
    switch (code) {

        case PA_SINK_MESSAGE_SET_STATE:
            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {
                case PA_SINK_SUSPENDED:
                    pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));
                    pa_smoother_pause(u->smoother, pa_rtclock_usec());
                    break;
                case PA_SINK_IDLE:
                case PA_SINK_RUNNING:
                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED)
                        pa_smoother_resume(u->smoother, pa_rtclock_usec());
                    break;
                case PA_SINK_UNLINKED:
                case PA_SINK_INIT:
                    ;
            }
            break;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t w, r;
/*             r = pa_smoother_get(u->smoother, pa_rtclock_usec()); */
/* /\*             w = pa_bytes_to_usec(u->offset + (uint64_t) u->memchunk.length, &u->sink->sample_spec); *\/ */
            *((pa_usec_t*) data) = /*w > r ? w - r :*/ 0;
            return 0;
        }

    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int sco_process_render(struct userdata *u) {
    void *p;
    int ret = 0;
    pa_memchunk memchunk;

    pa_sink_render_full(u->sink, u->block_size, &memchunk);

    p = pa_memblock_acquire(memchunk.memblock);

    for (;;) {
        ssize_t l;

        l = pa_loop_write(u->stream_fd, (uint8_t*) p, memchunk.length, NULL);
        pa_log_debug("Memblock written to socket: %li bytes", (long) l);

        pa_assert(l != 0);

        if (l > 0) {
            u->offset += (uint64_t) l;
            break;
        }

        if (errno == EINTR)
            pa_log_debug("EINTR");
        else if (errno == EAGAIN)
            pa_log_debug("EAGAIN");
        else {
            pa_log_error("Failed to write data to FIFO: %s", pa_cstrerror(errno));
            ret = -1;
            break;
        }
    }

    pa_memblock_release(memchunk.memblock);
    pa_memblock_unref(memchunk.memblock);

    return ret;
}

static int a2dp_process_render(struct userdata *u) {
    int written;

    struct bt_a2dp *a2dp = &u->a2dp;
    struct rtp_header *header = (void *) a2dp->buffer;
    struct rtp_payload *payload = (void *) (a2dp->buffer + sizeof(*header));

    pa_assert(u);

    do {
        /* Render some data */
        int frame_size, encoded;
        void *p;
        pa_memchunk memchunk;

        pa_sink_render_full(u->sink, u->block_size, &memchunk);

        p = pa_memblock_acquire(memchunk.memblock);

        frame_size = (uint16_t) sbc_get_frame_length(&a2dp->sbc);
        pa_log_debug("SBC frame_size: %d", frame_size);

        encoded = sbc_encode(&a2dp->sbc, p, (int) a2dp->codesize, a2dp->buffer + a2dp->count,
                             (int) (sizeof(a2dp->buffer) - a2dp->count), &written);
        pa_log_debug("SBC: encoded: %d; written: %d", encoded, written);

        pa_memblock_release(memchunk.memblock);
        pa_memblock_unref(memchunk.memblock);

        if (encoded <= 0) {
            pa_log_error("SBC encoding error (%d)", encoded);
            return -1;
        }

        a2dp->count += (size_t) written;
        a2dp->frame_count++;
        a2dp->samples += (unsigned) encoded / frame_size;
        a2dp->total_samples += (unsigned) encoded / frame_size;

    } while (a2dp->count + (size_t) written <= u->link_mtu);

    /* write it to the fifo */
    memset(a2dp->buffer, 0, sizeof(*header) + sizeof(*payload));
    payload->frame_count = a2dp->frame_count;
    header->v = 2;
    header->pt = 1;
    header->sequence_number = htons(a2dp->seq_num);
    header->timestamp = htonl(a2dp->total_samples);
    header->ssrc = htonl(1);

    for (;;) {
        ssize_t l;

        l = pa_loop_write(u->stream_fd, a2dp->buffer, a2dp->count, NULL);
        pa_log_debug("avdtp_write: requested %lu bytes; written %li bytes", (unsigned long) a2dp->count, (long) l);

        pa_assert(l != 0);

        if (l > 0)
            break;

        if (errno == EINTR)
            pa_log_debug("EINTR");
        else if (errno == EAGAIN)
            pa_log_debug("EAGAIN");
        else {
            pa_log_error("Failed to write data to FIFO: %s", pa_cstrerror(errno));
            return -1;
        }
    }

    u->offset += a2dp->codesize*a2dp->frame_count;

    /* Reset buffer of data to send */
    a2dp->count = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
    a2dp->frame_count = 0;
    a2dp->samples = 0;
    a2dp->seq_num++;

    return 0;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("IO Thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);
    pa_rtpoll_install(u->rtpoll);

    pa_smoother_set_time_offset(u->smoother, pa_rtclock_usec());

    for (;;) {
        int ret, l;
        struct pollfd *pollfd;
        uint64_t n;
        pa_usec_t usec;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            if (u->sink->thread_info.rewind_requested)
                pa_sink_process_rewind(u->sink, 0);

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state) && pollfd->revents) {
            if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP) {
                if ((l = a2dp_process_render(u)) < 0)
                    goto fail;
            } else {
                if ((l = sco_process_render(u)) < 0)
                    goto fail;
            }
            pollfd->revents = 0;

            /* feed the time smoother */
            n = u->offset;
            if (ioctl(u->stream_fd, SIOCOUTQ, &l) >= 0 && l > 0)
                n -= (uint64_t) l;
            usec = pa_bytes_to_usec(n, &u->sink->sample_spec);
            if (usec > u->latency)
                usec -= u->latency;
            else
                usec = 0;
            pa_smoother_put(u->smoother, pa_rtclock_usec(), usec);
        }

        /* Hmm, nothing to do. Let's sleep */
        pa_log_debug("IO thread going to sleep");
        pollfd->events = (short) (PA_SINK_IS_OPENED(u->sink->thread_info.state) ? POLLOUT : 0);
        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0) {
            pa_log_error("rtpoll_run < 0");
            goto fail;
        }
        pa_log_debug("IO thread waking up");

        if (ret == 0) {
            pa_log_debug("rtpoll_run == 0");
            goto finish;
        }

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
        if (pollfd->revents & ~POLLOUT) {
            pa_log_error("FIFO shutdown.");
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

int pa__init(pa_module* m) {
    int e;
    pa_modargs *ma;
    uint32_t channels;
    pa_sink_new_data data;
    struct pollfd *pollfd;
    struct userdata *u;

    pa_assert(m);
    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;
    u->audioservice_fd = -1;
    u->stream_fd = -1;
    u->transport = (uint8_t) -1;
    u->offset = 0;
    u->latency = 0;
    u->a2dp.sbc_initialized = FALSE;
    u->smoother = pa_smoother_new(PA_USEC_PER_SEC, PA_USEC_PER_SEC*2, TRUE, 10);
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, u->core->mainloop, u->rtpoll);
    u->rtpoll_item = NULL;
    u->ss = m->core->default_sample_spec;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        goto fail;
    }
    if (!(u->name = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME)))) {
        pa_log_error("Failed to get device name from module arguments");
        goto fail;
    }
    if (!(u->addr = pa_xstrdup(pa_modargs_get_value(ma, "address", NULL)))) {
        pa_log_error("Failed to get device address from module arguments");
        goto fail;
    }
    if (!(u->profile = pa_xstrdup(pa_modargs_get_value(ma, "profile", NULL)))) {
        pa_log_error("Failed to get profile from module arguments");
        goto fail;
    }
    if (pa_modargs_get_value_u32(ma, "rate", &u->ss.rate) < 0) {
        pa_log_error("Failed to get rate from module arguments");
        goto fail;
    }

    channels = u->ss.channels;
    if (pa_modargs_get_value_u32(ma, "channels", &channels) < 0) {
        pa_log_error("Failed to get channels from module arguments");
        goto fail;
    }
    u->ss.channels = (uint8_t) channels;

    /* connect to the bluez audio service */
    u->audioservice_fd = bt_audio_service_open();
    if (u->audioservice_fd <= 0) {
        pa_log_error("Couldn't connect to bluetooth audio service");
        goto fail;
    }
    pa_log_debug("Connected to the bluetooth audio service");

    /* queries device capabilities */
    e = bt_getcaps(u);
    if (e < 0) {
        pa_log_error("Failed to get device capabilities");
        goto fail;
    }
    pa_log_debug("Got device capabilities");

    /* configures the connection */
    e = bt_setconf(u);
    if (e < 0) {
        pa_log_error("Failed to set config");
        goto fail;
    }
    pa_log_debug("Connection to the device configured");

    /* gets the device socket */
    e = bt_getstreamfd(u);
    if (e < 0) {
        pa_log_error("Failed to get stream fd (%d)", e);
        goto fail;
    }
    pa_log_debug("Got the device socket");

    /* create sink */
    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, u->name);
    pa_sink_new_data_set_sample_spec(&data, &u->ss);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->name);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Bluetooth %s '%s' (%s)", u->strtransport, u->name, u->addr);
    pa_proplist_sets(data.proplist, "bluetooth.protocol", u->profile);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_API, "bluez");
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_CONNECTOR, "bluetooth");
/*     pa_proplist_setf(data.proplist, PA_PROP_DEVICE_FORM_FACTOR, "headset"); /\*FIXME*\/ */
/*     pa_proplist_setf(data.proplist, PA_PROP_DEVICE_VENDOR_PRODUCT_ID, "product_id"); /\*FIXME*\/ */
/*     pa_proplist_setf(data.proplist, PA_PROP_DEVICE_SERIAL, "serial"); /\*FIXME*\/ */
    u->sink = pa_sink_new(m->core, &data, PA_SINK_HARDWARE|PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);
    if (!u->sink) {
        pa_log_error("Failed to create sink");
        goto fail;
    }
    u->sink->userdata = u;
    u->sink->parent.process_msg = sink_process_msg;
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    pollfd->fd = u->stream_fd;
    pollfd->events = pollfd->revents = 0;

    /* start rt thread */
    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log_error("Failed to create IO thread");
        goto fail;
    }
    pa_sink_put(u->sink);

    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    if (u->sink)
        pa_sink_unref(u->sink);

    pa_thread_mq_done(&u->thread_mq);

    if (u->rtpoll_item)
        pa_rtpoll_item_free(u->rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->smoother)
        pa_smoother_free(u->smoother);

    pa_xfree(u->name);
    pa_xfree(u->addr);
    pa_xfree(u->profile);
    pa_xfree(u->strtransport);

    if (u->stream_fd >= 0)
        pa_close(u->stream_fd);

    if (u->audioservice_fd >= 0)
        pa_close(u->audioservice_fd);

    pa_xfree(u);
}
