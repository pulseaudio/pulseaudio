#ifndef foopulsespeexwraphfoo
#define foopulsespeexwraphfoo

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

/* We define a minimal version of speex_resampler.h however define one
 * version for fixed and another one for float. Yes, somewhat ugly */

#define spx_int16_t short
#define spx_int32_t int
#define spx_uint16_t unsigned short
#define spx_uint32_t unsigned int

typedef struct SpeexResamplerState_ SpeexResamplerState;

SpeexResamplerState *paspfx_resampler_init(spx_uint32_t nb_channels, spx_uint32_t in_rate, spx_uint32_t out_rate,  int quality, int *err);
void paspfx_resampler_destroy(SpeexResamplerState *st);
int paspfx_resampler_process_interleaved_int(SpeexResamplerState *st, const spx_int16_t *in,  spx_uint32_t *in_len, spx_int16_t *out, spx_uint32_t *out_len);
int paspfx_resampler_set_rate(SpeexResamplerState *st, spx_uint32_t in_rate, spx_uint32_t out_rate);

SpeexResamplerState *paspfl_resampler_init(spx_uint32_t nb_channels, spx_uint32_t in_rate, spx_uint32_t out_rate,  int quality, int *err);
void paspfl_resampler_destroy(SpeexResamplerState *st);
int paspfl_resampler_process_interleaved_float(SpeexResamplerState *st, const float *in,  spx_uint32_t *in_len, float *out, spx_uint32_t *out_len);
int paspfl_resampler_set_rate(SpeexResamplerState *st, spx_uint32_t in_rate, spx_uint32_t out_rate);

#endif
