/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/vector.h>
#include <pulsecore/log.h>

int main(int argc, char *argv[]) {

#ifdef __SSE2__
    pa_int16_vector_t input, zero;
    pa_int32_vector_t unpacked1, unpacked2;
    pa_int32_vector_t volume1, volume2, volume1_hi, volume1_lo, volume2_hi, volume2_lo, reduce, mask;
    pa_int16_vector_t output;

    unsigned u;

    zero.v = PA_INT16_VECTOR_MAKE(0);
    reduce.v = PA_INT32_VECTOR_MAKE(0x10000);
    volume1.v = volume2.v = PA_INT32_VECTOR_MAKE(0x10000*2+7);
    mask.v = PA_INT32_VECTOR_MAKE(0xFFFF);

    volume1_lo.m = _mm_and_si128(volume1.m, mask.m);
    volume2_lo.m = _mm_and_si128(volume2.m, mask.m);
    volume1_hi.m = _mm_srli_epi32(volume1.m, 16);
    volume2_hi.m = _mm_srli_epi32(volume2.m, 16);

    input.v = PA_INT16_VECTOR_MAKE(32000);

    for (u = 0; u < PA_INT16_VECTOR_SIZE; u++)
        pa_log("input=%i\n", input.i[u]);

    unpacked1.m = _mm_unpackhi_epi16(zero.m, input.m);
    unpacked2.m = _mm_unpacklo_epi16(zero.m, input.m);

    for (u = 0; u < PA_INT32_VECTOR_SIZE; u++)
        pa_log("unpacked1=%i\n", unpacked1.i[u]);

    unpacked1.v /= reduce.v;
    unpacked2.v /= reduce.v;

    for (u = 0; u < PA_INT32_VECTOR_SIZE; u++)
        pa_log("unpacked1=%i\n", unpacked1.i[u]);

    for (u = 0; u < PA_INT32_VECTOR_SIZE; u++)
        pa_log("volume1=%i\n", volume1.i[u]);

    unpacked1.v = (unpacked1.v * volume1_lo.v) / reduce.v + unpacked1.v * volume1_hi.v;
    unpacked2.v = (unpacked2.v * volume2_lo.v) / reduce.v + unpacked2.v * volume2_hi.v;

    for (u = 0; u < PA_INT32_VECTOR_SIZE; u++)
        pa_log("unpacked1=%i\n", unpacked1.i[u]);

    output.m = _mm_packs_epi32(unpacked1.m, unpacked2.m);

    for (u = 0; u < PA_INT16_VECTOR_SIZE; u++)
        pa_log("output=%i\n", output.i[u]);

#endif

    return 0;
}
