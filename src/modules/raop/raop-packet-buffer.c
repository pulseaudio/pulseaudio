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

#include <stdlib.h>
#include <limits.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-error.h>
#include "raop-client.h"

#include "raop-packet-buffer.h"

/* FRAMES_PER_PACKET*2*2 + sizeof(udp_audio_header) + sizeof(ALAC header), unencoded */
#define PACKET_SIZE_MAX (352*2*2 + 12 + 7) /* FIXME; hardcoded constant ! */
/* Header room for packet retransmission header */
#define RETRANS_HEADER_ROOM 4

/* Packet element */
struct pa_raop_packet_element {
    uint16_t  seq_num; /* RTP sequence number (in host byte order) */
    ssize_t   length;  /* Actual packet length */
    /* Packet data including RTP header */
    uint8_t   data[PACKET_SIZE_MAX + RETRANS_HEADER_ROOM];
};

/* Buffer struct */
struct pa_raop_packet_buffer {
    size_t   size;          /* max number of packets in buffer */
    size_t   start;         /* index of oldest packet */
    size_t   count;         /* number of packets in buffer */
    uint16_t first_seq_num; /* Sequence number of first packet in buffer */
    uint16_t latest_seq_num; /* Debug purpose */
    pa_raop_packet_element *packets; /* Packet element pointer */
};

pa_raop_packet_buffer *pa_raop_pb_new(size_t size) {
    pa_raop_packet_buffer *pb = pa_xmalloc0(sizeof(*pb));

    pb->size = size;
    pb->packets = (pa_raop_packet_element *)
        pa_xmalloc(size * sizeof(pa_raop_packet_element));

    pa_raop_pb_clear(pb);

    return pb;
}

void pa_raop_pb_clear(pa_raop_packet_buffer *pb) {
    pb->start = 0;
    pb->count = 0;
    pb->first_seq_num = 0;
    pb->latest_seq_num = 0;
    memset(pb->packets, 0, pb->size * sizeof(pa_raop_packet_element));
}

void pa_raop_pb_delete(pa_raop_packet_buffer *pb) {
    pa_xfree(pb->packets);
    pa_xfree(pb);
}

static int pb_is_full(pa_raop_packet_buffer *pb) {
    return pb->count == pb->size;
}

static int pb_is_empty(pa_raop_packet_buffer *pb) {
    return pb->count == 0;
}

static pa_raop_packet_element *pb_prepare_write(pa_raop_packet_buffer *pb, uint16_t seq) {
    size_t end = (pb->start + pb->count) % pb->size;
    pa_raop_packet_element *packet;

    /* Set first packet sequence number in buffer if buffer is empty */
    if (pb_is_empty(pb))
        pb->first_seq_num = seq;
    else
        pa_assert((uint16_t) (pb->latest_seq_num + 1) == seq);

    packet = &pb->packets[end];

    if (pb_is_full(pb)) {
        pb->start = (pb->start + 1) % pb->size; /* full, overwrite */

        /* Set first packet sequence number in buffer
           to new start packet sequence number */
        pb->first_seq_num = pb->packets[pb->start].seq_num;
    } else
        ++ pb->count;

    pb->latest_seq_num = seq;

    return packet;
}

/* Write packet data to packet buffer */
void pa_raop_pb_write_packet(pa_raop_packet_buffer *pb, uint16_t seq_num, const uint8_t *packet_data, ssize_t packet_length) {
    pa_raop_packet_element *packet;

    pa_assert(pb);
    pa_assert(packet_data);
    pa_assert(packet_length <= PACKET_SIZE_MAX);

    packet = pb_prepare_write(pb, seq_num);
    packet->seq_num = seq_num;
    packet->length = packet_length + RETRANS_HEADER_ROOM;

    /* Insert RETRANS_HEADER_ROOM bytes in front of packet data,
       for retransmission header */
    memset(packet->data, 0, RETRANS_HEADER_ROOM);
    memcpy(packet->data + RETRANS_HEADER_ROOM, packet_data, packet_length);
}

/* l < r?, considers wrapping */
static bool seq_lt(uint16_t l, uint16_t r) {
    return l - r > USHRT_MAX/2;
}

/* Random access to packet from buffer by sequence number for (re-)sending. */
ssize_t pa_raop_pb_read_packet(pa_raop_packet_buffer *pb, uint16_t seq_num, uint8_t **packet_data) {
    uint16_t index = 0; /* Index of requested packet */
    pa_raop_packet_element *packet;

    /* If the buffer is empty, there is no use in calculating indices */
    if (pb_is_empty(pb))
        return -1;

    /* If the requested packet is too old (seq_num below first seq number
       in buffer) or too young (seq_num greater than current seq number),
       do nothing and return */
    if (seq_lt(seq_num, pb->first_seq_num))
        return -1;

    index = (uint16_t) (seq_num - pb->first_seq_num);
    if (index >= pb->count)
        return -1;

    /*  Index of the requested packet in the buffer is calculated
        using the first sequence number stored in the buffer.
        The offset (seq_num - first_seq_num) is used to access the array. */
    packet = &pb->packets[(pb->start + index) % pb->size];

    pa_assert(packet->data[RETRANS_HEADER_ROOM + 2] == (seq_num >> 8));
    pa_assert(packet->data[RETRANS_HEADER_ROOM + 3] == (seq_num & 0xff));
    pa_assert(packet_data);

    *packet_data = packet->data;

    return packet->length;
}
