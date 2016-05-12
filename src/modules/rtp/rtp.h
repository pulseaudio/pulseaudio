#ifndef foortphfoo
#define foortphfoo

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

#include <inttypes.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pulsecore/memblockq.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/rtpoll.h>

typedef struct pa_rtp_context pa_rtp_context;

int pa_rtp_context_init_send(pa_rtp_context *c, int fd, uint8_t payload, size_t mtu, size_t frame_size);
pa_rtp_context* pa_rtp_context_new_send(int fd, uint8_t payload, size_t mtu, const pa_sample_spec *ss);

/* If the memblockq doesn't have a silence memchunk set, then the caller must
 * guarantee that the current read index doesn't point to a hole. */
int pa_rtp_send(pa_rtp_context *c, pa_memblockq *q);

pa_rtp_context* pa_rtp_context_new_recv(int fd, uint8_t payload, const pa_sample_spec *ss);
int pa_rtp_recv(pa_rtp_context *c, pa_memchunk *chunk, pa_mempool *pool, uint32_t *rtp_tstamp, struct timeval *tstamp);

void pa_rtp_context_free(pa_rtp_context *c);

size_t pa_rtp_context_get_frame_size(pa_rtp_context *c);
pa_rtpoll_item* pa_rtp_context_get_rtpoll_item(pa_rtp_context *c, pa_rtpoll *rtpoll);

pa_sample_spec* pa_rtp_sample_spec_fixup(pa_sample_spec *ss);
int pa_rtp_sample_spec_valid(const pa_sample_spec *ss);

uint8_t pa_rtp_payload_from_sample_spec(const pa_sample_spec *ss);
pa_sample_spec *pa_rtp_sample_spec_from_payload(uint8_t payload, pa_sample_spec *ss);

const char* pa_rtp_format_to_string(pa_sample_format_t f);
pa_sample_format_t pa_rtp_string_to_format(const char *s);

#endif
