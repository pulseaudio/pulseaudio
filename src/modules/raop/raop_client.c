/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

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
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <math.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

/* TODO: Replace OpenSSL with NSS */
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/aes.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/sample.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/arpa-inet.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/log.h>
#include <pulsecore/parseaddr.h>
#include <pulsecore/macro.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/random.h>

#include "raop_client.h"
#include "rtsp_client.h"
#include "base64.h"

#include "raop_packet_buffer.h"

#define AES_CHUNKSIZE 16

#define JACK_STATUS_DISCONNECTED 0
#define JACK_STATUS_CONNECTED 1

#define JACK_TYPE_ANALOG 0
#define JACK_TYPE_DIGITAL 1

#define VOLUME_DEF -30
#define VOLUME_MIN -144
#define VOLUME_MAX 0

#define DEFAULT_RAOP_PORT 5000
#define UDP_DEFAULT_AUDIO_PORT 6000
#define UDP_DEFAULT_CONTROL_PORT 6001
#define UDP_DEFAULT_TIMING_PORT 6002

#define UDP_DEFAULT_PKT_BUF_SIZE 1000

typedef enum {
    UDP_PAYLOAD_TIMING_REQUEST = 0x52,
    UDP_PAYLOAD_TIMING_RESPONSE = 0x53,
    UDP_PAYLOAD_SYNCHRONIZATION = 0x54,
    UDP_PAYLOAD_RETRANSMIT_REQUEST = 0x55,
    UDP_PAYLOAD_RETRANSMIT_REPLY = 0x56,
    UDP_PAYLOAD_AUDIO_DATA = 0x60
} pa_raop_udp_payload_type;

/* Openssl 1.1.0 broke compatibility. Before 1.1.0 we had to set RSA->n and
 * RSA->e manually, but after 1.1.0 the RSA struct is opaque and we have to use
 * RSA_set0_key(). RSA_set0_key() is a new function added in 1.1.0. We could
 * depend on openssl 1.1.0, but it may take some time before distributions will
 * be able to upgrade to the new openssl version. To insulate ourselves from
 * such transition problems, let's implement RSA_set0_key() ourselves if it's
 * not available. */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
static int RSA_set0_key(RSA *r, BIGNUM *n, BIGNUM *e, BIGNUM *d) {
    r->n = n;
    r->e = e;
    return 1;
}
#endif

struct pa_raop_client {
    pa_core *core;
    char *host;
    uint16_t port;
    char *sid;
    pa_rtsp_client *rtsp;
    pa_raop_protocol_t protocol;

    uint8_t jack_type;
    uint8_t jack_status;

    /* Encryption Related bits */
    int encryption; /* Enable encryption? */
    AES_KEY aes;
    uint8_t aes_iv[AES_CHUNKSIZE]; /* Initialization vector for aes-cbc */
    uint8_t aes_nv[AES_CHUNKSIZE]; /* Next vector for aes-cbc */
    uint8_t aes_key[AES_CHUNKSIZE]; /* Key for aes-cbc */

    uint16_t seq;
    uint32_t rtptime;

    /* Members only for the TCP protocol */
    pa_socket_client *tcp_sc;
    int tcp_fd;

    pa_raop_client_cb_t tcp_callback;
    void *tcp_userdata;
    pa_raop_client_closed_cb_t tcp_closed_callback;
    void *tcp_closed_userdata;

    /* Members only for the UDP protocol */
    uint16_t udp_my_control_port;
    uint16_t udp_my_timing_port;
    uint16_t udp_server_control_port;
    uint16_t udp_server_timing_port;

    int udp_stream_fd;
    int udp_control_fd;
    int udp_timing_fd;

    uint32_t udp_ssrc;

    bool udp_first_packet;
    uint32_t udp_sync_interval;
    uint32_t udp_sync_count;

    pa_raop_client_setup_cb_t udp_setup_callback;
    void *udp_setup_userdata;

    pa_raop_client_record_cb_t udp_record_callback;
    void *udp_record_userdata;

    pa_raop_client_disconnected_cb_t udp_disconnected_callback;
    void *udp_disconnected_userdata;

    pa_raop_packet_buffer *packet_buffer;
};

/* Timming packet header (8x8):
 *  [0]   RTP v2: 0x80,
 *  [1]   Payload type: 0x53 | marker bit: 0x80,
 *  [2,3] Sequence number: 0x0007,
 *  [4,7] Timestamp: 0x00000000 (unused). */
static const uint8_t udp_timming_header[8] = {
    0x80, 0xd3, 0x00, 0x07,
    0x00, 0x00, 0x00, 0x00
};

/* Sync packet header (8x8):
 *  [0]   RTP v2: 0x80,
 *  [1]   Payload type: 0x54 | marker bit: 0x80,
 *  [2,3] Sequence number: 0x0007,
 *  [4,7] Timestamp: 0x00000000 (to be set). */
static const uint8_t udp_sync_header[8] = {
    0x80, 0xd4, 0x00, 0x07,
    0x00, 0x00, 0x00, 0x00
};

static const uint8_t tcp_audio_header[16] = {
    0x24, 0x00, 0x00, 0x00,
    0xF0, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/* Audio packet header (12x8):
 *  [0]    RTP v2: 0x80,
 *  [1]    Payload type: 0x60,
 *  [2,3]  Sequence number: 0x0000 (to be set),
 *  [4,7]  Timestamp: 0x00000000 (to be set),
 *  [8,12] SSRC: 0x00000000 (to be set).*/
static const uint8_t udp_audio_header[12] = {
    0x80, 0x60, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/**
 * Function to write bits into a buffer.
 * @param buffer Handle to the buffer. It will be incremented if new data requires it.
 * @param bit_pos A pointer to a position buffer to keep track the current write location (0 for MSB, 7 for LSB)
 * @param size A pointer to the byte size currently written. This allows the calling function to do simple buffer overflow checks
 * @param data The data to write
 * @param data_bit_len The number of bits from data to write
 */
static inline void bit_writer(uint8_t **buffer, uint8_t *bit_pos, int *size, uint8_t data, uint8_t data_bit_len) {
    int bits_left, bit_overflow;
    uint8_t bit_data;

    if (!data_bit_len)
        return;

    /* If bit pos is zero, we will definately use at least one bit from the current byte so size increments. */
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

static int rsa_encrypt(uint8_t *text, int len, uint8_t *res) {
    const char n[] =
        "59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUtwC"
        "5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDR"
        "KSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuB"
        "OitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJ"
        "Q+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEpVviYnh"
        "imNVvYFZeCXg/IdTQ+x4IRdiXNv5hEew==";
    const char e[] = "AQAB";
    uint8_t modules[256];
    uint8_t exponent[8];
    int size;
    RSA *rsa;
    BIGNUM *n_bn;
    BIGNUM *e_bn;

    rsa = RSA_new();
    size = pa_base64_decode(n, modules);
    n_bn = BN_bin2bn(modules, size, NULL);
    size = pa_base64_decode(e, exponent);
    e_bn = BN_bin2bn(exponent, size, NULL);
    RSA_set0_key(rsa, n_bn, e_bn, NULL);

    size = RSA_public_encrypt(len, text, res, rsa, RSA_PKCS1_OAEP_PADDING);
    RSA_free(rsa);
    return size;
}

static int aes_encrypt(pa_raop_client *c, uint8_t *data, int size) {
    uint8_t *buf;
    int i=0, j;

    pa_assert(c);

    memcpy(c->aes_nv, c->aes_iv, AES_CHUNKSIZE);
    while (i+AES_CHUNKSIZE <= size) {
        buf = data + i;
        for (j=0; j<AES_CHUNKSIZE; ++j)
            buf[j] ^= c->aes_nv[j];

        AES_encrypt(buf, buf, &c->aes);
        memcpy(c->aes_nv, buf, AES_CHUNKSIZE);
        i += AES_CHUNKSIZE;
    }
    return i;
}

static inline void rtrimchar(char *str, char rc) {
    char *sp = str + strlen(str) - 1;
    while (sp >= str && *sp == rc) {
        *sp = '\0';
        sp -= 1;
    }
}

static void tcp_on_connection(pa_socket_client *sc, pa_iochannel *io, void *userdata) {
    pa_raop_client *c = userdata;

    pa_assert(sc);
    pa_assert(c);
    pa_assert(c->tcp_sc == sc);
    pa_assert(c->tcp_fd < 0);
    pa_assert(c->tcp_callback);

    pa_socket_client_unref(c->tcp_sc);
    c->tcp_sc = NULL;

    if (!io) {
        pa_log("Connection failed: %s", pa_cstrerror(errno));
        return;
    }

    c->tcp_fd = pa_iochannel_get_send_fd(io);

    pa_iochannel_set_noclose(io, true);
    pa_iochannel_free(io);

    pa_make_tcp_socket_low_delay(c->tcp_fd);

    pa_log_debug("Connection established");
    c->tcp_callback(c->tcp_fd, c->tcp_userdata);
}

static inline uint64_t timeval_to_ntp(struct timeval *tv) {
    uint64_t ntp = 0;

    /* Converting micro seconds to a fraction. */
    ntp = (uint64_t) tv->tv_usec * UINT32_MAX / PA_USEC_PER_SEC;
    /* Moving reference from  1 Jan 1970 to 1 Jan 1900 (seconds). */
    ntp |= (uint64_t) (tv->tv_sec + 0x83aa7e80) << 32;

    return ntp;
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
        sa = (struct sockaddr *) &sa4;
        salen = sizeof(sa4);
        sa_port = &sa4.sin_port;
#ifdef HAVE_IPV6
    } else if (inet_pton(AF_INET6, pa_rtsp_localip(c->rtsp), &sa6.sin6_addr) > 0) {
        sa6.sin6_family = af = AF_INET6;
        sa6.sin6_port = htons(port);
        sa = (struct sockaddr *) &sa6;
        salen = sizeof(sa6);
        sa_port = &sa6.sin6_port;
#endif
    } else {
        pa_log("Could not determine which address family to use");
        goto fail;
    }

    pa_zero(sa4);
#ifdef HAVE_IPV6
    pa_zero(sa6);
#endif

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
        *sa_port = htons(port);

        if (bind(fd, sa, salen) < 0 && errno != EADDRINUSE) {
            pa_log("bind_socket() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
        break;
    } while (++port > 0);

    pa_log_debug("Socket bound to port %d (SOCK_DGRAM)", port);
    *actual_port = port;

    return fd;

fail:
    if (fd >= 0)
        pa_close(fd);

    return -1;
}

static int udp_send_timing_packet(pa_raop_client *c, const uint32_t data[6], uint64_t received) {
    uint32_t packet[8];
    struct timeval tv;
    ssize_t written = 0;
    uint64_t trs = 0;
    int rv = 1;

    memcpy(packet, udp_timming_header, sizeof(udp_timming_header));
    /* Copying originate timestamp from the incoming request packet. */
    packet[2] = data[4];
    packet[3] = data[5];
    /* Set the receive timestamp to reception time. */
    packet[4] = htonl(received >> 32);
    packet[5] = htonl(received & 0xffffffff);
    /* Set the transmit timestamp to current time. */
    trs = timeval_to_ntp(pa_rtclock_get(&tv));
    packet[6] = htonl(trs >> 32);
    packet[7] = htonl(trs & 0xffffffff);

    written = pa_loop_write(c->udp_timing_fd, packet, sizeof(packet), NULL);
    if (written == sizeof(packet))
        rv = 0;

    return rv;
}

static int udp_send_sync_packet(pa_raop_client *c, uint32_t stamp) {
    const uint32_t delay = 88200;
    uint32_t packet[5];
    struct timeval tv;
    ssize_t written = 0;
    uint64_t trs = 0;
    int rv = 1;

    memcpy(packet, udp_sync_header, sizeof(udp_sync_header));
    if (c->udp_first_packet)
        packet[0] |= 0x10;
    stamp -= delay;
    packet[1] = htonl(stamp);
    /* Set the transmited timestamp to current time. */
    trs = timeval_to_ntp(pa_rtclock_get(&tv));
    packet[2] = htonl(trs >> 32);
    packet[3] = htonl(trs & 0xffffffff);
    stamp += delay;
    packet[4] = htonl(stamp);

    written = pa_loop_write(c->udp_control_fd, packet, sizeof(packet), NULL);
    if (written == sizeof(packet))
        rv = 0;

    return rv;
}

static void udp_build_audio_header(pa_raop_client *c, uint32_t *buffer, size_t size) {
    pa_assert(size >= sizeof(udp_audio_header));

    memcpy(buffer, udp_audio_header, sizeof(udp_audio_header));
    if (c->udp_first_packet)
        buffer[0] |= htonl((uint32_t) 0x80 << 16);
    buffer[0] |= htonl((uint32_t) c->seq);
    buffer[1] = htonl(c->rtptime);
    buffer[2] = htonl(c->udp_ssrc);
}

/* Audio retransmission header:
 * [0]    RTP v2: 0x80
 * [1]    Payload type: 0x56 + 0x80 (marker == on)
 * [2]    Unknown; seems always 0x01
 * [3]    Unknown; seems some random number around 0x20~0x40
 * [4,5]  Original RTP header htons(0x8060)
 * [6,7]  Packet sequence number to be retransmitted
 * [8,11] Original RTP timestamp on the lost packet */
static void udp_build_retrans_header(uint32_t *buffer, size_t size, uint16_t seq_num) {
    uint8_t x = 0x30; /* FIXME: what's this?? */

    pa_assert(size >= sizeof(uint32_t) * 2);

    buffer[0] = htonl((uint32_t) 0x80000000
                      | ((uint32_t) UDP_PAYLOAD_RETRANSMIT_REPLY | 0x80) << 16
                      | 0x0100
                      | x);
    buffer[1] = htonl((uint32_t) 0x80600000 | seq_num);
}

static ssize_t udp_send_audio_packet(pa_raop_client *c, bool retrans, uint8_t *buffer, size_t size) {
    ssize_t length;
    int fd = retrans ? c->udp_control_fd : c->udp_stream_fd;

    length = pa_write(fd, buffer, size, NULL);
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pa_log_debug("Discarding audio packet %d due to EAGAIN", c->seq);
        length = size;
    }
    return length;
}

static void do_rtsp_announce(pa_raop_client *c) {
    int i;
    uint8_t rsakey[512];
    char *key, *iv, *sac = NULL, *sdp;
    uint16_t rand_data;
    const char *ip;
    char *url;

    ip = pa_rtsp_localip(c->rtsp);
    /* First of all set the url properly. */
    url = pa_sprintf_malloc("rtsp://%s/%s", ip, c->sid);
    pa_rtsp_set_url(c->rtsp, url);
    pa_xfree(url);

    /* Now encrypt our aes_public key to send to the device. */
    i = rsa_encrypt(c->aes_key, AES_CHUNKSIZE, rsakey);
    pa_base64_encode(rsakey, i, &key);
    rtrimchar(key, '=');
    pa_base64_encode(c->aes_iv, AES_CHUNKSIZE, &iv);
    rtrimchar(iv, '=');

    /* UDP protocol does not need "Apple-Challenge" at announce. */
    if (c->protocol == RAOP_TCP) {
        pa_random(&rand_data, sizeof(rand_data));
        pa_base64_encode(&rand_data, AES_CHUNKSIZE, &sac);
        rtrimchar(sac, '=');
        pa_rtsp_add_header(c->rtsp, "Apple-Challenge", sac);
    }

    if (c->encryption)
        sdp = pa_sprintf_malloc(
            "v=0\r\n"
            "o=iTunes %s 0 IN IP4 %s\r\n"
            "s=iTunes\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=audio 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 AppleLossless\r\n"
            "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n"
            "a=rsaaeskey:%s\r\n"
            "a=aesiv:%s\r\n",
            c->sid, ip, c->host,
            c->protocol == RAOP_TCP ? 4096 : UDP_FRAMES_PER_PACKET,
            key, iv);
    else
        sdp = pa_sprintf_malloc(
            "v=0\r\n"
            "o=iTunes %s 0 IN IP4 %s\r\n"
            "s=iTunes\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=audio 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 AppleLossless\r\n"
            "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n",
            c->sid, ip, c->host,
            c->protocol == RAOP_TCP ? 4096 : UDP_FRAMES_PER_PACKET);

    pa_rtsp_announce(c->rtsp, sdp);
    pa_xfree(key);
    pa_xfree(iv);
    pa_xfree(sac);
    pa_xfree(sdp);
}

static void tcp_rtsp_cb(pa_rtsp_client *rtsp, pa_rtsp_state state, pa_headerlist* headers, void *userdata) {
    pa_raop_client* c = userdata;
    pa_assert(c);
    pa_assert(rtsp);
    pa_assert(rtsp == c->rtsp);

    switch (state) {
        case STATE_CONNECT: {
            pa_log_debug("RAOP: CONNECTED");
            do_rtsp_announce(c);
            break;
        }

        case STATE_OPTIONS:
            pa_log_debug("RAOP: OPTIONS");
            break;

        case STATE_ANNOUNCE:
            pa_log_debug("RAOP: ANNOUNCED");
            pa_rtsp_remove_header(c->rtsp, "Apple-Challenge");
            pa_rtsp_setup(c->rtsp, NULL);
            break;

        case STATE_SETUP: {
            char *aj = pa_xstrdup(pa_headerlist_gets(headers, "Audio-Jack-Status"));
            pa_log_debug("RAOP: SETUP");
            if (aj) {
                char *token, *pc;
                char delimiters[] = ";";
                const char* token_state = NULL;
                c->jack_type = JACK_TYPE_ANALOG;
                c->jack_status = JACK_STATUS_DISCONNECTED;

                while ((token = pa_split(aj, delimiters, &token_state))) {
                    if ((pc = strstr(token, "="))) {
                      *pc = 0;
                      if (pa_streq(token, "type") && pa_streq(pc+1, "digital")) {
                          c->jack_type = JACK_TYPE_DIGITAL;
                      }
                    } else {
                        if (pa_streq(token, "connected"))
                            c->jack_status = JACK_STATUS_CONNECTED;
                    }
                    pa_xfree(token);
                }
                pa_xfree(aj);
            } else {
                pa_log_warn("Audio Jack Status missing");
            }
            pa_rtsp_record(c->rtsp, &c->seq, &c->rtptime);
            break;
        }

        case STATE_RECORD: {
            uint32_t port = pa_rtsp_serverport(c->rtsp);
            pa_log_debug("RAOP: RECORDED");

            if (!(c->tcp_sc = pa_socket_client_new_string(c->core->mainloop, true, c->host, port))) {
                pa_log("failed to connect to server '%s:%d'", c->host, port);
                return;
            }
            pa_socket_client_set_callback(c->tcp_sc, tcp_on_connection, c);
            break;
        }

        case STATE_FLUSH:
            pa_log_debug("RAOP: FLUSHED");
            break;

        case STATE_TEARDOWN:
            pa_log_debug("RAOP: TEARDOWN");
            break;

        case STATE_SET_PARAMETER:
            pa_log_debug("RAOP: SET_PARAMETER");
            break;

        case STATE_DISCONNECTED:
            pa_assert(c->tcp_closed_callback);
            pa_assert(c->rtsp);

            pa_log_debug("RTSP control channel closed");
            pa_rtsp_client_free(c->rtsp);
            c->rtsp = NULL;
            if (c->tcp_fd > 0) {
                /* We do not close the fd, we leave it to the closed callback to do that */
                c->tcp_fd = -1;
            }
            if (c->tcp_sc) {
                pa_socket_client_unref(c->tcp_sc);
                c->tcp_sc = NULL;
            }
            pa_xfree(c->sid);
            c->sid = NULL;
            c->tcp_closed_callback(c->tcp_closed_userdata);
            break;
    }
}

static void udp_rtsp_cb(pa_rtsp_client *rtsp, pa_rtsp_state state, pa_headerlist *headers, void *userdata) {
    pa_raop_client *c = userdata;

    pa_assert(c);
    pa_assert(rtsp);
    pa_assert(rtsp == c->rtsp);

    switch (state) {
        case STATE_CONNECT: {
            uint16_t rand;
            char *sac;

            /* Set the Apple-Challenge key */
            pa_random(&rand, sizeof(rand));
            pa_base64_encode(&rand, AES_CHUNKSIZE, &sac);
            rtrimchar(sac, '=');
            pa_rtsp_add_header(c->rtsp, "Apple-Challenge", sac);

            pa_rtsp_options(c->rtsp);

            pa_xfree(sac);
            break;
        }

        case STATE_OPTIONS: {
            pa_log_debug("RAOP: OPTIONS");

            pa_rtsp_remove_header(c->rtsp, "Apple-Challenge");
            do_rtsp_announce(c);
            break;
        }

        case STATE_ANNOUNCE: {
            char *trs;

            pa_assert(c->udp_control_fd < 0);
            pa_assert(c->udp_timing_fd < 0);

            c->udp_control_fd = open_bind_udp_socket(c, &c->udp_my_control_port);
            if (c->udp_control_fd < 0)
                goto error_announce;
            c->udp_timing_fd  = open_bind_udp_socket(c, &c->udp_my_timing_port);
            if (c->udp_timing_fd < 0)
                goto error_announce;

            trs = pa_sprintf_malloc("RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=%d;timing_port=%d",
                c->udp_my_control_port,
                c->udp_my_timing_port);

            pa_rtsp_setup(c->rtsp, trs);

            pa_xfree(trs);
            break;

        error_announce:
            if (c->udp_control_fd > 0) {
                pa_close(c->udp_control_fd);
                c->udp_control_fd = -1;
            }
            if (c->udp_timing_fd > 0) {
                pa_close(c->udp_timing_fd);
                c->udp_timing_fd = -1;
            }

            pa_rtsp_client_free(c->rtsp);
            c->rtsp = NULL;

            c->udp_my_control_port     = UDP_DEFAULT_CONTROL_PORT;
            c->udp_server_control_port = UDP_DEFAULT_CONTROL_PORT;
            c->udp_my_timing_port      = UDP_DEFAULT_TIMING_PORT;
            c->udp_server_timing_port  = UDP_DEFAULT_TIMING_PORT;

            pa_log_error("aborting RTSP announce, failed creating required sockets");
        }

        case STATE_SETUP: {
            uint32_t stream_port = UDP_DEFAULT_AUDIO_PORT;
            char *ajs, *trs, *token, *pc;
            char delimiters[] = ";";
            const char *token_state = NULL;
            uint32_t port = 0;
            int ret;

            pa_log_debug("RAOP: SETUP");

            ajs = pa_xstrdup(pa_headerlist_gets(headers, "Audio-Jack-Status"));
            trs = pa_xstrdup(pa_headerlist_gets(headers, "Transport"));

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
                pa_log_warn("Audio-Jack-Status missing");
            }

            token_state = NULL;

            if (trs) {
                /* Now parse out the server port component of the response. */
                while ((token = pa_split(trs, delimiters, &token_state))) {
                    if ((pc = strstr(token, "="))) {
                        *pc = 0;
                        if (pa_streq(token, "control_port")) {
                            port = 0;
                            pa_atou(pc + 1, &port);
                            c->udp_server_control_port = port;
                        }
                        if (pa_streq(token, "timing_port")) {
                            port = 0;
                            pa_atou(pc + 1, &port);
                            c->udp_server_timing_port = port;
                        }
                        *pc = '=';
                    }
                    pa_xfree(token);
                }
            } else {
                pa_log_warn("Transport missing");
            }

            pa_xfree(ajs);
            pa_xfree(trs);

            stream_port = pa_rtsp_serverport(c->rtsp);
            if (stream_port == 0)
                goto error;
            if (c->udp_server_control_port == 0 || c->udp_server_timing_port == 0)
                goto error;

            pa_log_debug("Using server_port=%d, control_port=%d & timing_port=%d",
                stream_port,
                c->udp_server_control_port,
                c->udp_server_timing_port);

            pa_assert(c->udp_stream_fd < 0);
            pa_assert(c->udp_control_fd >= 0);
            pa_assert(c->udp_timing_fd >= 0);

            c->udp_stream_fd = connect_udp_socket(c, -1, stream_port);
            if (c->udp_stream_fd <= 0)
                goto error;
            ret = connect_udp_socket(c, c->udp_control_fd,
                                     c->udp_server_control_port);
            if (ret < 0)
                goto error;
            ret = connect_udp_socket(c, c->udp_timing_fd,
                                     c->udp_server_timing_port);
            if (ret < 0)
                goto error;

            c->udp_setup_callback(c->udp_control_fd, c->udp_timing_fd, c->udp_setup_userdata);
            pa_rtsp_record(c->rtsp, &c->seq, &c->rtptime);

            break;

        error:
            if (c->udp_stream_fd > 0) {
                pa_close(c->udp_stream_fd);
                c->udp_stream_fd = -1;
            }
            if (c->udp_control_fd > 0) {
                pa_close(c->udp_control_fd);
                c->udp_control_fd = -1;
            }
            if (c->udp_timing_fd > 0) {
                pa_close(c->udp_timing_fd);
                c->udp_timing_fd = -1;
            }

            pa_rtsp_client_free(c->rtsp);
            c->rtsp = NULL;

            c->udp_my_control_port     = UDP_DEFAULT_CONTROL_PORT;
            c->udp_server_control_port = UDP_DEFAULT_CONTROL_PORT;
            c->udp_my_timing_port      = UDP_DEFAULT_TIMING_PORT;
            c->udp_server_timing_port  = UDP_DEFAULT_TIMING_PORT;

            pa_log_error("aborting RTSP setup, failed creating required sockets");

            break;
        }

        case STATE_RECORD: {
            int32_t latency = 0;
            uint32_t rand;
            char *alt;

            pa_log_debug("RAOP: RECORD");

            alt = pa_xstrdup(pa_headerlist_gets(headers, "Audio-Latency"));
            /* Generate a random synchronization source identifier from this session. */
            pa_random(&rand, sizeof(rand));
            c->udp_ssrc = rand;

            if (alt)
                pa_atoi(alt, &latency);

            c->udp_first_packet = true;
            c->udp_sync_count = 0;

            c->udp_record_callback(c->udp_setup_userdata);

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
            pa_assert(c->udp_disconnected_callback);
            pa_assert(c->rtsp);

            pa_rtsp_disconnect(c->rtsp);

            if (c->udp_stream_fd > 0) {
                pa_close(c->udp_stream_fd);
                c->udp_stream_fd = -1;
            }

            pa_log_debug("RTSP control channel closed (teardown)");

            pa_raop_pb_clear(c->packet_buffer);

            pa_rtsp_client_free(c->rtsp);
            pa_xfree(c->sid);
            c->rtsp = NULL;
            c->sid = NULL;

            /*
              Callback for cleanup -- e.g. pollfd

              Share the disconnected callback since TEARDOWN event
              is essentially equivalent to DISCONNECTED.
              In case some special treatment turns out to be required
              for TEARDOWN in future, a new callback function may be
              defined and used.
            */
            c->udp_disconnected_callback(c->udp_disconnected_userdata);

            /* Control and timing fds are closed by udp_sink_process_msg,
               after it disables poll */
            c->udp_control_fd = -1;
            c->udp_timing_fd = -1;

            break;
        }

        case STATE_DISCONNECTED: {
            pa_log_debug("RAOP: DISCONNECTED");
            pa_assert(c->udp_disconnected_callback);
            pa_assert(c->rtsp);

            if (c->udp_stream_fd > 0) {
                pa_close(c->udp_stream_fd);
                c->udp_stream_fd = -1;
            }

            pa_log_debug("RTSP control channel closed (disconnected)");

            pa_raop_pb_clear(c->packet_buffer);

            pa_rtsp_client_free(c->rtsp);
            pa_xfree(c->sid);
            c->rtsp = NULL;
            c->sid = NULL;

            c->udp_disconnected_callback(c->udp_disconnected_userdata);
            /* Control and timing fds are closed by udp_sink_process_msg,
               after it disables poll */
            c->udp_control_fd = -1;
            c->udp_timing_fd = -1;

            break;
        }
    }
}

pa_raop_client* pa_raop_client_new(pa_core *core, const char *host, pa_raop_protocol_t protocol) {
    pa_raop_client* c;
    pa_parsed_address a;
    pa_sample_spec ss;

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
    c->tcp_fd = -1;
    c->protocol = protocol;
    c->udp_stream_fd = -1;
    c->udp_control_fd = -1;
    c->udp_timing_fd = -1;

    c->udp_my_control_port     = UDP_DEFAULT_CONTROL_PORT;
    c->udp_server_control_port = UDP_DEFAULT_CONTROL_PORT;
    c->udp_my_timing_port      = UDP_DEFAULT_TIMING_PORT;
    c->udp_server_timing_port  = UDP_DEFAULT_TIMING_PORT;

    c->host = a.path_or_host;
    if (a.port)
        c->port = a.port;
    else
        c->port = DEFAULT_RAOP_PORT;

    c->udp_first_packet = true;

    ss = core->default_sample_spec;
    /* Packet sync interval should be around 1s. */
    c->udp_sync_interval = ss.rate / UDP_FRAMES_PER_PACKET;
    c->udp_sync_count = 0;

    if (c->protocol == RAOP_TCP) {
        if (pa_raop_client_connect(c)) {
            pa_raop_client_free(c);
            return NULL;
        }
    } else
        c->packet_buffer = pa_raop_pb_new(UDP_DEFAULT_PKT_BUF_SIZE);

    return c;
}

void pa_raop_client_free(pa_raop_client *c) {
    pa_assert(c);

    pa_raop_pb_delete(c->packet_buffer);
    if (c->rtsp)
        pa_rtsp_client_free(c->rtsp);
    if (c->sid)
        pa_xfree(c->sid);
    pa_xfree(c->host);
    pa_xfree(c);
}

int pa_raop_client_connect(pa_raop_client *c) {
    char *sci;
    struct {
        uint32_t a;
        uint32_t b;
        uint32_t c;
    } rand_data;

    pa_assert(c);

    if (c->rtsp) {
        pa_log_debug("Connection already in progress");
        return 0;
    }

    if (c->protocol == RAOP_TCP)
        c->rtsp = pa_rtsp_client_new(c->core->mainloop, c->host, c->port, "iTunes/4.6 (Macintosh; U; PPC Mac OS X 10.3)");
    else
        c->rtsp = pa_rtsp_client_new(c->core->mainloop, c->host, c->port, "iTunes/7.6.2 (Windows; N;)");

    /* Initialise the AES encryption system. */
    pa_random(c->aes_iv, sizeof(c->aes_iv));
    pa_random(c->aes_key, sizeof(c->aes_key));
    memcpy(c->aes_nv, c->aes_iv, sizeof(c->aes_nv));
    AES_set_encrypt_key(c->aes_key, 128, &c->aes);

    /* Generate random instance id. */
    pa_random(&rand_data, sizeof(rand_data));
    c->sid = pa_sprintf_malloc("%u", rand_data.a);
    sci = pa_sprintf_malloc("%08x%08x",rand_data.b, rand_data.c);
    pa_rtsp_add_header(c->rtsp, "Client-Instance", sci);
    pa_xfree(sci);
    if (c->protocol == RAOP_TCP)
        pa_rtsp_set_callback(c->rtsp, tcp_rtsp_cb, c);
    else
        pa_rtsp_set_callback(c->rtsp, udp_rtsp_cb, c);

    return pa_rtsp_connect(c->rtsp);
}

int pa_raop_client_flush(pa_raop_client *c) {
    int rv = 0;
    pa_assert(c);

    if (c->rtsp != NULL) {
        rv = pa_rtsp_flush(c->rtsp, c->seq, c->rtptime);
        c->udp_sync_count = -1;
    }

    return rv;
}

int pa_raop_client_teardown(pa_raop_client *c) {
    int rv = 0;

    pa_assert(c);

    if (c->rtsp != NULL)
        rv = pa_rtsp_teardown(c->rtsp);

    return rv;
}

int pa_raop_client_udp_can_stream(pa_raop_client *c) {
    int rv = 0;

    pa_assert(c);

    if (c->udp_stream_fd > 0)
        rv = 1;

    return rv;
}

int pa_raop_client_udp_handle_timing_packet(pa_raop_client *c, const uint8_t packet[], ssize_t size) {
    const uint32_t * data = NULL;
    uint8_t payload = 0;
    struct timeval tv;
    uint64_t rci = 0;
    int rv = 0;

    pa_assert(c);
    pa_assert(packet);

    /* Timing packets are 32 bytes long: 1 x 8 RTP header (no ssrc) + 3 x 8 NTP timestamps. */
    if (size != 32 || packet[0] != 0x80)
    {
        pa_log_debug("Received an invalid timing packet.");
        return 1;
    }

    data = (uint32_t *) (packet + sizeof(udp_timming_header));
    rci = timeval_to_ntp(pa_rtclock_get(&tv));
    /* The market bit is always set (see rfc3550 for packet structure) ! */
    payload = packet[1] ^ 0x80;
    switch (payload) {
        case UDP_PAYLOAD_TIMING_REQUEST:
            rv = udp_send_timing_packet(c, data, rci);
            break;
        case UDP_PAYLOAD_TIMING_RESPONSE:
        default:
            pa_log_debug("Got an unexpected payload type on timing channel !");
            return 1;
    }

    return rv;
}

static int udp_resend_packets(pa_raop_client *c, uint16_t seq_num, uint16_t num_packets) {
    int rv = -1;
    uint8_t *data = NULL;
    ssize_t len = 0;
    int i = 0;

    pa_assert(c);
    pa_assert(num_packets > 0);
    pa_assert(c->packet_buffer);

    for (i = seq_num; i < seq_num + num_packets; i++) {
        len = pa_raop_pb_read_packet(c->packet_buffer, i, (uint8_t **) &data);

        if (len > 0) {
            ssize_t r;

            /* Obtained buffer has a header room for retransmission
               header */
            udp_build_retrans_header((uint32_t *) data, len, seq_num);
            r = udp_send_audio_packet(c, true /* retrans */, data, len);
            if (r == len)
                rv = 0;
            else
                rv = -1;
        } else
            pa_log_debug("Packet not found in retrans buffer: %u", i);
    }

    return rv;
}

int pa_raop_client_udp_handle_control_packet(pa_raop_client *c, const uint8_t packet[], ssize_t size) {
    uint8_t payload = 0;
    int rv = 0;

    uint16_t seq_num;
    uint16_t num_packets;

    pa_assert(c);
    pa_assert(packet);

    if ((size != 20 && size != 8) || packet[0] != 0x80)
    {
        pa_log_debug("Received an invalid control packet.");
        return 1;
    }

    /* The market bit is always set (see rfc3550 for packet structure) ! */

    payload = packet[1] ^ 0x80;
    switch (payload) {
        case UDP_PAYLOAD_RETRANSMIT_REQUEST:
            pa_assert(size == 8);

            /* Requested start sequence number */
            seq_num = ((uint16_t) packet[4]) << 8;
            seq_num |= (uint16_t) packet[5];
            /* Number of requested packets starting at requested seq. number */
            num_packets = (uint16_t) packet[6] << 8;
            num_packets |= (uint16_t) packet[7];
            pa_log_debug("Resending %d packets starting at %d", num_packets, seq_num);
            rv = udp_resend_packets(c, seq_num, num_packets);
            break;

        case UDP_PAYLOAD_RETRANSMIT_REPLY:
            pa_log_debug("Received a retransmit reply packet on control port (this should never happen)");
            break;

        default:
            pa_log_debug("Got an unexpected payload type on control channel: %u !", payload);
            return 1;
    }

    return rv;
}

int pa_raop_client_udp_get_blocks_size(pa_raop_client *c, size_t *size) {
    int rv = 0;

    pa_assert(c);
    pa_assert(size);

    *size = UDP_FRAMES_PER_PACKET;

    return rv;
}

ssize_t pa_raop_client_udp_send_audio_packet(pa_raop_client *c, pa_memchunk *block) {
    uint8_t *buf = NULL;
    ssize_t len;

    pa_assert(c);
    pa_assert(block);

    /* Sync RTP & NTP timestamp if required. */
    if (c->udp_first_packet || c->udp_sync_count >= c->udp_sync_interval) {
        udp_send_sync_packet(c, c->rtptime);
        c->udp_sync_count = 0;
    } else {
        c->udp_sync_count++;
    }

    buf = pa_memblock_acquire(block->memblock);
    pa_assert(buf);
    pa_assert(block->length > 0);
    udp_build_audio_header(c, (uint32_t *) (buf + block->index), block->length);
    len = udp_send_audio_packet(c, false, buf + block->index, block->length);

    /* Store packet for resending in the packet buffer */
    pa_raop_pb_write_packet(c->packet_buffer, c->seq, buf + block->index,
                            block->length);

    c->seq++;

    pa_memblock_release(block->memblock);

    if (len > 0) {
        pa_assert((size_t) len <= block->length);
        /* UDP packet has to be sent at once, so it is meaningless to
           preseve the partial data
           FIXME: This won't happen at least in *NIX systems?? */
        if (block->length > (size_t) len) {
            pa_log_warn("Tried to send %zu bytes but managed to send %zu bytes", block->length, len);
            len = block->length;
        }
        block->index += block->length;
        block->length = 0;
    }

    if (c->udp_first_packet)
        c->udp_first_packet = false;

    return len;
}

/* Adjust volume so that it fits into VOLUME_DEF <= v <= 0 dB */
pa_volume_t pa_raop_client_adjust_volume(pa_raop_client *c, pa_volume_t volume) {
    double minv, maxv;

    if (c->protocol != RAOP_UDP)
        return volume;

    maxv = pa_sw_volume_from_dB(0.0);
    minv = maxv * pow(10.0, (double) VOLUME_DEF / 60.0);

    return volume - volume * (minv / maxv) + minv;
}

int pa_raop_client_set_volume(pa_raop_client *c, pa_volume_t volume) {
    int rv = 0;
    double db;
    char *param;

    pa_assert(c);

    db = pa_sw_volume_to_dB(volume);
    if (db < VOLUME_MIN)
        db = VOLUME_MIN;
    else if (db > VOLUME_MAX)
        db = VOLUME_MAX;

    pa_log_debug("volume=%u db=%.6f", volume, db);

    param = pa_sprintf_malloc("volume: %0.6f\r\n",  db);

    /* We just hit and hope, cannot wait for the callback. */
    if (c->rtsp != NULL && pa_rtsp_exec_ready(c->rtsp))
        rv = pa_rtsp_setparameter(c->rtsp, param);
    pa_xfree(param);

    return rv;
}

int pa_raop_client_encode_sample(pa_raop_client *c, pa_memchunk *raw, pa_memchunk *encoded) {
    uint16_t len;
    size_t bufmax;
    uint8_t *bp, bpos;
    uint8_t *ibp, *maxibp;
    int size;
    uint8_t *b, *p;
    uint32_t bsize;
    size_t length;
    const uint8_t *header;
    int header_size;

    pa_assert(c);
    pa_assert(raw);
    pa_assert(raw->memblock);
    pa_assert(raw->length > 0);
    pa_assert(encoded);

    if (c->protocol == RAOP_TCP) {
        header = tcp_audio_header;
        header_size = sizeof(tcp_audio_header);
    } else {
        header = udp_audio_header;
        header_size = sizeof(udp_audio_header);
    }

    /* We have to send 4 byte chunks */
    bsize = (int)(raw->length / 4);
    length = bsize * 4;

    /* Leave 16 bytes extra to allow for the ALAC header which is about 55 bits. */
    bufmax = length + header_size + 16;
    pa_memchunk_reset(encoded);
    encoded->memblock = pa_memblock_new(c->core->mempool, bufmax);
    b = pa_memblock_acquire(encoded->memblock);
    memcpy(b, header, header_size);

    /* Now write the actual samples. */
    bp = b + header_size;
    size = bpos = 0;
    bit_writer(&bp,&bpos,&size,1,3); /* channel=1, stereo */
    bit_writer(&bp,&bpos,&size,0,4); /* Unknown */
    bit_writer(&bp,&bpos,&size,0,8); /* Unknown */
    bit_writer(&bp,&bpos,&size,0,4); /* Unknown */
    bit_writer(&bp,&bpos,&size,1,1); /* Hassize */
    bit_writer(&bp,&bpos,&size,0,2); /* Unused */
    bit_writer(&bp,&bpos,&size,1,1); /* Is-not-compressed */

    /* Size of data, integer, big endian. */
    bit_writer(&bp,&bpos,&size,(bsize>>24)&0xff,8);
    bit_writer(&bp,&bpos,&size,(bsize>>16)&0xff,8);
    bit_writer(&bp,&bpos,&size,(bsize>>8)&0xff,8);
    bit_writer(&bp,&bpos,&size,(bsize)&0xff,8);

    p = pa_memblock_acquire(raw->memblock);
    p += raw->index;
    ibp = p;
    maxibp = p + raw->length - 4;
    while (ibp <= maxibp) {
        /* Byte swap stereo data. */
        bit_writer(&bp,&bpos,&size,*(ibp+1),8);
        bit_writer(&bp,&bpos,&size,*(ibp+0),8);
        bit_writer(&bp,&bpos,&size,*(ibp+3),8);
        bit_writer(&bp,&bpos,&size,*(ibp+2),8);
        ibp += 4;
        raw->index += 4;
        raw->length -= 4;
    }
    if (c->protocol == RAOP_UDP)
        c->rtptime += (ibp - p) / 4;
    pa_memblock_release(raw->memblock);
    encoded->length = header_size + size;

    if (c->protocol == RAOP_TCP) {
        /* Store the length (endian swapped: make this better). */
        len = size + header_size - 4;
        *(b + 2) = len >> 8;
        *(b + 3) = len & 0xff;
    }

    if (c->encryption) {
        /* Encrypt our data. */
        aes_encrypt(c, (b + header_size), size);
    }

    /* We're done with the chunk. */
    pa_memblock_release(encoded->memblock);

    return 0;
}

void pa_raop_client_tcp_set_callback(pa_raop_client *c, pa_raop_client_cb_t callback, void *userdata) {
    pa_assert(c);

    c->tcp_callback = callback;
    c->tcp_userdata = userdata;
}

void pa_raop_client_tcp_set_closed_callback(pa_raop_client *c, pa_raop_client_closed_cb_t callback, void *userdata) {
    pa_assert(c);

    c->tcp_closed_callback = callback;
    c->tcp_closed_userdata = userdata;
}

void pa_raop_client_set_encryption(pa_raop_client *c, int encryption) {
    c->encryption = encryption;
}

void pa_raop_client_udp_set_setup_callback(pa_raop_client *c, pa_raop_client_setup_cb_t callback, void *userdata) {
    pa_assert(c);

    c->udp_setup_callback = callback;
    c->udp_setup_userdata = userdata;
}

void pa_raop_client_udp_set_record_callback(pa_raop_client *c, pa_raop_client_record_cb_t callback, void *userdata) {
    pa_assert(c);

    c->udp_record_callback = callback;
    c->udp_record_userdata = userdata;
}

void pa_raop_client_udp_set_disconnected_callback(pa_raop_client *c, pa_raop_client_disconnected_cb_t callback, void *userdata) {
    pa_assert(c);

    c->udp_disconnected_callback = callback;
    c->udp_disconnected_userdata = userdata;
}
