#ifndef foopstreamhfoo
#define foopstreamhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>

#include "packet.h"
#include "memblock.h"
#include "iochannel.h"
#include "mainloop-api.h"
#include "memchunk.h"

/* It is safe to destroy the calling pstream object from all callbacks */

struct pa_pstream;

struct pa_pstream* pa_pstream_new(struct pa_mainloop_api *m, struct pa_iochannel *io);
void pa_pstream_free(struct pa_pstream*p);

void pa_pstream_send_packet(struct pa_pstream*p, struct pa_packet *packet);
void pa_pstream_send_memblock(struct pa_pstream*p, uint32_t channel, int32_t delta, const struct pa_memchunk *chunk);

void pa_pstream_set_recieve_packet_callback(struct pa_pstream *p, void (*callback) (struct pa_pstream *p, struct pa_packet *packet, void *userdata), void *userdata);
void pa_pstream_set_recieve_memblock_callback(struct pa_pstream *p, void (*callback) (struct pa_pstream *p, uint32_t channel, int32_t delta, const struct pa_memchunk *chunk, void *userdata), void *userdata);
void pa_pstream_set_drain_callback(struct pa_pstream *p, void (*cb)(struct pa_pstream *p, void *userdata), void *userdata);

void pa_pstream_set_die_callback(struct pa_pstream *p, void (*callback)(struct pa_pstream *p, void *userdata), void *userdata);

int pa_pstream_is_pending(struct pa_pstream *p);

#endif
