/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include "memchunk.h"

void pa_memchunk_make_writable(pa_memchunk *c, size_t min) {
    pa_memblock *n;
    size_t l;
    
    assert(c);
    assert(c->memblock);
    assert(c->memblock->ref >= 1);

    if (c->memblock->ref == 1 && !c->memblock->read_only && c->memblock->length >= c->index+min)
        return;

    l = c->length;
    if (l < min)
        l = min;
    
    n = pa_memblock_new(c->memblock->pool, l);
    memcpy(n->data, (uint8_t*) c->memblock->data + c->index, c->length);
    pa_memblock_unref(c->memblock);
    c->memblock = n;
    c->index = 0;
}

void pa_memchunk_reset(pa_memchunk *c) {
    assert(c);

    c->memblock = NULL;
    c->length = c->index = 0;
}
