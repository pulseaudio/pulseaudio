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
#include <limits.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/macro.h>

#include "raop-packet-buffer.h"

struct pa_raop_packet_buffer {
    pa_memchunk *packets;
    pa_mempool *mempool;
    size_t size;

    uint16_t seq;
    size_t pos;
};

pa_raop_packet_buffer *pa_raop_packet_buffer_new(pa_mempool *mempool, const size_t size) {
    pa_raop_packet_buffer *pb = pa_xnew0(pa_raop_packet_buffer, 1);

    pa_assert(mempool);
    pa_assert(size > 0);

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
    pb->seq = seq - 1;
    for (i = 0; i < pb->size; i++) {
        if (pb->packets[i].memblock)
            pa_memblock_unref(pb->packets[i].memblock);
        pa_memchunk_reset(&pb->packets[i]);
    }
}

pa_memchunk *pa_raop_packet_buffer_get(pa_raop_packet_buffer *pb, uint16_t seq, const size_t size) {
    pa_memchunk *packet = NULL;
    size_t delta, i;

    pa_assert(pb);
    pa_assert(pb->packets);
    pa_assert(seq > 0);

    if (seq == pb->seq)
        packet = &pb->packets[pb->pos];
    else if (seq < pb->seq) {
        delta = pb->seq - seq;
        i = (pb->size + pb->pos - delta) % pb->size;
        if (delta < pb->size)
            packet = &pb->packets[i];
    } else {
        i = (pb->pos + (seq - pb->seq)) % pb->size;
        if (pb->packets[i].memblock)
            pa_memblock_unref(pb->packets[i].memblock);
        pa_memchunk_reset(&pb->packets[i]);
        pb->packets[i].memblock = pa_memblock_new(pb->mempool, size);
        packet = &pb->packets[i];
        pb->seq = seq;
        pb->pos = i;
    }

    return packet;
}
