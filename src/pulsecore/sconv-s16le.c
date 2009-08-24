/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* Despite the name of this file we implement S32 and S24 handling here, too. */

#include <inttypes.h>
#include <stdio.h>

#include <pulsecore/sconv.h>
#include <pulsecore/macro.h>
#include <pulsecore/log.h>

#include "endianmacros.h"

#include "sconv-s16le.h"

#ifndef INT16_FROM
#define INT16_FROM PA_INT16_FROM_LE
#endif
#ifndef UINT16_FROM
#define UINT16_FROM PA_UINT16_FROM_LE
#endif

#ifndef INT16_TO
#define INT16_TO PA_INT16_TO_LE
#endif
#ifndef UINT16_TO
#define UINT16_TO PA_UINT16_TO_LE
#endif

#ifndef INT32_FROM
#define INT32_FROM PA_INT32_FROM_LE
#endif
#ifndef UINT32_FROM
#define UINT32_FROM PA_UINT32_FROM_LE
#endif

#ifndef INT32_TO
#define INT32_TO PA_INT32_TO_LE
#endif
#ifndef UINT32_TO
#define UINT32_TO PA_UINT32_TO_LE
#endif

#ifndef READ24
#define READ24 PA_READ24LE
#endif
#ifndef WRITE24
#define WRITE24 PA_WRITE24LE
#endif

#ifndef SWAP_WORDS
#ifdef WORDS_BIGENDIAN
#define SWAP_WORDS 1
#else
#define SWAP_WORDS 0
#endif
#endif

void pa_sconv_s16le_to_float32ne(unsigned n, const int16_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1
    for (; n > 0; n--) {
        int16_t s = *(a++);
        *(b++) = ((float) INT16_FROM(s))/(float) 0x7FFF;
    }
#else
    for (; n > 0; n--)
        *(b++) = ((float) (*(a++)))/(float) 0x7FFF;
#endif
}

void pa_sconv_s32le_to_float32ne(unsigned n, const int32_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1
    for (; n > 0; n--) {
        int32_t s = *(a++);
        *(b++) = (float) (((double) INT32_FROM(s))/0x7FFFFFFF);
    }
#else
    for (; n > 0; n--)
        *(b++) = (float) (((double) (*(a++)))/0x7FFFFFFF);
#endif
}

void pa_sconv_s16le_from_float32ne(unsigned n, const float *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1
    for (; n > 0; n--) {
        int16_t s;
        float v = *(a++);

        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.f);
        s = (int16_t) lrintf(v * 0x7FFF);
        *(b++) = INT16_TO(s);
    }
#else
    for (; n > 0; n--) {
        float v = *(a++);

        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.f);
        *(b++) = (int16_t) lrintf(v * 0x7FFF);
    }
#endif
}

void pa_sconv_s32le_from_float32ne(unsigned n, const float *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1
    for (; n > 0; n--) {
        int32_t s;
        float v = *(a++);

        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        s = (int32_t) lrint((double) v * (double) 0x7FFFFFFF);
        *(b++) = INT32_TO(s);
    }
#else
    for (; n > 0; n--) {
        float v = *(a++);

        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        *(b++) = (int32_t) lrint((double) v * (double) 0x7FFFFFFF);
    }
#endif
}

void pa_sconv_s16le_to_float32re(unsigned n, const int16_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s = *(a++);
        float k = ((float) INT16_FROM(s))/0x7FFF;
        k = PA_FLOAT32_SWAP(k);
        *(b++) = k;
    }
}

void pa_sconv_s32le_to_float32re(unsigned n, const int32_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = *(a++);
        float k = (float) (((double) INT32_FROM(s))/0x7FFFFFFF);
        k = PA_FLOAT32_SWAP(k);
        *(b++) = k;
    }
}

void pa_sconv_s16le_from_float32re(unsigned n, const float *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s;
        float v = *(a++);
        v = PA_FLOAT32_SWAP(v);
        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        s = (int16_t) lrintf(v * 0x7FFF);
        *(b++) = INT16_TO(s);
    }
}

void pa_sconv_s32le_from_float32re(unsigned n, const float *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s;
        float v = *(a++);
        v = PA_FLOAT32_SWAP(v);
        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        s = (int32_t) lrint((double) v * 0x7FFFFFFF);
        *(b++) = INT32_TO(s);
    }
}

void pa_sconv_s32le_to_s16ne(unsigned n, const int32_t*a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = (int16_t) (INT32_FROM(*a) >> 16);
        a++;
        b++;
    }
}

void pa_sconv_s32le_to_s16re(unsigned n, const int32_t*a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s = (int16_t) (INT32_FROM(*a) >> 16);
        *b = PA_INT16_SWAP(s);
        a++;
        b++;
    }
}

void pa_sconv_s32le_from_s16ne(unsigned n, const int16_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = INT32_TO(((int32_t) *a) << 16);
        a++;
        b++;
    }
}

void pa_sconv_s32le_from_s16re(unsigned n, const int16_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = ((int32_t) PA_INT16_SWAP(*a)) << 16;
        *b = INT32_TO(s);
        a++;
        b++;
    }
}

void pa_sconv_s24le_to_s16ne(unsigned n, const uint8_t *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = (int16_t) (READ24(a) >> 8);
        a += 3;
        b++;
    }
}

void pa_sconv_s24le_from_s16ne(unsigned n, const int16_t *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        WRITE24(b, ((uint32_t) *a) << 8);
        a++;
        b += 3;
    }
}

void pa_sconv_s24le_to_s16re(unsigned n, const uint8_t *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s = (int16_t) (READ24(a) >> 8);
        *b = PA_INT16_SWAP(s);
        a += 3;
        b++;
    }
}

void pa_sconv_s24le_from_s16re(unsigned n, const int16_t *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        uint32_t s = ((uint32_t) PA_INT16_SWAP(*a)) << 8;
        WRITE24(b, s);
        a++;
        b += 3;
    }
}

void pa_sconv_s24le_to_float32ne(unsigned n, const uint8_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = READ24(a) << 8;
        *b = ((float) s) / 0x7FFFFFFF;
        a += 3;
        b ++;
    }
}

void pa_sconv_s24le_from_float32ne(unsigned n, const float *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s;
        float v = *a;
        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        s = (int32_t) lrint((double) v * (double) 0x7FFFFFFF);
        WRITE24(b, ((uint32_t) s) >> 8);
        a++;
        b+=3;
    }
}

void pa_sconv_s24le_to_float32re(unsigned n, const uint8_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = READ24(a) << 8;
        float k = ((float) s) / 0x7FFFFFFF;
        *b = PA_FLOAT32_SWAP(k);
        a += 3;
        b ++;
    }
}

void pa_sconv_s24le_from_float32re(unsigned n, const float *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s;
        float v = *a;
        v = PA_FLOAT32_SWAP(v);
        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        s = (int32_t) lrint((double) v * (double) 0x7FFFFFFF);
        WRITE24(b, ((uint32_t) s) >> 8);
        a++;
        b+=3;
    }
}

void pa_sconv_s24_32le_to_s16ne(unsigned n, const uint32_t *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = (int16_t) (((int32_t) (UINT32_FROM(*a) << 8)) >> 16);
        a++;
        b++;
    }
}

void pa_sconv_s24_32le_to_s16re(unsigned n, const uint32_t *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s = (int16_t) ((int32_t) (UINT32_FROM(*a) << 8) >> 16);
        *b = PA_INT16_SWAP(s);
        a++;
        b++;
    }
}

void pa_sconv_s24_32le_from_s16ne(unsigned n, const int16_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = UINT32_TO(((uint32_t) ((int32_t) *a << 16)) >> 8);
        a++;
        b++;
    }
}

void pa_sconv_s24_32le_from_s16re(unsigned n, const int16_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        uint32_t s = ((uint32_t) ((int32_t) PA_INT16_SWAP(*a) << 16)) >> 8;
        *b = UINT32_TO(s);
        a++;
        b++;
    }
}

void pa_sconv_s24_32le_to_float32ne(unsigned n, const uint32_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = (int32_t) (UINT32_FROM(*a) << 8);
        *b = (float) s / (float) 0x7FFFFFFF;
        a ++;
        b ++;
    }
}

void pa_sconv_s24_32le_to_float32re(unsigned n, const uint32_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = (int32_t) (UINT32_FROM(*a) << 8);
        float k = (float) s / (float) 0x7FFFFFFF;
        *b = PA_FLOAT32_SWAP(k);
        a ++;
        b ++;
    }
}

void pa_sconv_s24_32le_from_float32ne(unsigned n, const float *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s;
        float v = *a;
        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        s = (int32_t) lrint((double) v * (double) 0x7FFFFFFF);
        *b = UINT32_TO(((uint32_t) s) >> 8);
        a++;
        b++;
    }
}

void pa_sconv_s24_32le_from_float32re(unsigned n, const float *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s;
        float v = *a;
        v = PA_FLOAT32_SWAP(v);
        v = PA_CLAMP_UNLIKELY(v, -1.0f, 1.0f);
        s = (int32_t) lrint((double) v * (double) 0x7FFFFFFF);
        *b = UINT32_TO(((uint32_t) s) >> 8);
        a++;
        b++;
    }
}
