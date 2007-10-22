#ifndef foosampleutilhfoo
#define foosampleutilhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <pulse/sample.h>
#include <pulse/volume.h>
#include <pulsecore/memblock.h>
#include <pulsecore/memchunk.h>

pa_memblock *pa_silence_memblock(pa_memblock* b, const pa_sample_spec *spec);
pa_memblock *pa_silence_memblock_new(pa_mempool *pool, const pa_sample_spec *spec, size_t length);
void pa_silence_memchunk(pa_memchunk *c, const pa_sample_spec *spec);
void pa_silence_memory(void *p, size_t length, const pa_sample_spec *spec);

typedef struct pa_mix_info {
    pa_memchunk chunk;
    pa_cvolume volume;
    void *userdata;
    void *internal; /* Used internally by pa_mix(), should not be initialised when calling pa_mix() */
} pa_mix_info;

size_t pa_mix(
    pa_mix_info channels[],
    unsigned nchannels,
    void *data,
    size_t length,
    const pa_sample_spec *spec,
    const pa_cvolume *volume,
    int mute);

void pa_volume_memchunk(
    pa_memchunk*c,
    const pa_sample_spec *spec,
    const pa_cvolume *volume);

size_t pa_frame_align(size_t l, const pa_sample_spec *ss) PA_GCC_PURE;

int pa_frame_aligned(size_t l, const pa_sample_spec *ss) PA_GCC_PURE;

void pa_interleave(const void *src[], unsigned channels, void *dst, size_t ss, unsigned n);
void pa_deinterleave(const void *src, void *dst[], unsigned channels, size_t ss, unsigned n);

#endif
