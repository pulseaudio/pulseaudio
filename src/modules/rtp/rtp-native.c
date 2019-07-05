/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/arpa-inet.h>
#include <pulsecore/poll.h>

#include "rtp.h"

typedef struct pa_rtp_context {
    int fd;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    uint8_t payload;
    size_t frame_size;
    size_t mtu;

    uint8_t *recv_buf;
    size_t recv_buf_size;
    pa_memchunk memchunk;
} pa_rtp_context;

pa_rtp_context* pa_rtp_context_new_send(int fd, uint8_t payload, size_t mtu, const pa_sample_spec *ss) {
    pa_rtp_context *c;

    pa_assert(fd >= 0);

    pa_log_info("Initialising native RTP backend for send");

    c = pa_xnew0(pa_rtp_context, 1);

    c->fd = fd;
    c->sequence = (uint16_t) (rand()*rand());
    c->timestamp = 0;
    c->ssrc = (uint32_t) (rand()*rand());
    c->payload = (uint8_t) (payload & 127U);
    c->frame_size = pa_frame_size(ss);
    c->mtu = mtu;

    c->recv_buf = NULL;
    c->recv_buf_size = 0;
    pa_memchunk_reset(&c->memchunk);

    return c;
}

#define MAX_IOVECS 16

int pa_rtp_send(pa_rtp_context *c, pa_memblockq *q) {
    struct iovec iov[MAX_IOVECS];
    pa_memblock* mb[MAX_IOVECS];
    int iov_idx = 1;
    size_t n = 0;

    pa_assert(c);
    pa_assert(q);

    if (pa_memblockq_get_length(q) < c->mtu)
        return 0;

    for (;;) {
        int r;
        pa_memchunk chunk;

        pa_memchunk_reset(&chunk);

        if ((r = pa_memblockq_peek(q, &chunk)) >= 0) {

            size_t k = n + chunk.length > c->mtu ? c->mtu - n : chunk.length;

            pa_assert(chunk.memblock);

            iov[iov_idx].iov_base = pa_memblock_acquire_chunk(&chunk);
            iov[iov_idx].iov_len = k;
            mb[iov_idx] = chunk.memblock;
            iov_idx ++;

            n += k;
            pa_memblockq_drop(q, k);
        }

        pa_assert(n % c->frame_size == 0);

        if (r < 0 || n >= c->mtu || iov_idx >= MAX_IOVECS) {
            uint32_t header[3];
            struct msghdr m;
            ssize_t k;
            int i;

            if (n > 0) {
                header[0] = htonl(((uint32_t) 2 << 30) | ((uint32_t) c->payload << 16) | ((uint32_t) c->sequence));
                header[1] = htonl(c->timestamp);
                header[2] = htonl(c->ssrc);

                iov[0].iov_base = (void*)header;
                iov[0].iov_len = sizeof(header);

                m.msg_name = NULL;
                m.msg_namelen = 0;
                m.msg_iov = iov;
                m.msg_iovlen = (size_t) iov_idx;
                m.msg_control = NULL;
                m.msg_controllen = 0;
                m.msg_flags = 0;

                k = sendmsg(c->fd, &m, MSG_DONTWAIT);

                for (i = 1; i < iov_idx; i++) {
                    pa_memblock_release(mb[i]);
                    pa_memblock_unref(mb[i]);
                }

                c->sequence++;
            } else
                k = 0;

            c->timestamp += (unsigned) (n/c->frame_size);

            if (k < 0) {
                if (errno != EAGAIN && errno != EINTR) /* If the queue is full, just ignore it */
                    pa_log("sendmsg() failed: %s", pa_cstrerror(errno));
                return -1;
            }

            if (r < 0 || pa_memblockq_get_length(q) < c->mtu)
                break;

            n = 0;
            iov_idx = 1;
        }
    }

    return 0;
}

pa_rtp_context* pa_rtp_context_new_recv(int fd, uint8_t payload, const pa_sample_spec *ss) {
    pa_rtp_context *c;

    pa_log_info("Initialising native RTP backend for receive");

    c = pa_xnew0(pa_rtp_context, 1);

    c->fd = fd;
    c->payload = payload;
    c->frame_size = pa_frame_size(ss);

    c->recv_buf_size = 2000;
    c->recv_buf = pa_xmalloc(c->recv_buf_size);
    pa_memchunk_reset(&c->memchunk);

    return c;
}

int pa_rtp_recv(pa_rtp_context *c, pa_memchunk *chunk, pa_mempool *pool, uint32_t *rtp_tstamp, struct timeval *tstamp) {
    int size;
    size_t audio_length;
    size_t metadata_length;
    struct msghdr m;
    struct cmsghdr *cm;
    struct iovec iov;
    uint32_t header;
    uint32_t ssrc;
    uint8_t payload;
    unsigned cc;
    ssize_t r;
    uint8_t aux[1024];
    bool found_tstamp = false;

    pa_assert(c);
    pa_assert(chunk);

    pa_memchunk_reset(chunk);

    if (ioctl(c->fd, FIONREAD, &size) < 0) {
        pa_log_warn("FIONREAD failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (size <= 0) {
        /* size can be 0 due to any of the following reasons:
         *
         * 1. Somebody sent us a perfectly valid zero-length UDP packet.
         * 2. Somebody sent us a UDP packet with a bad CRC.
         *
         * It is unknown whether size can actually be less than zero.
         *
         * In the first case, the packet has to be read out, otherwise the
         * kernel will tell us again and again about it, thus preventing
         * reception of any further packets. So let's just read it out
         * now and discard it later, when comparing the number of bytes
         * received (0) with the number of bytes wanted (1, see below).
         *
         * In the second case, recvmsg() will fail, thus allowing us to
         * return the error.
         *
         * Just to avoid passing zero-sized memchunks and NULL pointers to
         * recvmsg(), let's force allocation of at least one byte by setting
         * size to 1.
         */
        size = 1;
    }

    if (c->recv_buf_size < (size_t) size) {
        do
            c->recv_buf_size *= 2;
        while (c->recv_buf_size < (size_t) size);

        c->recv_buf = pa_xrealloc(c->recv_buf, c->recv_buf_size);
    }

    pa_assert(c->recv_buf_size >= (size_t) size);

    iov.iov_base = c->recv_buf;
    iov.iov_len = (size_t) size;

    m.msg_name = NULL;
    m.msg_namelen = 0;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = aux;
    m.msg_controllen = sizeof(aux);
    m.msg_flags = 0;

    r = recvmsg(c->fd, &m, 0);

    if (r != size) {
        if (r < 0 && errno != EAGAIN && errno != EINTR)
            pa_log_warn("recvmsg() failed: %s", r < 0 ? pa_cstrerror(errno) : "size mismatch");

        goto fail;
    }

    if (size < 12) {
        pa_log_warn("RTP packet too short.");
        goto fail;
    }

    memcpy(&header, iov.iov_base, sizeof(uint32_t));
    memcpy(rtp_tstamp, (uint8_t*) iov.iov_base + 4, sizeof(uint32_t));
    memcpy(&ssrc, (uint8_t*) iov.iov_base + 8, sizeof(uint32_t));

    header = ntohl(header);
    *rtp_tstamp = ntohl(*rtp_tstamp);
    ssrc = ntohl(c->ssrc);

    if ((header >> 30) != 2) {
        pa_log_warn("Unsupported RTP version.");
        goto fail;
    }

    if ((header >> 29) & 1) {
        pa_log_warn("RTP padding not supported.");
        goto fail;
    }

    if ((header >> 28) & 1) {
        pa_log_warn("RTP header extensions not supported.");
        goto fail;
    }

    if (ssrc != c->ssrc) {
        pa_log_debug("Got unexpected SSRC");
        goto fail;
    }

    cc = (header >> 24) & 0xF;
    payload = (uint8_t) ((header >> 16) & 127U);
    c->sequence = (uint16_t) (header & 0xFFFFU);

    metadata_length = 12 + cc * 4;

    if (payload != c->payload) {
        pa_log_debug("Got unexpected payload: %u", payload);
        goto fail;
    }

    if (metadata_length > (unsigned) size) {
        pa_log_warn("RTP packet too short. (CSRC)");
        goto fail;
    }

    audio_length = size - metadata_length;

    if (audio_length % c->frame_size != 0) {
        pa_log_warn("Bad RTP packet size.");
        goto fail;
    }

    if (c->memchunk.length < (unsigned) audio_length) {
        size_t l;

        if (c->memchunk.memblock)
            pa_memblock_unref(c->memchunk.memblock);

        l = PA_MAX((size_t) audio_length, pa_mempool_block_size_max(pool));

        c->memchunk.memblock = pa_memblock_new(pool, l);
        c->memchunk.index = 0;
        c->memchunk.length = pa_memblock_get_length(c->memchunk.memblock);
    }

    memcpy(pa_memblock_acquire_chunk(&c->memchunk), c->recv_buf + metadata_length, audio_length);
    pa_memblock_release(c->memchunk.memblock);

    chunk->memblock = pa_memblock_ref(c->memchunk.memblock);
    chunk->index = c->memchunk.index;
    chunk->length = audio_length;

    c->memchunk.index += audio_length;
    c->memchunk.length -= audio_length;

    if (c->memchunk.length <= 0) {
        pa_memblock_unref(c->memchunk.memblock);
        pa_memchunk_reset(&c->memchunk);
    }

    for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMP) {
            memcpy(tstamp, CMSG_DATA(cm), sizeof(struct timeval));
            found_tstamp = true;
            break;
        }

    if (!found_tstamp) {
        pa_log_warn("Couldn't find SCM_TIMESTAMP data in auxiliary recvmsg() data!");
        pa_zero(*tstamp);
    }

    return 0;

fail:
    if (chunk->memblock)
        pa_memblock_unref(chunk->memblock);

    return -1;
}

void pa_rtp_context_free(pa_rtp_context *c) {
    pa_assert(c);

    pa_assert_se(pa_close(c->fd) == 0);

    if (c->memchunk.memblock)
        pa_memblock_unref(c->memchunk.memblock);

    pa_xfree(c->recv_buf);
    pa_xfree(c);
}

size_t pa_rtp_context_get_frame_size(pa_rtp_context *c) {
    return c->frame_size;
}

pa_rtpoll_item* pa_rtp_context_get_rtpoll_item(pa_rtp_context *c, pa_rtpoll *rtpoll) {
    pa_rtpoll_item *item;
    struct pollfd *p;

    item = pa_rtpoll_item_new(rtpoll, PA_RTPOLL_LATE, 1);

    p = pa_rtpoll_item_get_pollfd(item, NULL);
    p->fd = c->fd;
    p->events = POLLIN;
    p->revents = 0;

    return item;
}
