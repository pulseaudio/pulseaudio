#ifndef foopstreamhfoo
#define foopstreamhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>

#include <polyp/mainloop-api.h>
#include <polyp/def.h>
#include <polypcore/packet.h>
#include <polypcore/memblock.h>
#include <polypcore/iochannel.h>
#include <polypcore/memchunk.h>

typedef struct pa_pstream pa_pstream;

pa_pstream* pa_pstream_new(pa_mainloop_api *m, pa_iochannel *io, pa_memblock_stat *s);
void pa_pstream_unref(pa_pstream*p);
pa_pstream* pa_pstream_ref(pa_pstream*p);

void pa_pstream_send_packet(pa_pstream*p, pa_packet *packet);
void pa_pstream_send_memblock(pa_pstream*p, uint32_t channel, int64_t offset, pa_seek_mode_t seek, const pa_memchunk *chunk);

void pa_pstream_set_recieve_packet_callback(pa_pstream *p, void (*callback) (pa_pstream *p, pa_packet *packet, void *userdata), void *userdata);
void pa_pstream_set_recieve_memblock_callback(pa_pstream *p, void (*callback) (pa_pstream *p, uint32_t channel, int64_t offset, pa_seek_mode_t seek, const pa_memchunk *chunk, void *userdata), void *userdata);
void pa_pstream_set_drain_callback(pa_pstream *p, void (*cb)(pa_pstream *p, void *userdata), void *userdata);

void pa_pstream_set_die_callback(pa_pstream *p, void (*callback)(pa_pstream *p, void *userdata), void *userdata);

int pa_pstream_is_pending(pa_pstream *p);

void pa_pstream_close(pa_pstream *p);

#endif
