#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "endianmacros.h"
#include "sconv.h"

#include "sconv-s16le.h"
#include "sconv-s16be.h"

static void u8_to_float32(unsigned n, const void *a, unsigned an, float *b) {
    unsigned i;
    const uint8_t *ca = a;
    assert(n && a && an && b);

    for (; n > 0; n--) {
        float sum = 0;

        for (i = 0; i < an; i++) {
            uint8_t v = *(ca++);
            sum += (((float) v)-127)/127;
        }

        if (sum > 1)
            sum = 1;
        if (sum < -1)
            sum = -1;

        *(b++) = sum;
    }
}    

static void u8_from_float32(unsigned n, const float *a, void *b, unsigned bn) {
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

        u = (uint8_t) (v*127+127);
        
        for (i = 0; i < bn; i++)
            *(cb++) = u;
    }
}

static void float32_to_float32(unsigned n, const void *a, unsigned an, float *b) {
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

static void float32_from_float32(unsigned n, const float *a, void *b, unsigned bn) {
    unsigned i;
    float *cb = b;
    assert(n && a && b && bn);
    for (; n > 0; n--) {
        float v = *(a++);
        for (i = 0; i < bn; i++)
            *(cb++) = v;
    }
}

pa_convert_to_float32_func_t pa_get_convert_to_float32_function(enum pa_sample_format f) {
    switch(f) {
        case PA_SAMPLE_U8:
            return u8_to_float32;
        case PA_SAMPLE_S16LE:
            return pa_sconv_s16le_to_float32;
        case PA_SAMPLE_S16BE:
            return pa_sconv_s16be_to_float32;
        case PA_SAMPLE_FLOAT32:
            return float32_to_float32;
        default:
            return NULL;
    }
}

pa_convert_from_float32_func_t pa_get_convert_from_float32_function(enum pa_sample_format f) {
    switch(f) {
        case PA_SAMPLE_U8:
            return u8_from_float32;
        case PA_SAMPLE_S16LE:
            return pa_sconv_s16le_from_float32;
        case PA_SAMPLE_S16BE:
            return pa_sconv_s16be_from_float32;
        case PA_SAMPLE_FLOAT32:
            return float32_from_float32;
        default:
            return NULL;
    }
}
