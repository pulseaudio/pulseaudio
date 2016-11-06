#ifndef RAOP_PACKET_BUFFER_H_INCLUDED
#define RAOP_PACKET_BUFFER_H_INCLUDED

/***
  Circular buffer for RTP audio packets with random access support
  by RTP sequence number.

  Copyright 2013 Matthias Wabersich, Hajime Fujita

  This is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  This is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.

***/

struct pa_raop_packet_element;
typedef struct pa_raop_packet_element pa_raop_packet_element;

struct pa_raop_packet_buffer;
typedef struct pa_raop_packet_buffer pa_raop_packet_buffer;

/* Allocates a new circular packet buffer
   size: Maximum number of packets to store */
pa_raop_packet_buffer *pa_raop_pb_new(size_t size);
void pa_raop_pb_clear(pa_raop_packet_buffer *pb);
void pa_raop_pb_delete(pa_raop_packet_buffer *pb);

void pa_raop_pb_write_packet(pa_raop_packet_buffer *pb, uint16_t seq_num, const uint8_t *packet_data, ssize_t packet_length);
ssize_t pa_raop_pb_read_packet(pa_raop_packet_buffer *pb, uint16_t seq_num, uint8_t **packet_data);

#endif /* RAOP_PACKET_BUFFER_H_INCLUDED */
