#ifndef foopackethfoo
#define foopackethfoo

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

#include <sys/types.h>
#include <inttypes.h>

struct pa_packet {
    enum { PA_PACKET_APPENDED, PA_PACKET_DYNAMIC } type;
    unsigned ref;
    size_t length;
    uint8_t *data;
};

struct pa_packet* pa_packet_new(size_t length);
struct pa_packet* pa_packet_new_dynamic(uint8_t* data, size_t length);

struct pa_packet* pa_packet_ref(struct pa_packet *p);
void pa_packet_unref(struct pa_packet *p);

#endif
