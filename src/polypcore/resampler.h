#ifndef fooresamplerhfoo
#define fooresamplerhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <samplerate.h>

#include <polyp/sample.h>
#include "memblock.h"
#include "memchunk.h"
#include <polyp/channelmap.h>

typedef struct pa_resampler pa_resampler;

typedef enum pa_resample_method {
    PA_RESAMPLER_INVALID                 = -1,
    PA_RESAMPLER_SRC_SINC_BEST_QUALITY   = SRC_SINC_BEST_QUALITY,
    PA_RESAMPLER_SRC_SINC_MEDIUM_QUALITY = SRC_SINC_MEDIUM_QUALITY,
    PA_RESAMPLER_SRC_SINC_FASTEST        = SRC_SINC_FASTEST,
    PA_RESAMPLER_SRC_ZERO_ORDER_HOLD     = SRC_ZERO_ORDER_HOLD,
    PA_RESAMPLER_SRC_LINEAR              = SRC_LINEAR,
    PA_RESAMPLER_TRIVIAL,
    PA_RESAMPLER_MAX
} pa_resample_method_t;

pa_resampler* pa_resampler_new(
    const pa_sample_spec *a,
    const pa_channel_map *am,
    const pa_sample_spec *b,
    const pa_channel_map *bm,
    pa_memblock_stat *s,
    pa_resample_method_t resample_method);

void pa_resampler_free(pa_resampler *r);

/* Returns the size of an input memory block which is required to return the specified amount of output data */
size_t pa_resampler_request(pa_resampler *r, size_t out_length);

/* Pass the specified memory chunk to the resampler and return the newly resampled data */
void pa_resampler_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out);

/* Change the input rate of the resampler object */
void pa_resampler_set_input_rate(pa_resampler *r, uint32_t rate);

/* Return the resampling method of the resampler object */
pa_resample_method_t pa_resampler_get_method(pa_resampler *r);

/* Try to parse the resampler method */
pa_resample_method_t pa_parse_resample_method(const char *string);

/* return a human readable string for the specified resampling method. Inverse of pa_parse_resample_method() */
const char *pa_resample_method_to_string(pa_resample_method_t m);

#endif
