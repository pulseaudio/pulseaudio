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
#include "endianmacros.h"
#include "sconv.h"
#include "g711.h"

#include "sconv-s16le.h"
#include "sconv-s16be.h"

static void u8_to_float32ne(unsigned n, const void *a, unsigned an, float *b) {
    unsigned i;
    const uint8_t *ca = a;
    assert(n && a && an && b);

    for (; n > 0; n--) {
        float sum = 0;

        for (i = 0; i < an; i++) {
            uint8_t v = *(ca++);
            sum += (((float) v)-128)/127;
        }

        if (sum > 1)
            sum = 1;
        if (sum < -1)
            sum = -1;

        *(b++) = sum;
    }
}    

static void u8_from_float32ne(unsigned n, const float *a, void *b, unsigned bn) {
    unsigned i;
    uint8_t *cb = b;

    assert(n && a && b && bn);
    for (; n > 0; n--) {
        float v = *(a++);
        uint8_t u;

        if (v > 1)
            v = 1;

        if (v < -1)
            v = -1;

        u = (uint8_t) (v*127+128);
        
        for (i = 0; i < bn; i++)
            *(cb++) = u;
    }
}

static void float32ne_to_float32ne(unsigned n, const void *a, unsigned an, float *b) {
    unsigned i;
    const float *ca = a;
    assert(n && a && an && b);
    for (; n > 0; n--) {
        float sum = 0;

        for (i = 0; i < an; i++)
            sum += *(ca++);

        if (sum > 1)
            sum = 1;
        if (sum < -1)
            sum = -1;

        *(b++) = sum;
    }
}

static void float32ne_from_float32ne(unsigned n, const float *a, void *b, unsigned bn) {
    unsigned i;
    float *cb = b;
    assert(n && a && b && bn);
    for (; n > 0; n--) {
        float v = *(a++);
        for (i = 0; i < bn; i++)
            *(cb++) = v;
    }
}

static void ulaw_to_float32ne(unsigned n, const void *a, unsigned an, float *b) {
    unsigned i;
    const uint8_t *ca = a;
    assert(n && a && an && b);
    for (; n > 0; n--) {
        float sum = 0;

        for (i = 0; i < an; i++)
            sum += st_ulaw2linear16(*ca++) * 1.0F / 0x7FFF;

        if (sum > 1)
            sum = 1;
        if (sum < -1)
            sum = -1;

        *(b++) = sum;
    }
}

static void ulaw_from_float32ne(unsigned n, const float *a, void *b, unsigned bn) {
    unsigned i;
    uint8_t *cb = b;

    assert(n && a && b && bn);
    for (; n > 0; n--) {
        float v = *(a++);
        uint8_t u;

        if (v > 1)
            v = 1;

        if (v < -1)
            v = -1;

        u = st_14linear2ulaw((int16_t) (v * 0x1FFF));
        
        for (i = 0; i < bn; i++)
            *(cb++) = u;
    }
}

static void alaw_to_float32ne(unsigned n, const void *a, unsigned an, float *b) {
    unsigned i;
    const uint8_t *ca = a;
    assert(n && a && an && b);
    for (; n > 0; n--) {
        float sum = 0;

        for (i = 0; i < an; i++)
            sum += st_alaw2linear16(*ca++) * 1.0F / 0x7FFF;

        if (sum > 1)
            sum = 1;
        if (sum < -1)
            sum = -1;

        *(b++) = sum;
    }
}

static void alaw_from_float32ne(unsigned n, const float *a, void *b, unsigned bn) {
    unsigned i;
    uint8_t *cb = b;

    assert(n && a && b && bn);
    for (; n > 0; n--) {
        float v = *(a++);
        uint8_t u;

        if (v > 1)
            v = 1;

        if (v < -1)
            v = -1;

        u = st_13linear2alaw((int16_t) (v * 0xFFF));
        
        for (i = 0; i < bn; i++)
            *(cb++) = u;
    }
}


pa_convert_to_float32ne_func_t pa_get_convert_to_float32ne_function(pa_sample_format f) {
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

pa_convert_from_float32ne_func_t pa_get_convert_from_float32ne_function(pa_sample_format f) {
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
