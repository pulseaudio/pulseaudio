#ifndef fooraoppacketbufferfoo
#define fooraoppacketbufferfoo

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

#include <pulsecore/memblock.h>
#include <pulsecore/memchunk.h>

typedef struct pa_raop_packet_buffer pa_raop_packet_buffer;

/* Allocates a new circular packet buffer, size: Maximum number of packets to store */
pa_raop_packet_buffer *pa_raop_packet_buffer_new(pa_mempool *mempool, const size_t size);
void pa_raop_packet_buffer_free(pa_raop_packet_buffer *pb);

void pa_raop_packet_buffer_reset(pa_raop_packet_buffer *pb, uint16_t seq);

pa_memchunk *pa_raop_packet_buffer_prepare(pa_raop_packet_buffer *pb, uint16_t seq, const size_t size);
pa_memchunk *pa_raop_packet_buffer_retrieve(pa_raop_packet_buffer *pb, uint16_t seq);

#endif
