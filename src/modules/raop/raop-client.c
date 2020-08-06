/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie
  Copyright 2013 Hajime Fujita
  Copyright 2013 Martin Blanchard

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

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <math.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/sample.h>

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/arpa-inet.h>
#include <pulsecore/socket-client.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/log.h>
#include <pulsecore/parseaddr.h>
#include <pulsecore/macro.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/random.h>
#include <pulsecore/poll.h>

#include <modules/rtp/rtsp_client.h>

#include "raop-client.h"
#include "raop-packet-buffer.h"
#include "raop-crypto.h"
#include "raop-util.h"

#define DEFAULT_RAOP_PORT 5000

#define FRAMES_PER_TCP_PACKET 4096
#define FRAMES_PER_UDP_PACKET 352

#define RTX_BUFFERING_SECONDS 4

#define DEFAULT_TCP_AUDIO_PORT   6000
#define DEFAULT_UDP_AUDIO_PORT   6000
#define DEFAULT_UDP_CONTROL_PORT 6001
#define DEFAULT_UDP_TIMING_PORT  6002

#define DEFAULT_USER_AGENT "iTunes/11.0.4 (Windows; N)"
#define DEFAULT_USER_NAME  "iTunes"

#define JACK_STATUS_DISCONNECTED 0
#define JACK_STATUS_CONNECTED    1
#define JACK_TYPE_ANALOG         0
#define JACK_TYPE_DIGITAL        1

#define VOLUME_MAX  0.0
#define VOLUME_DEF -30.0
#define VOLUME_MIN -144.0

#define UDP_DEFAULT_PKT_BUF_SIZE 1000
#define APPLE_CHALLENGE_LENGTH 16

struct pa_raop_client {
    pa_core *core;
    char *host;
    uint16_t port;
    pa_rtsp_client *rtsp;
    char *sci, *sid;
    char *password;
    bool autoreconnect;

    pa_raop_protocol_t protocol;
    pa_raop_encryption_t encryption;
    pa_raop_codec_t codec;

    pa_raop_secret *secret;

    int tcp_sfd;

    int udp_sfd;
    int udp_cfd;
    int udp_tfd;

    pa_raop_packet_buffer *pbuf;

    uint16_t seq;
    uint32_t rtptime;
    bool is_recording;
    uint32_t ssrc;

    bool is_first_packet;
    uint32_t sync_interval;
    uint32_t sync_count;

    uint8_t jack_type;
    uint8_t jack_status;

    pa_raop_client_state_cb_t state_callback;
    void *state_userdata;
};

/* Audio TCP packet header [16x8] (cf. rfc4571):
 *  [0,1]   Frame marker; seems always 0x2400
 *  [2,3]   RTP packet size (following): 0x0000 (to be set)
 *   [4,5]   RTP v2: 0x80
 *   [5]     Payload type: 0x60 | Marker bit: 0x80 (always set)
 *   [6,7]   Sequence number: 0x0000 (to be set)
 *   [8,11]  Timestamp: 0x00000000 (to be set)
 *   [12,15] SSRC: 0x00000000 (to be set) */
#define PAYLOAD_TCP_AUDIO_DATA 0x60
static const uint8_t tcp_audio_header[16] = {
    0x24, 0x00, 0x00, 0x00,
    0x80, 0xe0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/* Audio UDP packet header [12x8] (cf. rfc3550):
 *  [0]    RTP v2: 0x80
 *  [1]    Payload type: 0x60
 *  [2,3]  Sequence number: 0x0000 (to be set)
 *  [4,7]  Timestamp: 0x00000000 (to be set)
 *  [8,12] SSRC: 0x00000000 (to be set) */
#define PAYLOAD_UDP_AUDIO_DATA 0x60
static const uint8_t udp_audio_header[12] = {
    0x80, 0x60, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/* Audio retransmission UDP packet header [4x8]:
 *  [0] RTP v2: 0x80
 *  [1] Payload type: 0x56 | Marker bit: 0x80 (always set)
 *  [2] Unknown; seems always 0x01
 *  [3] Unknown; seems some random number around 0x20~0x40 */
#define PAYLOAD_RETRANSMIT_REQUEST 0x55
#define PAYLOAD_RETRANSMIT_REPLY   0x56
static const uint8_t udp_audio_retrans_header[4] = {
    0x80, 0xd6, 0x00, 0x00
};

/* Sync packet header [8x8] (cf. rfc3550):
 *  [0]   RTP v2: 0x80
 *  [1]   Payload type: 0x54 | Marker bit: 0x80 (always set)
 *  [2,3] Sequence number: 0x0007
 *  [4,7] Timestamp: 0x00000000 (to be set) */
static const uint8_t udp_sync_header[8] = {
    0x80, 0xd4, 0x00, 0x07,
    0x00, 0x00, 0x00, 0x00
};

/* Timing packet header [8x8] (cf. rfc3550):
 *  [0]   RTP v2: 0x80
 *  [1]   Payload type: 0x53 | Marker bit: 0x80 (always set)
 *  [2,3] Sequence number: 0x0007
 *  [4,7] Timestamp: 0x00000000 (unused) */
#define PAYLOAD_TIMING_REQUEST  0x52
#define PAYLOAD_TIMING_REPLY    0x53
static const uint8_t udp_timing_header[8] = {
    0x80, 0xd3, 0x00, 0x07,
    0x00, 0x00, 0x00, 0x00
};

/**
 * Function to trim a given character at the end of a string (no realloc).
 * @param str Pointer to string
 * @param rc Character to trim
 */
static inline void rtrim_char(char *str, char rc) {
    char *sp = str + strlen(str) - 1;
    while (sp >= str && *sp == rc) {
        *sp = '\0';
        sp -= 1;
    }
}

/**
 * Function to convert a timeval to ntp timestamp.
 * @param tv Pointer to the timeval structure
 * @return The NTP timestamp
 */
static inline uint64_t timeval_to_ntp(struct timeval *tv) {
    uint64_t ntp = 0;

    /* Converting micro seconds to a fraction. */
    ntp = (uint64_t) tv->tv_usec * UINT32_MAX / PA_USEC_PER_SEC;
    /* Moving reference from  1 Jan 1970 to 1 Jan 1900 (seconds). */
    ntp |= (uint64_t) (tv->tv_sec + 0x83aa7e80) << 32;

    return ntp;
}

/**
 * Function to write bits into a buffer.
 * @param buffer Handle to the buffer. It will be incremented if new data requires it.
 * @param bit_pos A pointer to a position buffer to keep track the current write location (0 for MSB, 7 for LSB)
 * @param size A pointer to the byte size currently written. This allows the calling function to do simple buffer overflow checks
 * @param data The data to write
 * @param data_bit_len The number of bits from data to write
 */
static inline void bit_writer(uint8_t **buffer, uint8_t *bit_pos, size_t *size, uint8_t data, uint8_t data_bit_len) {
    int bits_left, bit_overflow;
    uint8_t bit_data;

    if (!data_bit_len)
        return;

    /* If bit pos is zero, we will definitely use at least one bit from the current byte so size increments. */
    if (!*bit_pos)
        *size += 1;

    /* Calc the number of bits left in the current byte of buffer. */
    bits_left = 7 - *bit_pos  + 1;
    /* Calc the overflow of bits in relation to how much space we have left... */
    bit_overflow = bits_left - data_bit_len;
    if (bit_overflow >= 0) {
        /* We can fit the new data in our current byte.
         * As we write from MSB->LSB we need to left shift by the overflow amount. */
        bit_data = data << bit_overflow;
        if (*bit_pos)
            **buffer |= bit_data;
        else
            **buffer = bit_data;
        /* If our data fits exactly into the current byte, we need to increment our pointer. */
        if (0 == bit_overflow) {
            /* Do not increment size as it will be incremented on next call as bit_pos is zero. */
            *buffer += 1;
            *bit_pos = 0;
        } else {
            *bit_pos += data_bit_len;
        }
    } else {
        /* bit_overflow is negative, there for we will need a new byte from our buffer
         * Firstly fill up what's left in the current byte. */
        bit_data = data >> -bit_overflow;
        **buffer |= bit_data;
        /* Increment our buffer pointer and size counter. */
        *buffer += 1;
        *size += 1;
        **buffer = data << (8 + bit_overflow);
        *bit_pos = -bit_overflow;
    }
}

static size_t write_ALAC_data(uint8_t *packet, const size_t max, uint8_t *raw, size_t *length, bool compress) {
    uint32_t nbs = (*length / 2) / 2;
    uint8_t *ibp, *maxibp;
    uint8_t *bp, bpos;
    size_t size = 0;

    bp = packet;
    pa_memzero(packet, max);
    size = bpos = 0;

    bit_writer(&bp, &bpos, &size, 1, 3); /* channel=1, stereo */
    bit_writer(&bp, &bpos, &size, 0, 4); /* Unknown */
    bit_writer(&bp, &bpos, &size, 0, 8); /* Unknown */
    bit_writer(&bp, &bpos, &size, 0, 4); /* Unknown */
    bit_writer(&bp, &bpos, &size, 1, 1); /* Hassize */
    bit_writer(&bp, &bpos, &size, 0, 2); /* Unused */
    bit_writer(&bp, &bpos, &size, 1, 1); /* Is-not-compressed */
    /* Size of data, integer, big endian. */
    bit_writer(&bp, &bpos, &size, (nbs >> 24) & 0xff, 8);
    bit_writer(&bp, &bpos, &size, (nbs >> 16) & 0xff, 8);
    bit_writer(&bp, &bpos, &size, (nbs >> 8)  & 0xff, 8);
    bit_writer(&bp, &bpos, &size, (nbs)       & 0xff, 8);

    ibp = raw;
    maxibp = raw + (4 * nbs) - 4;
    while (ibp <= maxibp) {
        /* Byte swap stereo data. */
        bit_writer(&bp, &bpos, &size, *(ibp + 1), 8);
        bit_writer(&bp, &bpos, &size, *(ibp + 0), 8);
        bit_writer(&bp, &bpos, &size, *(ibp + 3), 8);
        bit_writer(&bp, &bpos, &size, *(ibp + 2), 8);
        ibp += 4;
    }

    *length = (ibp - raw);
    return size;
}

static size_t build_tcp_audio_packet(pa_raop_client *c, pa_memchunk *block, pa_memchunk *packet) {
    const size_t head = sizeof(tcp_audio_header);
    uint32_t *buffer = NULL;
    uint8_t *raw = NULL;
    size_t length, size;

    raw = pa_memblock_acquire(block->memblock);
    buffer = pa_memblock_acquire(packet->memblock);
    buffer += packet->index / sizeof(uint32_t);
    raw += block->index;

    /* Wrap sequence number to 0 then UINT16_MAX is reached */
    if (c->seq == UINT16_MAX)
        c->seq = 0;
    else
        c->seq++;

    memcpy(buffer, tcp_audio_header, sizeof(tcp_audio_header));
    buffer[1] |= htonl((uint32_t) c->seq);
    buffer[2] = htonl(c->rtptime);
    buffer[3] = htonl(c->ssrc);

    length = block->length;
    size = sizeof(tcp_audio_header);
    if (c->codec == PA_RAOP_CODEC_ALAC)
        size += write_ALAC_data(((uint8_t *) buffer + head), packet->length - head, raw, &length, false);
    else {
        pa_log_debug("Only ALAC encoding is supported, sending zeros...");
        pa_memzero(((uint8_t *) buffer + head), packet->length - head);
        size += length;
    }

    c->rtptime += length / 4;

    pa_memblock_release(block->memblock);

    buffer[0] |= htonl((uint32_t) size - 4);
    if (c->encryption == PA_RAOP_ENCRYPTION_RSA)
        pa_raop_aes_encrypt(c->secret, (uint8_t *) buffer + head, size - head);

    pa_memblock_release(packet->memblock);
    packet->length = size;

    return size;
}

static ssize_t send_tcp_audio_packet(pa_raop_client *c, pa_memchunk *block, size_t offset) {
    static int write_type = 0;
    const size_t max = sizeof(tcp_audio_header) + 8 + 16384;
    pa_memchunk *packet = NULL;
    uint8_t *buffer = NULL;
    double progress = 0.0;
    ssize_t written = -1;
    size_t done = 0;

    packet = pa_raop_packet_buffer_retrieve(c->pbuf, c->seq);

    if (!packet || (packet && packet->length <= 0)) {
        pa_assert(block->index == offset);

        if (!(packet = pa_raop_packet_buffer_prepare(c->pbuf, c->seq, max)))
            return -1;

        packet->index = 0;
        packet->length = max;
        if (!build_tcp_audio_packet(c, block, packet))
            return -1;
    }

    buffer = pa_memblock_acquire(packet->memblock);

    pa_assert(buffer);

    buffer += packet->index;
    if (buffer && packet->length > 0)
        written = pa_write(c->tcp_sfd, buffer, packet->length, &write_type);
    if (written > 0) {
        progress = (double) written / (double) packet->length;
        packet->length -= written;
        packet->index += written;

        done = block->length * progress;
        block->length -= done;
        block->index += done;
    }

    pa_memblock_release(packet->memblock);

    return written;
}

static size_t build_udp_audio_packet(pa_raop_client *c, pa_memchunk *block, pa_memchunk *packet) {
    const size_t head = sizeof(udp_audio_header);
    uint32_t *buffer = NULL;
    uint8_t *raw = NULL;
    size_t length, size;

    raw = pa_memblock_acquire(block->memblock);
    buffer = pa_memblock_acquire(packet->memblock);
    buffer += packet->index / sizeof(uint32_t);
    raw += block->index;

    memcpy(buffer, udp_audio_header, sizeof(udp_audio_header));
    if (c->is_first_packet)
        buffer[0] |= htonl((uint32_t) 0x80 << 16);
    buffer[0] |= htonl((uint32_t) c->seq);
    buffer[1] = htonl(c->rtptime);
    buffer[2] = htonl(c->ssrc);

    length = block->length;
    size = sizeof(udp_audio_header);
    if (c->codec == PA_RAOP_CODEC_ALAC)
        size += write_ALAC_data(((uint8_t *) buffer + head), packet->length - head, raw, &length, false);
    else {
        pa_log_debug("Only ALAC encoding is supported, sending zeros...");
        pa_memzero(((uint8_t *) buffer + head), packet->length - head);
        size += length;
    }

    c->rtptime += length / 4;

    /* Wrap sequence number to 0 then UINT16_MAX is reached */
    if (c->seq == UINT16_MAX)
        c->seq = 0;
    else
        c->seq++;

    pa_memblock_release(block->memblock);

    if (c->encryption == PA_RAOP_ENCRYPTION_RSA)
        pa_raop_aes_encrypt(c->secret, (uint8_t *) buffer + head, size - head);

    pa_memblock_release(packet->memblock);
    packet->length = size;

    return size;
}

static ssize_t send_udp_audio_packet(pa_raop_client *c, pa_memchunk *block, size_t offset) {
    const size_t max = sizeof(udp_audio_retrans_header) + sizeof(udp_audio_header) + 8 + 1408;
    pa_memchunk *packet = NULL;
    uint8_t *buffer = NULL;
    ssize_t written = -1;

    /* UDP packet has to be sent at once ! */
    pa_assert(block->index == offset);

    if (!(packet = pa_raop_packet_buffer_prepare(c->pbuf, c->seq, max)))
        return -1;

    packet->index = sizeof(udp_audio_retrans_header);
    packet->length = max - sizeof(udp_audio_retrans_header);
    if (!build_udp_audio_packet(c, block, packet))
        return -1;

    buffer = pa_memblock_acquire(packet->memblock);

    pa_assert(buffer);

    buffer += packet->index;
    if (buffer && packet->length > 0)
        written = pa_write(c->udp_sfd, buffer, packet->length, NULL);
    if (written < 0 && errno == EAGAIN) {
        pa_log_debug("Discarding UDP (audio, seq=%d) packet due to EAGAIN (%s)", c->seq, pa_cstrerror(errno));
        written = packet->length;
    }

    pa_memblock_release(packet->memblock);
    /* It is meaningless to preseve the partial data */
    block->index += block->length;
    block->length = 0;

    return written;
}

static size_t rebuild_udp_audio_packet(pa_raop_client *c, uint16_t seq, pa_memchunk *packet) {
    size_t size = sizeof(udp_audio_retrans_header);
    uint32_t *buffer = NULL;

    buffer = pa_memblock_acquire(packet->memblock);

    memcpy(buffer, udp_audio_retrans_header, sizeof(udp_audio_retrans_header));
    buffer[0] |= htonl((uint32_t) seq);
    size += packet->length;

    pa_memblock_release(packet->memblock);
    packet->length += sizeof(udp_audio_retrans_header);
    packet->index -= sizeof(udp_audio_retrans_header);

    return size;
}

static ssize_t resend_udp_audio_packets(pa_raop_client *c, uint16_t seq, uint16_t nbp) {
    ssize_t total = 0;
    int i = 0;

    for (i = 0; i < nbp; i++) {
        pa_memchunk *packet = NULL;
        uint8_t *buffer = NULL;
        ssize_t written = -1;

        if (!(packet = pa_raop_packet_buffer_retrieve(c->pbuf, seq + i)))
            continue;

        if (packet->index > 0) {
            if (!rebuild_udp_audio_packet(c, seq + i, packet))
                continue;
        }

        pa_assert(packet->index == 0);

        buffer = pa_memblock_acquire(packet->memblock);

        pa_assert(buffer);

        if (buffer && packet->length > 0)
            written = pa_write(c->udp_cfd, buffer, packet->length, NULL);
        if (written < 0 && errno == EAGAIN) {
            pa_log_debug("Discarding UDP (audio-retransmitted, seq=%d) packet due to EAGAIN", seq + i);
            pa_memblock_release(packet->memblock);
            continue;
        }

        pa_memblock_release(packet->memblock);
        total +=  written;
    }

    return total;
}

/* Caller has to free the allocated memory region for packet */
static size_t build_udp_sync_packet(pa_raop_client *c, uint32_t stamp, uint32_t **packet) {
    const size_t size = sizeof(udp_sync_header) + 12;
    const uint32_t delay = 88200;
    uint32_t *buffer = NULL;
    uint64_t transmitted = 0;
    struct timeval tv;

    *packet = NULL;
    if (!(buffer = pa_xmalloc0(size)))
        return 0;

    memcpy(buffer, udp_sync_header, sizeof(udp_sync_header));
    if (c->is_first_packet)
        buffer[0] |= 0x10;
    stamp -= delay;
    buffer[1] = htonl(stamp);
    /* Set the transmitted timestamp to current time. */
    transmitted = timeval_to_ntp(pa_rtclock_get(&tv));
    buffer[2] = htonl(transmitted >> 32);
    buffer[3] = htonl(transmitted & 0xffffffff);
    stamp += delay;
    buffer[4] = htonl(stamp);

    *packet = buffer;
    return size;
}

static ssize_t send_udp_sync_packet(pa_raop_client *c, uint32_t stamp) {
    uint32_t * packet = NULL;
    ssize_t written = 0;
    size_t size = 0;

    size = build_udp_sync_packet(c, stamp, &packet);
    if (packet != NULL && size > 0) {
        written = pa_loop_write(c->udp_cfd, packet, size, NULL);
        pa_xfree(packet);
    }

    return written;
}

static size_t handle_udp_control_packet(pa_raop_client *c, const uint8_t packet[], ssize_t size) {
    uint8_t payload = 0;
    uint16_t seq, nbp = 0;
    ssize_t written = 0;

    /* Control packets are 8 bytes long:  */
    if (size != 8 || packet[0] != 0x80)
        return 1;

    seq = ntohs((uint16_t) (packet[4] | packet[5] << 8));
    nbp = ntohs((uint16_t) (packet[6] | packet[7] << 8));
    if (nbp <= 0)
        return 1;

    /* The marker bit is always set (see rfc3550 for packet structure) ! */
    payload = packet[1] ^ 0x80;
    switch (payload) {
        case PAYLOAD_RETRANSMIT_REQUEST:
            pa_log_debug("Resending %u packets starting at %u", nbp, seq);
            written = resend_udp_audio_packets(c, seq, nbp);
            break;
        case PAYLOAD_RETRANSMIT_REPLY:
        default:
            pa_log_debug("Got an unexpected payload type on control channel (%u) !", payload);
            break;
    }

    return written;
}

/* Caller has to free the allocated memory region for packet */
static size_t build_udp_timing_packet(pa_raop_client *c, const uint32_t data[6], uint64_t received, uint32_t **packet) {
    const size_t size = sizeof(udp_timing_header) + 24;
    uint32_t *buffer = NULL;
    uint64_t transmitted = 0;
    struct timeval tv;

    *packet = NULL;
    if (!(buffer = pa_xmalloc0(size)))
        return 0;

    memcpy(buffer, udp_timing_header, sizeof(udp_timing_header));
    /* Copying originate timestamp from the incoming request packet. */
    buffer[2] = data[4];
    buffer[3] = data[5];
    /* Set the receive timestamp to reception time. */
    buffer[4] = htonl(received >> 32);
    buffer[5] = htonl(received & 0xffffffff);
    /* Set the transmit timestamp to current time. */
    transmitted = timeval_to_ntp(pa_rtclock_get(&tv));
    buffer[6] = htonl(transmitted >> 32);
    buffer[7] = htonl(transmitted & 0xffffffff);

    *packet = buffer;
    return size;
}

static ssize_t send_udp_timing_packet(pa_raop_client *c, const uint32_t data[6], uint64_t received) {
    uint32_t * packet = NULL;
    ssize_t written = 0;
    size_t size = 0;

    size = build_udp_timing_packet(c, data, received, &packet);
    if (packet != NULL && size > 0) {
        written = pa_loop_write(c->udp_tfd, packet, size, NULL);
        pa_xfree(packet);
    }

    return written;
}

static size_t handle_udp_timing_packet(pa_raop_client *c, const uint8_t packet[], ssize_t size) {
    const uint32_t * data = NULL;
    uint8_t payload = 0;
    struct timeval tv;
    size_t written = 0;
    uint64_t rci = 0;

    /* Timing packets are 32 bytes long: 1 x 8 RTP header (no ssrc) + 3 x 8 NTP timestamps */
    if (size != 32 || packet[0] != 0x80)
        return 0;

    rci = timeval_to_ntp(pa_rtclock_get(&tv));
    data = (uint32_t *) (packet + sizeof(udp_timing_header));

    /* The marker bit is always set (see rfc3550 for packet structure) ! */
    payload = packet[1] ^ 0x80;
    switch (payload) {
        case PAYLOAD_TIMING_REQUEST:
            pa_log_debug("Sending timing packet at %" PRIu64 , rci);
            written = send_udp_timing_packet(c, data, rci);
            break;
        case PAYLOAD_TIMING_REPLY:
        default:
            pa_log_debug("Got an unexpected payload type on timing channel (%u) !", payload);
            break;
    }

    return written;
}

static void send_initial_udp_timing_packet(pa_raop_client *c) {
    uint32_t data[6] = { 0 };
    struct timeval tv;
    uint64_t initial_time = 0;

    initial_time = timeval_to_ntp(pa_rtclock_get(&tv));
    data[4] = htonl(initial_time >> 32);
    data[5] = htonl(initial_time & 0xffffffff);

    send_udp_timing_packet(c, data, initial_time);
}

static int connect_udp_socket(pa_raop_client *c, int fd, uint16_t port) {
    struct sockaddr_in sa4;
#ifdef HAVE_IPV6
    struct sockaddr_in6 sa6;
#endif
    struct sockaddr *sa;
    socklen_t salen;
    sa_family_t af;

    pa_zero(sa4);
#ifdef HAVE_IPV6
    pa_zero(sa6);
#endif
    if (inet_pton(AF_INET, c->host, &sa4.sin_addr) > 0) {
        sa4.sin_family = af = AF_INET;
        sa4.sin_port = htons(port);
        sa = (struct sockaddr *) &sa4;
        salen = sizeof(sa4);
#ifdef HAVE_IPV6
    } else if (inet_pton(AF_INET6, c->host, &sa6.sin6_addr) > 0) {
        sa6.sin6_family = af = AF_INET6;
        sa6.sin6_port = htons(port);
        sa = (struct sockaddr *) &sa6;
        salen = sizeof(sa6);
#endif
    } else {
        pa_log("Invalid destination '%s'", c->host);
        goto fail;
    }

    if (fd < 0 && (fd = pa_socket_cloexec(af, SOCK_DGRAM, 0)) < 0) {
        pa_log("socket() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    /* If the socket queue is full, let's drop packets */
    pa_make_udp_socket_low_delay(fd);
    pa_make_fd_nonblock(fd);

    if (connect(fd, sa, salen) < 0) {
        pa_log("connect() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    pa_log_debug("Connected to %s on port %d (SOCK_DGRAM)", c->host, port);
    return fd;

fail:
    if (fd >= 0)
        pa_close(fd);

    return -1;
}

static int open_bind_udp_socket(pa_raop_client *c, uint16_t *actual_port) {
    int fd = -1;
    uint16_t port;
    struct sockaddr_in sa4;
#ifdef HAVE_IPV6
    struct sockaddr_in6 sa6;
#endif
    struct sockaddr *sa;
    uint16_t *sa_port;
    socklen_t salen;
    sa_family_t af;
    int one = 1;

    pa_assert(actual_port);

    port = *actual_port;

    pa_zero(sa4);
#ifdef HAVE_IPV6
    pa_zero(sa6);
#endif
    if (inet_pton(AF_INET, pa_rtsp_localip(c->rtsp), &sa4.sin_addr) > 0) {
        sa4.sin_family = af = AF_INET;
        sa4.sin_port = htons(port);
        sa4.sin_addr.s_addr = INADDR_ANY;
        sa = (struct sockaddr *) &sa4;
        salen = sizeof(sa4);
        sa_port = &sa4.sin_port;
#ifdef HAVE_IPV6
    } else if (inet_pton(AF_INET6, pa_rtsp_localip(c->rtsp), &sa6.sin6_addr) > 0) {
        sa6.sin6_family = af = AF_INET6;
        sa6.sin6_port = htons(port);
        sa6.sin6_addr = in6addr_any;
        sa = (struct sockaddr *) &sa6;
        salen = sizeof(sa6);
        sa_port = &sa6.sin6_port;
#endif
    } else {
        pa_log("Could not determine which address family to use");
        goto fail;
    }

    if ((fd = pa_socket_cloexec(af, SOCK_DGRAM, 0)) < 0) {
        pa_log("socket() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

#ifdef SO_TIMESTAMP
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof(one)) < 0) {
        pa_log("setsockopt(SO_TIMESTAMP) failed: %s", pa_cstrerror(errno));
        goto fail;
    }
#else
    pa_log("SO_TIMESTAMP unsupported on this platform");
    goto fail;
#endif

    one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        pa_log("setsockopt(SO_REUSEADDR) failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    do {
        int ret;

        *sa_port = htons(port);
        ret = bind(fd, sa, salen);
        if (!ret)
            break;
        if (ret < 0 && errno != EADDRINUSE) {
            pa_log("bind() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
    } while (++port > 0);

    if (!port) {
        pa_log("Could not bind port");
        goto fail;
    }

    pa_log_debug("Socket bound to port %d (SOCK_DGRAM)", port);
    *actual_port = port;

    return fd;

fail:
    if (fd >= 0)
        pa_close(fd);

    return -1;
}

static void tcp_connection_cb(pa_socket_client *sc, pa_iochannel *io, void *userdata) {
    pa_raop_client *c = userdata;

    pa_assert(sc);
    pa_assert(c);

    pa_socket_client_unref(sc);

    if (!io) {
        pa_log("Connection failed: %s", pa_cstrerror(errno));
        return;
    }

    c->tcp_sfd = pa_iochannel_get_send_fd(io);
    pa_iochannel_set_noclose(io, true);
    pa_make_tcp_socket_low_delay(c->tcp_sfd);

    pa_iochannel_free(io);

    pa_log_debug("Connection established (TCP)");

    if (c->state_callback)
        c->state_callback(PA_RAOP_CONNECTED, c->state_userdata);
}

static void rtsp_stream_cb(pa_rtsp_client *rtsp, pa_rtsp_state_t state, pa_rtsp_status_t status, pa_headerlist *headers, void *userdata) {
    pa_raop_client *c = userdata;

    pa_assert(c);
    pa_assert(rtsp);
    pa_assert(rtsp == c->rtsp);

    switch (state) {
        case STATE_CONNECT: {
            char *key, *iv, *sdp = NULL;
            int frames = 0;
            const char *ip;
            char *url;
            int ipv;

            pa_log_debug("RAOP: CONNECTED");

            ip = pa_rtsp_localip(c->rtsp);
            if (pa_is_ip6_address(ip)) {
                ipv = 6;
                url = pa_sprintf_malloc("rtsp://[%s]/%s", ip, c->sid);
            } else {
                ipv = 4;
                url = pa_sprintf_malloc("rtsp://%s/%s", ip, c->sid);
            }
            pa_rtsp_set_url(c->rtsp, url);

            if (c->protocol == PA_RAOP_PROTOCOL_TCP)
                frames = FRAMES_PER_TCP_PACKET;
            else if (c->protocol == PA_RAOP_PROTOCOL_UDP)
                frames = FRAMES_PER_UDP_PACKET;

            switch(c->encryption) {
                case PA_RAOP_ENCRYPTION_NONE: {
                    sdp = pa_sprintf_malloc(
                        "v=0\r\n"
                        "o=iTunes %s 0 IN IP%d %s\r\n"
                        "s=iTunes\r\n"
                        "c=IN IP%d %s\r\n"
                        "t=0 0\r\n"
                        "m=audio 0 RTP/AVP 96\r\n"
                        "a=rtpmap:96 AppleLossless\r\n"
                        "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n",
                        c->sid, ipv, ip, ipv, c->host, frames);

                    break;
                }

                case PA_RAOP_ENCRYPTION_RSA:
                case PA_RAOP_ENCRYPTION_FAIRPLAY:
                case PA_RAOP_ENCRYPTION_MFISAP:
                case PA_RAOP_ENCRYPTION_FAIRPLAY_SAP25: {
                    key = pa_raop_secret_get_key(c->secret);
                    if (!key) {
                        pa_log("pa_raop_secret_get_key() failed.");
                        pa_rtsp_disconnect(rtsp);
                        /* FIXME: This is an unrecoverable failure. We should notify
                         * the pa_raop_client owner so that it could shut itself
                         * down. */
                        goto connect_finish;
                    }

                    iv = pa_raop_secret_get_iv(c->secret);

                    sdp = pa_sprintf_malloc(
                        "v=0\r\n"
                        "o=iTunes %s 0 IN IP%d %s\r\n"
                        "s=iTunes\r\n"
                        "c=IN IP%d %s\r\n"
                        "t=0 0\r\n"
                        "m=audio 0 RTP/AVP 96\r\n"
                        "a=rtpmap:96 AppleLossless\r\n"
                        "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n"
                        "a=rsaaeskey:%s\r\n"
                        "a=aesiv:%s\r\n",
                        c->sid, ipv, ip, ipv, c->host, frames, key, iv);

                    pa_xfree(key);
                    pa_xfree(iv);
                    break;
                }
            }

            pa_rtsp_announce(c->rtsp, sdp);

connect_finish:
            pa_xfree(sdp);
            pa_xfree(url);
            break;
        }

        case STATE_OPTIONS: {
            pa_log_debug("RAOP: OPTIONS (stream cb)");

            break;
        }

        case STATE_ANNOUNCE: {
            uint16_t cport = DEFAULT_UDP_CONTROL_PORT;
            uint16_t tport = DEFAULT_UDP_TIMING_PORT;
            char *trs = NULL;

            pa_log_debug("RAOP: ANNOUNCE");

            if (c->protocol == PA_RAOP_PROTOCOL_TCP) {
                trs = pa_sprintf_malloc(
                    "RTP/AVP/TCP;unicast;interleaved=0-1;mode=record");
            } else if (c->protocol == PA_RAOP_PROTOCOL_UDP) {
                c->udp_cfd = open_bind_udp_socket(c, &cport);
                c->udp_tfd  = open_bind_udp_socket(c, &tport);
                if (c->udp_cfd < 0 || c->udp_tfd < 0)
                    goto annonce_error;

                trs = pa_sprintf_malloc(
                    "RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
                    "control_port=%d;timing_port=%d",
                    cport, tport);
            }

            pa_rtsp_setup(c->rtsp, trs);

            pa_xfree(trs);
            break;

        annonce_error:
            if (c->udp_cfd >= 0)
                pa_close(c->udp_cfd);
            c->udp_cfd = -1;
            if (c->udp_tfd >= 0)
                pa_close(c->udp_tfd);
            c->udp_tfd = -1;

            pa_rtsp_client_free(c->rtsp);

            pa_log_error("Aborting RTSP announce, failed creating required sockets");

            c->rtsp = NULL;
            pa_xfree(trs);
            break;
        }

        case STATE_SETUP: {
            pa_socket_client *sc = NULL;
            uint32_t sport = DEFAULT_UDP_AUDIO_PORT;
            uint32_t cport =0, tport = 0;
            char *ajs, *token, *pc, *trs;
            const char *token_state = NULL;
            char delimiters[] = ";";

            pa_log_debug("RAOP: SETUP");

            ajs = pa_xstrdup(pa_headerlist_gets(headers, "Audio-Jack-Status"));

            if (ajs) {
                c->jack_type = JACK_TYPE_ANALOG;
                c->jack_status = JACK_STATUS_DISCONNECTED;

                while ((token = pa_split(ajs, delimiters, &token_state))) {
                    if ((pc = strstr(token, "="))) {
                      *pc = 0;
                      if (pa_streq(token, "type") && pa_streq(pc + 1, "digital"))
                          c->jack_type = JACK_TYPE_DIGITAL;
                    } else {
                        if (pa_streq(token, "connected"))
                            c->jack_status = JACK_STATUS_CONNECTED;
                    }
                    pa_xfree(token);
                }

            } else {
                pa_log_warn("\"Audio-Jack-Status\" missing in RTSP setup response");
            }

            sport = pa_rtsp_serverport(c->rtsp);
            if (sport <= 0)
                goto setup_error;

            token_state = NULL;
            if (c->protocol == PA_RAOP_PROTOCOL_TCP) {
                if (!(sc = pa_socket_client_new_string(c->core->mainloop, true, c->host, sport)))
                    goto setup_error;

                pa_socket_client_ref(sc);
                pa_socket_client_set_callback(sc, tcp_connection_cb, c);

                pa_socket_client_unref(sc);
                sc = NULL;
            } else if (c->protocol == PA_RAOP_PROTOCOL_UDP) {
                trs = pa_xstrdup(pa_headerlist_gets(headers, "Transport"));

                if (trs) {
                    /* Now parse out the server port component of the response. */
                    while ((token = pa_split(trs, delimiters, &token_state))) {
                        if ((pc = strstr(token, "="))) {
                            *pc = 0;
                            if (pa_streq(token, "control_port")) {
                                if (pa_atou(pc + 1, &cport) < 0)
                                    goto setup_error_parse;
                            }
                            if (pa_streq(token, "timing_port")) {
                                if (pa_atou(pc + 1, &tport) < 0)
                                    goto setup_error_parse;
                            }
                            *pc = '=';
                        }
                        pa_xfree(token);
                    }
                    pa_xfree(trs);
                } else {
                    pa_log_warn("\"Transport\" missing in RTSP setup response");
                }

                if (cport <= 0 || tport <= 0)
                    goto setup_error;

                if ((c->udp_sfd = connect_udp_socket(c, -1, sport)) <= 0)
                    goto setup_error;
                if ((c->udp_cfd = connect_udp_socket(c, c->udp_cfd, cport)) <= 0)
                    goto setup_error;
                if ((c->udp_tfd = connect_udp_socket(c, c->udp_tfd, tport)) <= 0)
                    goto setup_error;

                pa_log_debug("Connection established (UDP;control_port=%d;timing_port=%d)", cport, tport);

                /* Send an initial UDP packet so a connection tracking firewall
                 * knows the src_ip:src_port <-> dest_ip:dest_port relation
                 * and accepts the incoming timing packets.
                 */
                send_initial_udp_timing_packet(c);
                pa_log_debug("Sent initial timing packet to UDP port %d", tport);

                if (c->state_callback)
                    c->state_callback(PA_RAOP_CONNECTED, c->state_userdata);
            }

            pa_rtsp_record(c->rtsp, &c->seq, &c->rtptime);

            pa_xfree(ajs);
            break;

        setup_error_parse:
            pa_log("Failed parsing server port components");
            pa_xfree(token);
            pa_xfree(trs);
            /* fall-thru */
        setup_error:
            if (c->tcp_sfd >= 0)
                pa_close(c->tcp_sfd);
            c->tcp_sfd = -1;

            if (c->udp_sfd >= 0)
                pa_close(c->udp_sfd);
            c->udp_sfd = -1;

            c->udp_cfd = c->udp_tfd = -1;

            pa_rtsp_client_free(c->rtsp);

            pa_log_error("aborting RTSP setup, failed creating required sockets");

            if (c->state_callback)
                c->state_callback(PA_RAOP_DISCONNECTED, c->state_userdata);

            c->rtsp = NULL;
            break;
        }

        case STATE_RECORD: {
            int32_t latency = 0;
            uint32_t ssrc;
            char *alt;

            pa_log_debug("RAOP: RECORD");

            alt = pa_xstrdup(pa_headerlist_gets(headers, "Audio-Latency"));
            if (alt) {
                if (pa_atoi(alt, &latency) < 0)
                    pa_log("Failed to parse audio latency");
            }

            pa_raop_packet_buffer_reset(c->pbuf, c->seq);

            pa_random(&ssrc, sizeof(ssrc));
            c->is_first_packet = true;
            c->is_recording = true;
            c->sync_count = 0;
            c->ssrc = ssrc;

            if (c->state_callback)
                c->state_callback((int) PA_RAOP_RECORDING, c->state_userdata);

            pa_xfree(alt);
            break;
        }

        case STATE_SET_PARAMETER: {
            pa_log_debug("RAOP: SET_PARAMETER");

            break;
        }

        case STATE_FLUSH: {
            pa_log_debug("RAOP: FLUSHED");

            break;
        }

        case STATE_TEARDOWN: {
            pa_log_debug("RAOP: TEARDOWN");

            if (c->tcp_sfd >= 0)
                pa_close(c->tcp_sfd);
            c->tcp_sfd = -1;

            if (c->udp_sfd >= 0)
                pa_close(c->udp_sfd);
            c->udp_sfd = -1;

            /* Polling sockets will be closed by sink */
            c->udp_cfd = c->udp_tfd = -1;
            c->tcp_sfd = -1;

            pa_rtsp_client_free(c->rtsp);
            pa_xfree(c->sid);
            c->rtsp = NULL;
            c->sid = NULL;

            if (c->state_callback)
                c->state_callback(PA_RAOP_DISCONNECTED, c->state_userdata);

            break;
        }

        case STATE_DISCONNECTED: {
            pa_log_debug("RAOP: DISCONNECTED");

            c->is_recording = false;

            if (c->tcp_sfd >= 0)
                pa_close(c->tcp_sfd);
            c->tcp_sfd = -1;

            if (c->udp_sfd >= 0)
                pa_close(c->udp_sfd);
            c->udp_sfd = -1;

            /* Polling sockets will be closed by sink */
            c->udp_cfd = c->udp_tfd = -1;
            c->tcp_sfd = -1;

            pa_log_error("RTSP control channel closed (disconnected)");

            pa_rtsp_client_free(c->rtsp);
            pa_xfree(c->sid);
            c->rtsp = NULL;
            c->sid = NULL;

            if (c->state_callback)
                c->state_callback((int) PA_RAOP_DISCONNECTED, c->state_userdata);

            break;
        }
    }
}

static void rtsp_auth_cb(pa_rtsp_client *rtsp, pa_rtsp_state_t state, pa_rtsp_status_t status, pa_headerlist *headers, void *userdata) {
    pa_raop_client *c = userdata;

    pa_assert(c);
    pa_assert(rtsp);
    pa_assert(rtsp == c->rtsp);

    switch (state) {
        case STATE_CONNECT: {
            char *sci = NULL, *sac = NULL;
            uint8_t rac[APPLE_CHALLENGE_LENGTH];
            struct {
                uint32_t ci1;
                uint32_t ci2;
            } rci;

            pa_random(&rci, sizeof(rci));
            /* Generate a random Client-Instance number */
            sci = pa_sprintf_malloc("%08x%08x",rci.ci1, rci.ci2);
            pa_rtsp_add_header(c->rtsp, "Client-Instance", sci);

            pa_random(rac, APPLE_CHALLENGE_LENGTH);
            /* Generate a random Apple-Challenge key */
            pa_raop_base64_encode(rac, APPLE_CHALLENGE_LENGTH, &sac);
            rtrim_char(sac, '=');
            pa_rtsp_add_header(c->rtsp, "Apple-Challenge", sac);

            pa_rtsp_options(c->rtsp);

            pa_xfree(sac);
            pa_xfree(sci);
            break;
        }

        case STATE_OPTIONS: {
            static bool waiting = false;
            const char *current = NULL;
            char space[] = " ";
            char *token, *ath = NULL;
            char *publ, *wath, *mth = NULL, *val;
            char *realm = NULL, *nonce = NULL, *response = NULL;
            char comma[] = ",";

            pa_log_debug("RAOP: OPTIONS (auth cb)");
            /* We do not consider the Apple-Response */
            pa_rtsp_remove_header(c->rtsp, "Apple-Challenge");

            if (STATUS_UNAUTHORIZED == status) {
                wath = pa_xstrdup(pa_headerlist_gets(headers, "WWW-Authenticate"));
                if (true == waiting) {
                    pa_xfree(wath);
                    goto fail;
                }

                if (wath) {
                    mth = pa_split(wath, space, &current);
                    while ((token = pa_split(wath, comma, &current))) {
                        if ((val = strstr(token, "="))) {
                            if (NULL == realm && val > strstr(token, "realm"))
                                realm = pa_xstrdup(val + 2);
                            else if (NULL == nonce && val > strstr(token, "nonce"))
                                nonce = pa_xstrdup(val + 2);
                        }

                        pa_xfree(token);
                    }
                }

                if (pa_safe_streq(mth, "Basic") && realm) {
                    rtrim_char(realm, '\"');

                    pa_raop_basic_response(DEFAULT_USER_NAME, c->password, &response);
                    ath = pa_sprintf_malloc("Basic %s",
                        response);
                } else if (pa_safe_streq(mth, "Digest") && realm && nonce) {
                    rtrim_char(realm, '\"');
                    rtrim_char(nonce, '\"');

                    pa_raop_digest_response(DEFAULT_USER_NAME, realm, c->password, nonce, "*", &response);
                    ath = pa_sprintf_malloc("Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"*\", response=\"%s\"",
                        DEFAULT_USER_NAME, realm, nonce,
                        response);
                } else {
                    pa_log_error("unsupported authentication method: %s", mth);
                    pa_xfree(realm);
                    pa_xfree(nonce);
                    pa_xfree(wath);
                    pa_xfree(mth);
                    goto error;
                }

                pa_xfree(response);
                pa_xfree(realm);
                pa_xfree(nonce);
                pa_xfree(wath);
                pa_xfree(mth);

                pa_rtsp_add_header(c->rtsp, "Authorization", ath);
                pa_xfree(ath);

                waiting = true;
                pa_rtsp_options(c->rtsp);
                break;
            }

            if (STATUS_OK == status) {
                publ = pa_xstrdup(pa_headerlist_gets(headers, "Public"));
                c->sci = pa_xstrdup(pa_rtsp_get_header(c->rtsp, "Client-Instance"));

                if (c->password)
                    pa_xfree(c->password);
                pa_xfree(publ);
                c->password = NULL;
            }

            pa_rtsp_client_free(c->rtsp);
            c->rtsp = NULL;
            /* Ensure everything is cleaned before calling the callback, otherwise it may raise a crash */
            if (c->state_callback)
                c->state_callback((int) PA_RAOP_AUTHENTICATED, c->state_userdata);

            waiting = false;
            break;

        fail:
            if (c->state_callback)
                c->state_callback((int) PA_RAOP_DISCONNECTED, c->state_userdata);
            pa_rtsp_client_free(c->rtsp);
            c->rtsp = NULL;

            pa_log_error("aborting authentication, wrong password");

            waiting = false;
            break;

        error:
            if (c->state_callback)
                c->state_callback((int) PA_RAOP_DISCONNECTED, c->state_userdata);
            pa_rtsp_client_free(c->rtsp);
            c->rtsp = NULL;

            pa_log_error("aborting authentication, unexpected failure");

            waiting = false;
            break;
        }

        case STATE_ANNOUNCE:
        case STATE_SETUP:
        case STATE_RECORD:
        case STATE_SET_PARAMETER:
        case STATE_FLUSH:
        case STATE_TEARDOWN:
        case STATE_DISCONNECTED:
        default: {
            if (c->state_callback)
                c->state_callback((int) PA_RAOP_DISCONNECTED, c->state_userdata);
            pa_rtsp_client_free(c->rtsp);
            c->rtsp = NULL;

            if (c->sci)
                pa_xfree(c->sci);
            c->sci = NULL;

            break;
        }
    }
}


void pa_raop_client_disconnect(pa_raop_client *c) {
    c->is_recording = false;

    if (c->tcp_sfd >= 0)
        pa_close(c->tcp_sfd);
    c->tcp_sfd = -1;

    if (c->udp_sfd >= 0)
        pa_close(c->udp_sfd);
    c->udp_sfd = -1;

    /* Polling sockets will be closed by sink */
    c->udp_cfd = c->udp_tfd = -1;
    c->tcp_sfd = -1;

    pa_log_error("RTSP control channel closed (disconnected)");

    if (c->rtsp)
        pa_rtsp_client_free(c->rtsp);
    if (c->sid)
        pa_xfree(c->sid);
    c->rtsp = NULL;
    c->sid = NULL;

    if (c->state_callback)
        c->state_callback((int) PA_RAOP_DISCONNECTED, c->state_userdata);

}


pa_raop_client* pa_raop_client_new(pa_core *core, const char *host, pa_raop_protocol_t protocol,
                                   pa_raop_encryption_t encryption, pa_raop_codec_t codec, bool autoreconnect) {
    pa_raop_client *c;

    pa_parsed_address a;
    pa_sample_spec ss;
    size_t size = 2;

    pa_assert(core);
    pa_assert(host);

    if (pa_parse_address(host, &a) < 0)
        return NULL;

    if (a.type == PA_PARSED_ADDRESS_UNIX) {
        pa_xfree(a.path_or_host);
        return NULL;
    }

    c = pa_xnew0(pa_raop_client, 1);
    c->core = core;
    c->host = a.path_or_host; /* Will eventually be freed on destruction of c */
    if (a.port > 0)
        c->port = a.port;
    else
        c->port = DEFAULT_RAOP_PORT;
    c->rtsp = NULL;
    c->sci = c->sid = NULL;
    c->password = NULL;
    c->autoreconnect = autoreconnect;

    c->protocol = protocol;
    c->encryption = encryption;
    c->codec = codec;

    c->tcp_sfd = -1;

    c->udp_sfd = -1;
    c->udp_cfd = -1;
    c->udp_tfd = -1;

    c->secret = NULL;
    if (c->encryption != PA_RAOP_ENCRYPTION_NONE)
        c->secret = pa_raop_secret_new();

    ss = core->default_sample_spec;
    if (c->protocol == PA_RAOP_PROTOCOL_UDP)
        size = RTX_BUFFERING_SECONDS * ss.rate / FRAMES_PER_UDP_PACKET;

    c->is_recording = false;
    c->is_first_packet = true;
    /* Packet sync interval should be around 1s (UDP only) */
    c->sync_interval = ss.rate / FRAMES_PER_UDP_PACKET;
    c->sync_count = 0;

    c->pbuf = pa_raop_packet_buffer_new(c->core->mempool, size);

    return c;
}

void pa_raop_client_free(pa_raop_client *c) {
    pa_assert(c);

    pa_raop_packet_buffer_free(c->pbuf);

    pa_xfree(c->sid);
    pa_xfree(c->sci);
    if (c->secret)
        pa_raop_secret_free(c->secret);
    pa_xfree(c->password);
    c->sci = c->sid = NULL;
    c->password = NULL;
    c->secret = NULL;

    if (c->rtsp)
        pa_rtsp_client_free(c->rtsp);
    c->rtsp = NULL;

    pa_xfree(c->host);
    pa_xfree(c);
}

int pa_raop_client_authenticate (pa_raop_client *c, const char *password) {
    int rv = 0;

    pa_assert(c);

    if (c->rtsp || c->password) {
        pa_log_debug("Authentication/Connection already in progress...");
        return 0;
    }

    c->password = NULL;
    if (password)
        c->password = pa_xstrdup(password);
    c->rtsp = pa_rtsp_client_new(c->core->mainloop, c->host, c->port, DEFAULT_USER_AGENT, c->autoreconnect);

    pa_assert(c->rtsp);

    pa_rtsp_set_callback(c->rtsp, rtsp_auth_cb, c);
    rv = pa_rtsp_connect(c->rtsp);
    return rv;
}

bool pa_raop_client_is_authenticated(pa_raop_client *c) {
    pa_assert(c);

    return (c->sci != NULL);
}

int pa_raop_client_announce(pa_raop_client *c) {
    uint32_t sid;
    int rv = 0;

    pa_assert(c);

    if (c->rtsp) {
        pa_log_debug("Connection already in progress...");
        return 0;
    } else if (!c->sci) {
        pa_log_debug("ANNOUNCE requires a preliminary authentication");
        return 1;
    }

    c->rtsp = pa_rtsp_client_new(c->core->mainloop, c->host, c->port, DEFAULT_USER_AGENT, c->autoreconnect);

    pa_assert(c->rtsp);

    c->sync_count = 0;
    c->is_recording = false;
    c->is_first_packet = true;
    pa_random(&sid, sizeof(sid));
    c->sid = pa_sprintf_malloc("%u", sid);
    pa_rtsp_set_callback(c->rtsp, rtsp_stream_cb, c);

    rv = pa_rtsp_connect(c->rtsp);
    return rv;
}

bool pa_raop_client_is_alive(pa_raop_client *c) {
    pa_assert(c);

    if (!c->rtsp || !c->sci) {
        pa_log_debug("Not alive, connection not established yet...");
        return false;
    }

    switch (c->protocol) {
        case PA_RAOP_PROTOCOL_TCP:
            if (c->tcp_sfd >= 0)
                return true;
            break;
        case PA_RAOP_PROTOCOL_UDP:
            if (c->udp_sfd >= 0)
                return true;
            break;
        default:
            break;
    }

    return false;
}

bool pa_raop_client_can_stream(pa_raop_client *c) {
    pa_assert(c);

    if (!c->rtsp || !c->sci) {
        return false;
    }

    switch (c->protocol) {
        case PA_RAOP_PROTOCOL_TCP:
            if (c->tcp_sfd >= 0 && c->is_recording)
                return true;
            break;
        case PA_RAOP_PROTOCOL_UDP:
            if (c->udp_sfd >= 0 && c->is_recording)
                return true;
            break;
        default:
            break;
    }

    return false;
}

bool pa_raop_client_is_recording(pa_raop_client *c) {
    return c->is_recording;
}

int pa_raop_client_stream(pa_raop_client *c) {
    int rv = 0;

    pa_assert(c);

    if (!c->rtsp || !c->sci) {
        pa_log_debug("Streaming's impossible, connection not established yet...");
        return 0;
    }

    switch (c->protocol) {
        case PA_RAOP_PROTOCOL_TCP:
            if (c->tcp_sfd >= 0 && !c->is_recording) {
                c->is_recording = true;
                c->is_first_packet = true;
                c->sync_count = 0;
            }
            break;
        case PA_RAOP_PROTOCOL_UDP:
            if (c->udp_sfd >= 0 && !c->is_recording) {
                c->is_recording = true;
                c->is_first_packet = true;
                c->sync_count = 0;
            }
            break;
        default:
            rv = 1;
            break;
    }

    return rv;
}

int pa_raop_client_set_volume(pa_raop_client *c, pa_volume_t volume) {
    char *param;
    int rv = 0;
    double db;

    pa_assert(c);

    if (!c->rtsp) {
        pa_log_debug("Cannot SET_PARAMETER, connection not established yet...");
        return 0;
    } else if (!c->sci) {
        pa_log_debug("SET_PARAMETER requires a preliminary authentication");
        return 1;
    }

    db = pa_sw_volume_to_dB(volume);
    if (db < VOLUME_MIN)
        db = VOLUME_MIN;
    else if (db > VOLUME_MAX)
        db = VOLUME_MAX;

    pa_log_debug("volume=%u db=%.6f", volume, db);

    param = pa_sprintf_malloc("volume: %0.6f\r\n", db);
    /* We just hit and hope, cannot wait for the callback. */
    if (c->rtsp != NULL && pa_rtsp_exec_ready(c->rtsp))
        rv = pa_rtsp_setparameter(c->rtsp, param);

    pa_xfree(param);
    return rv;
}

int pa_raop_client_flush(pa_raop_client *c) {
    int rv = 0;

    pa_assert(c);

    if (!c->rtsp || !pa_rtsp_exec_ready(c->rtsp)) {
        pa_log_debug("Cannot FLUSH, connection not established yet...)");
        return 0;
    } else if (!c->sci) {
        pa_log_debug("FLUSH requires a preliminary authentication");
        return 1;
    }

    c->is_recording = false;

    rv = pa_rtsp_flush(c->rtsp, c->seq, c->rtptime);
    return rv;
}

int pa_raop_client_teardown(pa_raop_client *c) {
    int rv = 0;

    pa_assert(c);

    if (!c->rtsp) {
        pa_log_debug("Cannot TEARDOWN, connection not established yet...");
        return 0;
    } else if (!c->sci) {
        pa_log_debug("TEARDOWN requires a preliminary authentication");
        return 1;
    }

    c->is_recording = false;

    rv = pa_rtsp_teardown(c->rtsp);
    return rv;
}

void pa_raop_client_get_frames_per_block(pa_raop_client *c, size_t *frames) {
    pa_assert(c);
    pa_assert(frames);

    switch (c->protocol) {
        case PA_RAOP_PROTOCOL_TCP:
            *frames = FRAMES_PER_TCP_PACKET;
            break;
        case PA_RAOP_PROTOCOL_UDP:
            *frames = FRAMES_PER_UDP_PACKET;
            break;
        default:
            *frames = 0;
            break;
    }
}

bool pa_raop_client_register_pollfd(pa_raop_client *c, pa_rtpoll *poll, pa_rtpoll_item **poll_item) {
    struct pollfd *pollfd = NULL;
    pa_rtpoll_item *item = NULL;
    bool oob = true;

    pa_assert(c);
    pa_assert(poll);
    pa_assert(poll_item);

    switch (c->protocol) {
        case PA_RAOP_PROTOCOL_TCP:
            item = pa_rtpoll_item_new(poll, PA_RTPOLL_NEVER, 1);
            pollfd = pa_rtpoll_item_get_pollfd(item, NULL);
            pollfd->fd = c->tcp_sfd;
            pollfd->events = POLLOUT;
            pollfd->revents = 0;
            *poll_item = item;
            oob = false;
            break;
        case PA_RAOP_PROTOCOL_UDP:
            item = pa_rtpoll_item_new(poll, PA_RTPOLL_NEVER, 2);
            pollfd = pa_rtpoll_item_get_pollfd(item, NULL);
            pollfd->fd = c->udp_cfd;
            pollfd->events = POLLIN | POLLPRI;
            pollfd->revents = 0;
            pollfd++;
            pollfd->fd = c->udp_tfd;
            pollfd->events = POLLIN | POLLPRI;
            pollfd->revents = 0;
            *poll_item = item;
            oob = true;
            break;
        default:
            *poll_item = NULL;
            break;
    }

    return oob;
}

bool pa_raop_client_is_timing_fd(pa_raop_client *c, const int fd) {
    return fd == c->udp_tfd;
}

pa_volume_t pa_raop_client_adjust_volume(pa_raop_client *c, pa_volume_t volume) {
    double minv, maxv;

    pa_assert(c);

    if (c->protocol != PA_RAOP_PROTOCOL_UDP)
        return volume;

    maxv = pa_sw_volume_from_dB(0.0);
    minv = maxv * pow(10.0, VOLUME_DEF / 60.0);

    /* Adjust volume so that it fits into VOLUME_DEF <= v <= 0 dB */
    return volume - volume * (minv / maxv) + minv;
}

void pa_raop_client_handle_oob_packet(pa_raop_client *c, const int fd, const uint8_t packet[], ssize_t size) {
    pa_assert(c);
    pa_assert(fd >= 0);
    pa_assert(packet);

    if (c->protocol == PA_RAOP_PROTOCOL_UDP) {
        if (fd == c->udp_cfd) {
            pa_log_debug("Received UDP control packet...");
            handle_udp_control_packet(c, packet, size);
        } else if (fd == c->udp_tfd) {
            pa_log_debug("Received UDP timing packet...");
            handle_udp_timing_packet(c, packet, size);
        }
    }
}

ssize_t pa_raop_client_send_audio_packet(pa_raop_client *c, pa_memchunk *block, size_t offset) {
    ssize_t written = 0;

    pa_assert(c);
    pa_assert(block);

    /* Sync RTP & NTP timestamp if required (UDP). */
    if (c->protocol == PA_RAOP_PROTOCOL_UDP) {
        c->sync_count++;
        if (c->is_first_packet || c->sync_count >= c->sync_interval) {
            send_udp_sync_packet(c, c->rtptime);
            c->sync_count = 0;
        }
    }

    switch (c->protocol) {
        case PA_RAOP_PROTOCOL_TCP:
            written = send_tcp_audio_packet(c, block, offset);
            break;
        case PA_RAOP_PROTOCOL_UDP:
            written = send_udp_audio_packet(c, block, offset);
            break;
        default:
            written = -1;
            break;
    }

    c->is_first_packet = false;
    return written;
}

void pa_raop_client_set_state_callback(pa_raop_client *c, pa_raop_client_state_cb_t callback, void *userdata) {
    pa_assert(c);

    c->state_callback = callback;
    c->state_userdata = userdata;
}
