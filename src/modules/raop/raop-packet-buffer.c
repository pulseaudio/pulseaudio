/***
  This file is part of PulseAudio.

  Copyright 2013 Matthias Wabersich
  Copyright 2013 Hajime Fujita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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

#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/macro.h>

#include "raop-packet-buffer.h"

struct pa_raop_packet_buffer {
    pa_memchunk *packets;
    pa_mempool *mempool;

    size_t size;
    size_t count;

    uint16_t seq;
    size_t pos;
};

pa_raop_packet_buffer *pa_raop_packet_buffer_new(pa_mempool *mempool, const size_t size) {
    pa_raop_packet_buffer *pb = pa_xnew0(pa_raop_packet_buffer, 1);

    pa_assert(mempool);
    pa_assert(size > 0);

    pb->count = 0;
    pb->size = size;
    pb->mempool = mempool;
    pb->packets = pa_xnew0(pa_memchunk, size);
    pb->seq = pb->pos = 0;

    return pb;
}

void pa_raop_packet_buffer_free(pa_raop_packet_buffer *pb) {
    size_t i;

    pa_assert(pb);

    for (i = 0; pb->packets && i < pb->size; i++) {
        if (pb->packets[i].memblock)
            pa_memblock_unref(pb->packets[i].memblock);
        pa_memchunk_reset(&pb->packets[i]);
    }

    pa_xfree(pb->packets);
    pb->packets = NULL;
    pa_xfree(pb);
}

void pa_raop_packet_buffer_reset(pa_raop_packet_buffer *pb, uint16_t seq) {
    size_t i;

    pa_assert(pb);
    pa_assert(pb->packets);

    pb->pos = 0;
    pb->count = 0;
    pb->seq = (!seq) ? UINT16_MAX : seq - 1;
    for (i = 0; i < pb->size; i++) {
        if (pb->packets[i].memblock)
            pa_memblock_unref(pb->packets[i].memblock);
        pa_memchunk_reset(&pb->packets[i]);
    }
}

pa_memchunk *pa_raop_packet_buffer_prepare(pa_raop_packet_buffer *pb, uint16_t seq, const size_t size) {
    pa_memchunk *packet = NULL;
    size_t i;

    pa_assert(pb);
    pa_assert(pb->packets);

    if (seq == 0) {
        /* 0 means seq reached UINT16_MAX and has been wrapped... */
        pa_assert(pb->seq == UINT16_MAX);
        pb->seq = 0;
    } else {
        /* ...otherwise, seq MUST have be increased! */
        pa_assert(seq == pb->seq + 1);
        pb->seq++;
    }

    i = (pb->pos + 1) % pb->size;

    if (pb->packets[i].memblock)
        pa_memblock_unref(pb->packets[i].memblock);
    pa_memchunk_reset(&pb->packets[i]);

    pb->packets[i].memblock = pa_memblock_new(pb->mempool, size);
    pb->packets[i].length = size;
    pb->packets[i].index = 0;

    packet = &pb->packets[i];

    if (pb->count < pb->size)
        pb->count++;
    pb->pos = i;

    return packet;
}

pa_memchunk *pa_raop_packet_buffer_retrieve(pa_raop_packet_buffer *pb, uint16_t seq) {
    pa_memchunk *packet = NULL;
    size_t delta, i;

    pa_assert(pb);
    pa_assert(pb->packets);

    if (seq == pb->seq)
        packet = &pb->packets[pb->pos];
    else {
        if (seq < pb->seq) {
            /* Regular case: pb->seq did not wrapped since seq. */
            delta = pb->seq - seq;
        } else {
            /* Tricky case: pb->seq wrapped since seq! */
            delta = pb->seq + (UINT16_MAX - seq);
        }

        /* If the requested packet is too old, do nothing and return */
        if (delta > pb->count)
            return NULL;

        i = (pb->size + pb->pos - delta) % pb->size;

        if (delta < pb->size && pb->packets[i].memblock)
            packet = &pb->packets[i];
    }

    return packet;
}
