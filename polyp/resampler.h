#ifndef fooresamplerhfoo
#define fooresamplerhfoo

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

struct pa_resampler;

struct pa_resampler* pa_resampler_new(const struct pa_sample_spec *a, const struct pa_sample_spec *b);
void pa_resampler_free(struct pa_resampler *r);

size_t pa_resampler_request(struct pa_resampler *r, size_t out_length);
void pa_resampler_run(struct pa_resampler *r, const struct pa_memchunk *in, struct pa_memchunk *out);

#endif
