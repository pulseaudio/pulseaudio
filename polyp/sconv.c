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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <liboil/liboilfuncs.h>
#include <liboil/liboil.h>

#include "endianmacros.h"
#include "sconv.h"
#include "g711.h"

#include "sconv-s16le.h"
#include "sconv-s16be.h"

static void u8_to_float32ne(unsigned n, const void *a, float *b) {
    const uint8_t *ca = a;
    static const double add = -128.0/127.0, factor = 1.0/127.0;
    
    assert(a);
    assert(b);

    oil_scaleconv_f32_u8(b, ca, n, &add, &factor);
}    

static void u8_from_float32ne(unsigned n, const float *a, void *b) {
    uint8_t *cb = b;
    static const double add = 128.0, factor = 127.0;

    assert(a);
    assert(b);

    oil_scaleconv_u8_f32(cb, a, n, &add, &factor);
}

static void float32ne_to_float32ne(unsigned n, const void *a, float *b) {
    assert(a);
    assert(b);

    oil_memcpy(b, a, sizeof(float) * n);
}

static void float32ne_from_float32ne(unsigned n, const float *a, void *b) {
    assert(a);
    assert(b);

    oil_memcpy(b, a, sizeof(float) * n);
}

static void ulaw_to_float32ne(unsigned n, const void *a, float *b) {
    const uint8_t *ca = a;

    assert(a);
    assert(b);
    
    for (; n > 0; n--)
        *(b++) = st_ulaw2linear16(*(ca++)) * 1.0F / 0x7FFF;
}

static void ulaw_from_float32ne(unsigned n, const float *a, void *b) {
    uint8_t *cb = b;

    assert(a);
    assert(b);
    
    for (; n > 0; n--) {
        float v = *(a++);

        if (v > 1)
            v = 1;

        if (v < -1)
            v = -1;

        *(cb++) = st_14linear2ulaw((int16_t) (v * 0x1FFF));
    }
}

static void alaw_to_float32ne(unsigned n, const void *a, float *b) {
    const uint8_t *ca = a;

    assert(a);
    assert(b);

    for (; n > 0; n--)
        *(b++) = st_alaw2linear16(*(ca++)) * 1.0F / 0x7FFF;
}

static void alaw_from_float32ne(unsigned n, const float *a, void *b) {
    uint8_t *cb = b;

    assert(a);
    assert(b);
    
    for (; n > 0; n--) {
        float v = *(a++);

        if (v > 1)
            v = 1;

        if (v < -1)
            v = -1;

        *(cb++) = st_13linear2alaw((int16_t) (v * 0xFFF));
    }
}

pa_convert_to_float32ne_func_t pa_get_convert_to_float32ne_function(pa_sample_format_t f) {
    switch(f) {
        case PA_SAMPLE_U8:
            return u8_to_float32ne;
        case PA_SAMPLE_S16LE:
            return pa_sconv_s16le_to_float32ne;
        case PA_SAMPLE_S16BE:
            return pa_sconv_s16be_to_float32ne;
        case PA_SAMPLE_FLOAT32NE:
            return float32ne_to_float32ne;
        case PA_SAMPLE_ALAW:
            return alaw_to_float32ne;
        case PA_SAMPLE_ULAW:
            return ulaw_to_float32ne;
        default:
            return NULL;
    }
}

pa_convert_from_float32ne_func_t pa_get_convert_from_float32ne_function(pa_sample_format_t f) {
    switch(f) {
        case PA_SAMPLE_U8:
            return u8_from_float32ne;
        case PA_SAMPLE_S16LE:
            return pa_sconv_s16le_from_float32ne;
        case PA_SAMPLE_S16BE:
            return pa_sconv_s16be_from_float32ne;
        case PA_SAMPLE_FLOAT32NE:
            return float32ne_from_float32ne;
        case PA_SAMPLE_ALAW:
            return alaw_from_float32ne;
        case PA_SAMPLE_ULAW:
            return ulaw_from_float32ne;
        default:
            return NULL;
    }
}
