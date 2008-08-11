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

#include <pulse/xmalloc.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/time-smoother.h>
#include <pulsecore/rtclock.h>

#include "dbus-util.h"
#include "module-bt-device-symdef.h"
#include "bt-ipc.h"
#include "bt-sbc.h"

#define DEFAULT_SINK_NAME "bt_sink"
#define BUFFER_SIZE 2048
#define MAX_BITPOOL 64
#define MIN_BITPOOL 2

PA_MODULE_AUTHOR("Joao Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Bluetooth audio sink and source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "name=<name of the device> "
        "addr=<address of the device> "
        "profile=<a2dp|hsp>");

struct bt_a2dp {
    sbc_capabilities_t sbc_capabilities;
    sbc_t sbc;                           /* Codec data */
    int sbc_initialized;                 /* Keep track if the encoder is initialized */
    int codesize;                        /* SBC codesize */
    int samples;                         /* Number of encoded samples */
    uint8_t buffer[BUFFER_SIZE];         /* Codec transfer buffer */
    int count;                           /* Codec transfer buffer counter */

    int nsamples;                        /* Cumulative number of codec samples */
    uint16_t seq_num;                    /* Cumulative packet sequence */
    int frame_count;                     /* Current frames in buffer*/
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    pa_thread *thread;

    pa_sample_spec ss;
    pa_smoother *smoother;

    pa_memchunk memchunk;

    const char *name;
    const char *addr;
    const char *profile;
    int rate;
    int channels;

    int audioservice_fd;
    int stream_fd;
    int transport;
    int link_mtu;

    struct bt_a2dp a2dp;
};

static const char* const valid_modargs[] = {
    "name",
    "addr",
    "profile",
    "rate",
    "channels",
    NULL
};

static int bt_audioservice_send(int sk, const bt_audio_msg_header_t *msg) {
    int e;
    pa_log/*_debug*/("sending %s", bt_audio_strmsg(msg->msg_type));
    if (send(sk, msg, BT_AUDIO_IPC_PACKET_SIZE, 0) > 0)
        e = 0;
    else {
        e = -errno;
        pa_log_error("Error sending data to audio service: %s(%d)", strerror(errno), errno);
    }
    return e;
}

static int bt_audioservice_recv(int sk, bt_audio_msg_header_t *inmsg) {
    int e;
    const char *type;

    pa_log/*_debug*/("trying to receive msg from audio service...");
    if (recv(sk, inmsg, BT_AUDIO_IPC_PACKET_SIZE, 0) > 0) {
        type = bt_audio_strmsg(inmsg->msg_type);
        if (type) {
            pa_log/*_debug*/("Received %s", type);
            e = 0;
        }
        else {
            e = -EINVAL;
            pa_log_error("Bogus message type %d received from audio service", inmsg->msg_type);
        }
    }
    else {
        e = -errno;
        pa_log_error("Error receiving data from audio service: %s(%d)", strerror(errno), errno);
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
    char buf[BT_AUDIO_IPC_PACKET_SIZE];
    bt_audio_rsp_msg_header_t *rsp_hdr = (void*) buf;
    struct bt_getcapabilities_req *getcaps_req = (void*) buf;
    struct bt_getcapabilities_rsp *getcaps_rsp = (void*) buf;

    memset(getcaps_req, 0, BT_AUDIO_IPC_PACKET_SIZE);
    getcaps_req->h.msg_type = BT_GETCAPABILITIES_REQ;
    strncpy(getcaps_req->device, u->addr, 18);
    if (strcasecmp(u->profile, "a2dp") == 0)
        getcaps_req->transport = BT_CAPABILITIES_TRANSPORT_A2DP;
    else if (strcasecmp(u->profile, "hsp") == 0)
        getcaps_req->transport = BT_CAPABILITIES_TRANSPORT_SCO;
    else {
        pa_log_error("invalid profile argument: %s", u->profile);
        return -1;
    }
    getcaps_req->flags = 0;
    getcaps_req->flags |= BT_FLAG_AUTOCONNECT;

    e = bt_audioservice_send(u->audioservice_fd, &getcaps_req->h);
    if (e < 0) {
        pa_log_error("failed to send GETCAPABILITIES_REQ");
        return e;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &rsp_hdr->msg_h, BT_GETCAPABILITIES_RSP);
    if (e < 0) {
        pa_log_error("failed to expect for GETCAPABILITIES_RSP");
        return e;
    }
    if (rsp_hdr->posix_errno != 0) {
        pa_log_error("BT_GETCAPABILITIES failed : %s (%d)", strerror(rsp_hdr->posix_errno), rsp_hdr->posix_errno);
        return -rsp_hdr->posix_errno;
    }

    if ((u->transport = getcaps_rsp->transport) == BT_CAPABILITIES_TRANSPORT_A2DP)
        u->a2dp.sbc_capabilities = getcaps_rsp->sbc_capabilities;

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
    unsigned int max_bitpool, min_bitpool, rate, channels;
    int dir;

    switch (u->rate) {
        case 48000:
            cap->frequency = BT_SBC_SAMPLING_FREQ_48000;
            break;
        case 44100:
            cap->frequency = BT_SBC_SAMPLING_FREQ_44100;
            break;
        case 32000:
            cap->frequency = BT_SBC_SAMPLING_FREQ_32000;
            break;
        case 16000:
            cap->frequency = BT_SBC_SAMPLING_FREQ_16000;
            break;
        default:
            pa_log_error("Rate %d not supported", rate);
            return -1;
    }

//    if (cfg->has_channel_mode)
//        cap->channel_mode = cfg->channel_mode;
//    else 
    if (channels == 2) {
        if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_JOINT_STEREO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
        else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_STEREO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_STEREO;
        else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL;
    } else {
        if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_MONO)
            cap->channel_mode = BT_A2DP_CHANNEL_MODE_MONO;
    }

    if (!cap->channel_mode) {
        pa_log_error("No supported channel modes");
        return -1;
    }

//    if (cfg->has_block_length)
//        cap->block_length = cfg->block_length;
//    else 
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

//    if (cfg->has_subbands)
//        cap->subbands = cfg->subbands;
    if (cap->subbands & BT_A2DP_SUBBANDS_8)
        cap->subbands = BT_A2DP_SUBBANDS_8;
    else if (cap->subbands & BT_A2DP_SUBBANDS_4)
        cap->subbands = BT_A2DP_SUBBANDS_4;
    else {
        pa_log_error("No supported subbands");
        return -1;
    }

//    if (cfg->has_allocation_method)
//        cap->allocation_method = cfg->allocation_method;
    if (cap->allocation_method & BT_A2DP_ALLOCATION_LOUDNESS)
        cap->allocation_method = BT_A2DP_ALLOCATION_LOUDNESS;
    else if (cap->allocation_method & BT_A2DP_ALLOCATION_SNR)
        cap->allocation_method = BT_A2DP_ALLOCATION_SNR;

//    if (cfg->has_bitpool)
//        min_bitpool = max_bitpool = cfg->bitpool;
//    else {
    min_bitpool = MAX(MIN_BITPOOL, cap->min_bitpool);
    max_bitpool = MIN(default_bitpool(cap->frequency, cap->channel_mode), cap->max_bitpool);
//    }

    cap->min_bitpool = min_bitpool;
    cap->max_bitpool = max_bitpool;

    return 0;
}

static void bt_a2dp_setup(struct bt_a2dp *a2dp) {
    sbc_capabilities_t active_capabilities = a2dp->sbc_capabilities;

    if (a2dp->sbc_initialized)
        sbc_reinit(&a2dp->sbc, 0);
    else
        sbc_init(&a2dp->sbc, 0);
    a2dp->sbc_initialized = 1;

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

    a2dp->sbc.allocation = active_capabilities.allocation_method == BT_A2DP_ALLOCATION_SNR ? SBC_AM_SNR : SBC_AM_LOUDNESS;

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
    a2dp->codesize = sbc_get_codesize(&a2dp->sbc);
//    a2dp->count = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
}

static int bt_setconf(struct userdata *u) {
    int e;
    char buf[BT_AUDIO_IPC_PACKET_SIZE];
    bt_audio_rsp_msg_header_t *rsp_hdr = (void*) buf;
    struct bt_setconfiguration_req *setconf_req = (void*) buf;
    struct bt_setconfiguration_rsp *setconf_rsp = (void*) buf;

    if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP) {
        e = bt_a2dp_init(u);
        if (e < 0) {
            pa_log_error("a2dp_init error");
            return e;
        }
    }

    memset(setconf_req, 0, BT_AUDIO_IPC_PACKET_SIZE);
    setconf_req->h.msg_type = BT_SETCONFIGURATION_REQ;
    strncpy(setconf_req->device, u->addr, 18);
    setconf_req->transport = u->transport;
    if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP)
        setconf_req->sbc_capabilities = u->a2dp.sbc_capabilities;
    setconf_req->access_mode = BT_CAPABILITIES_ACCESS_MODE_WRITE;

    e = bt_audioservice_send(u->audioservice_fd, &setconf_req->h);
    if (e < 0) {
        pa_log_error("failed to send BT_SETCONFIGURATION_REQ");
        return e;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &rsp_hdr->msg_h, BT_SETCONFIGURATION_RSP);
    if (e < 0) {
        pa_log_error("failed to expect BT_SETCONFIGURATION_RSP");
        return e;
    }

    if (rsp_hdr->posix_errno != 0) {
        pa_log_error("BT_SETCONFIGURATION failed : %s(%d)", strerror(rsp_hdr->posix_errno), rsp_hdr->posix_errno);
        return -rsp_hdr->posix_errno;
    }

    u->transport = setconf_rsp->transport;
    u->link_mtu = setconf_rsp->link_mtu;

    /* setup SBC encoder now we agree on parameters */
    if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP) {
        bt_a2dp_setup(&u->a2dp);
        pa_log/*debug*/("\tallocation=%u\n\tsubbands=%u\n\tblocks=%u\n\tbitpool=%u\n",
                u->a2dp.sbc.allocation, u->a2dp.sbc.subbands, u->a2dp.sbc.blocks, u->a2dp.sbc.bitpool);
    }

    return 0;
}

static int bt_getstreamfd(struct userdata *u) {
    int e, opt_name;
    char buf[BT_AUDIO_IPC_PACKET_SIZE];
    struct bt_streamstart_req *start_req = (void*) buf;
    bt_audio_rsp_msg_header_t *rsp_hdr = (void*) buf;
    struct bt_streamfd_ind *streamfd_ind = (void*) buf;

    memset(start_req, 0, BT_AUDIO_IPC_PACKET_SIZE);
    start_req->h.msg_type = BT_STREAMSTART_REQ;

    e = bt_audioservice_send(u->audioservice_fd, &start_req->h);
    if (e < 0) {
        pa_log_error("failed to send BT_STREAMSTART_REQ");
        return e;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &rsp_hdr->msg_h, BT_STREAMSTART_RSP);
    if (e < 0) {
        pa_log_error("failed to expect BT_STREAMSTART_RSP");
        return e;
    }

    if (rsp_hdr->posix_errno != 0) {
        pa_log_error("BT_START failed : %s(%d)", strerror(rsp_hdr->posix_errno), rsp_hdr->posix_errno);
        return -rsp_hdr->posix_errno;
    }

    e = bt_audioservice_expect(u->audioservice_fd, &streamfd_ind->h, BT_STREAMFD_IND);
    if (e < 0) {
        pa_log_error("failed to expect BT_STREAMFD_IND");
        return e;
    }

    if (u->stream_fd >= 0)
        close(u->stream_fd);

    u->stream_fd = bt_audio_service_get_data_fd(u->audioservice_fd);
    if (u->stream_fd < 0) {
        pa_log_error("failed to get data fd");
        return -errno;
    }

//    if (u->transport == BT_CAPABILITIES_TRANSPORT_A2DP) {
//        opt_name = SO_SNDTIMEO;
//        if (setsockopt(u->stream_fd, SOL_SOCKET, opt_name, &t, sizeof(t)) < 0) {
//            pa_log_error("failed to set socket options for A2DP");
//            return -errno;
//        }
//    }
//    else {
//        opt_name = SCO_TXBUFS;
//        if (setsockopt(u->stream_fd, SOL_SCO, opt_name, &period_count, sizeof(period_count)) == 0)
//            return 0;
//        opt_name = SO_SNDBUF;
//        if (setsockopt(u->stream_fd, SOL_SCO, opt_name, &period_count, sizeof(period_count)) == 0)
//            return 0;
//        /* FIXME : handle error codes */
//    }

    return 0;
}

static int bt_hw_constraint(struct userdata *u) {
    /*TODO: A2DP */
    u->ss.format = PA_SAMPLE_S16LE;
    u->ss.rate = 8000;
    u->ss.channels = 1;
    return 0;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

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
//            pa_usec_t w, r;
//            r = pa_smoother_get(u->smoother, pa_rtclock_usec());
//            w = pa_bytes_to_usec(u->offset + u->memchunk.length, &u->sink->sample_spec);
//            *((pa_usec_t*) data) = w > r ? w - r : 0;
            break;
        }

//        case SINK_MESSAGE_PASS_SOCKET: {
//            struct pollfd *pollfd;
//            pa_assert(!u->rtpoll_item);
//            u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
//            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
//            pollfd->fd = u->fd;
//            pollfd->events = pollfd->revents = 0;
//            return 0;
//        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void thread_func(void *userdata) {
//    struct userdata *u = userdata;
//    int write_type = 0;
//
//    pa_assert(u);
//
//    pa_log_debug("Thread starting up");
//
//    pa_thread_mq_install(&u->thread_mq);
//    pa_rtpoll_install(u->rtpoll);
//
//    pa_smoother_set_time_offset(u->smoother, pa_rtclock_usec());
//
//    for (;;) {
//        int ret;
//
//        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
//            if (u->sink->thread_info.rewind_requested)
//                pa_sink_process_rewind(u->sink, 0);
//
//        if (u->rtpoll_item) {
//            struct pollfd *pollfd;
//            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
//
//            /* Render some data and write it to the fifo */
//            if (PA_SINK_IS_OPENED(u->sink->thread_info.state) && pollfd->revents) {
//                pa_usec_t usec;
//                int64_t n;
//
//                for (;;) {
//                    ssize_t l;
//                    void *p;
//
//                    if (u->memchunk.length <= 0)
//                        pa_sink_render(u->sink, u->block_size, &u->memchunk);
//
//                    pa_assert(u->memchunk.length > 0);
//
//                    p = pa_memblock_acquire(u->memchunk.memblock);
//                    l = pa_write(u->fd, (uint8_t*) p + u->memchunk.index, u->memchunk.length, &write_type);
//                    pa_memblock_release(u->memchunk.memblock);
//
//                    pa_assert(l != 0);
//
//                    if (l < 0) {
//
//                        if (errno == EINTR)
//                            continue;
//                        else if (errno == EAGAIN) {
//
//                            /* OK, we filled all socket buffers up
//                             * now. */
//                            goto filled_up;
//
//                        } else {
//                            pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
//                            goto fail;
//                        }
//
//                    } else {
//                        u->offset += l;
//
//                        u->memchunk.index += l;
//                        u->memchunk.length -= l;
//
//                        if (u->memchunk.length <= 0) {
//                            pa_memblock_unref(u->memchunk.memblock);
//                            pa_memchunk_reset(&u->memchunk);
//                        }
//
//                        pollfd->revents = 0;
//
//                        if (u->memchunk.length > 0)
//
//                            /* OK, we wrote less that we asked for,
//                             * hence we can assume that the socket
//                             * buffers are full now */
//                            goto filled_up;
//                    }
//                }
//
//            filled_up:
//
//                /* At this spot we know that the socket buffers are
//                 * fully filled up. This is the best time to estimate
//                 * the playback position of the server */
//
//                n = u->offset;
//
//#ifdef SIOCOUTQ
//                {
//                    int l;
//                    if (ioctl(u->fd, SIOCOUTQ, &l) >= 0 && l > 0)
//                        n -= l;
//                }
//#endif
//
//                usec = pa_bytes_to_usec(n, &u->sink->sample_spec);
//
//                if (usec > u->latency)
//                    usec -= u->latency;
//                else
//                    usec = 0;
//
//                pa_smoother_put(u->smoother, pa_rtclock_usec(), usec);
//            }
//
//            /* Hmm, nothing to do. Let's sleep */
//            pollfd->events = PA_SINK_IS_OPENED(u->sink->thread_info.state) ? POLLOUT : 0;
//        }
//
//        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
//            goto fail;
//
//        if (ret == 0)
//            goto finish;
//
//        if (u->rtpoll_item) {
//            struct pollfd* pollfd;
//
//            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
//
//            if (pollfd->revents & ~POLLOUT) {
//                pa_log("FIFO shutdown.");
//                goto fail;
//            }
//        }
//    }
//
//fail:
//    /* If this was no regular exit from the loop we have to continue
//     * processing messages until we received PA_MESSAGE_SHUTDOWN */
//    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
//    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);
//
//finish:
//    pa_log_debug("Thread shutting down");
}

int pa__init(pa_module* m) {
    int e;
    char *rate, *channels;
    pa_modargs *ma;
    pa_sink_new_data data;
    struct userdata *u;

    pa_assert(m);
    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;
    u->audioservice_fd = -1;
    u->stream_fd = -1;
    u->transport = -1;
    u->smoother = pa_smoother_new(PA_USEC_PER_SEC, PA_USEC_PER_SEC*2, TRUE, 10);
    pa_memchunk_reset(&u->memchunk);
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, u->core->mainloop, u->rtpoll);
    u->rtpoll_item = NULL;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("failed to parse module arguments");
        goto fail;
    }
    if (!(u->name = pa_modargs_get_value(ma, "name", DEFAULT_SINK_NAME))) {
        pa_log_error("failed to get device address from module arguments");
        goto fail;
    }
    if (!(u->addr = pa_modargs_get_value(ma, "addr", NULL))) {
        pa_log_error("failed to get device address from module arguments");
        goto fail;
    }
    if (!(u->profile = pa_modargs_get_value(ma, "profile", NULL))) {
        pa_log_error("failed to get profile from module arguments");
        goto fail;
    }
    if (!(rate = pa_modargs_get_value(ma, "rate", NULL))) {
        pa_log_error("failed to get rate from module arguments");
        goto fail;
    }
    u->rate = atoi(rate);
    if (!(channels = pa_modargs_get_value(ma, "channels", NULL))) {
        pa_log_error("failed to get profile from module arguments");
        goto fail;
    }
    u->channels = atoi(channels);
    pa_log("Loading module-bt-device for %s (%s), profile %s", u->name, u->addr, u->profile);

    /* connect to the bluez audio service */
    u->audioservice_fd = bt_audio_service_open();
    if (u->audioservice_fd <= 0) {
        pa_log_error("couldn't connect to bluetooth audio service");
        goto fail;
    }

    /* queries device capabilities */
    e = bt_getcaps(u);
    if (e < 0) {
        pa_log_error("failed to get device capabilities");
        goto fail;
    }

    /* configures the connection */
    e = bt_setconf(u);
    if (e < 0) {
        pa_log_error("failed to set config");
        goto fail;
    }

    /* gets the device socket */
    e = bt_getstreamfd(u);
    if (e < 0) {
        pa_log_error("failed to get stream fd (%d)", e);
        goto fail;
    }

    /* configure hw supported sample specs */
    e = bt_hw_constraint(u);
    if (e < 0) {
        pa_log_error("failed to configure sample spec");
        goto fail;
    }

    /* create sink */
    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, u->name);
    pa_sink_new_data_set_sample_spec(&data, &u->ss);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->name);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Bluetooth sink '%s' (%s)", u->name, u->addr);
    u->sink = pa_sink_new(m->core, &data, PA_SINK_HARDWARE|PA_SINK_NETWORK);
    pa_sink_new_data_done(&data);
    if (!u->sink) {
        pa_log_error("failed to create sink");
        goto fail;
    }
    u->sink->userdata = u;
    u->sink->parent.process_msg = sink_process_msg;
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    /* start rt thread */
    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log_error("failed to create thread");
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
    pa_log("Unloading module-bt-device");
}

