#ifndef foopulsecorebitsethfoo
#define foopulsecorebitsethfoo

/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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

#include <inttypes.h>
#include <pulsecore/macro.h>

#define PA_BITSET_ELEMENTS(n) (((n)+31)/32)
#define PA_BITSET_SIZE(n) (PA_BITSET_ELEMENTS(n)*4)

typedef uint32_t pa_bitset_t;

void pa_bitset_set(pa_bitset_t *b, unsigned k, pa_bool_t v);
pa_bool_t pa_bitset_get(const pa_bitset_t *b, unsigned k);
pa_bool_t pa_bitset_equals(const pa_bitset_t *b, unsigned n, ...);

#endif
