#include <stdlib.h>
#include <assert.h>
#include "endianmacros.h"
#include "sconv.h"

static void s16le_to_float32(unsigned n, const void *a, unsigned an, float *b) {
    const int16_t *ca = a;
    assert(n && a && an && b);

    for (; n > 0; n--) {
        unsigned i;
        float sum = 0;
        
        for (i = 0; i < an; i++) {
            int16_t s = *(ca++);
            sum += ((float) INT16_FROM_LE(s))/0x7FFF;
        }

        if (sum > 1)
            sum = 1;
        if (sum < -1)
            sum = -1;
        
        *(b++) = sum;
    }
}

static void s16le_from_float32(unsigned n, const float *a, void *b, unsigned bn) {
    int16_t *cb = b;
    assert(n && a && b && bn);
    
    for (; n > 0; n--) {
        unsigned i;
        int16_t s;
        float v = *(a++);

        if (v > 1)
            v = 1;
        if (v < -1)
            v = -1;
        
        s = (int16_t) (v * 0x7FFF);

        for (i = 0; i < bn; i++)
            *(cb++) = INT16_TO_LE(v);
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

convert_to_float32_func_t get_convert_to_float32_function(enum pa_sample_format f) {
    switch(f) {
        case PA_SAMPLE_S16LE:
            return s16le_to_float32;
        case PA_SAMPLE_FLOAT32:
            return float32_to_float32;
        default:
            return NULL;
    }
}

convert_from_float32_func_t get_convert_from_float32_function(enum pa_sample_format f) {
    switch(f) {
        case PA_SAMPLE_S16LE:
            return s16le_from_float32;
        case PA_SAMPLE_FLOAT32:
            return float32_from_float32;
        default:
            return NULL;
    }
}
