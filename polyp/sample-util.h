#ifndef foosampleutilhfoo
#define foosampleutilhfoo

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

#include "sample.h"
#include "memblock.h"
#include "memchunk.h"


struct pa_memblock *pa_silence_memblock(struct pa_memblock* b, const struct pa_sample_spec *spec);
void pa_silence_memchunk(struct pa_memchunk *c, const struct pa_sample_spec *spec);
void pa_silence_memory(void *p, size_t length, const struct pa_sample_spec *spec);

struct pa_mix_info {
    struct pa_memchunk chunk;
    pa_volume_t volume;
    void *userdata;
};

size_t pa_mix(struct pa_mix_info channels[], unsigned nchannels, void *data, size_t length, const struct pa_sample_spec *spec, pa_volume_t volume);

void pa_volume_memchunk(struct pa_memchunk*c, const struct pa_sample_spec *spec, pa_volume_t volume);

#endif
