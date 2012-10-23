/***
  This file is part of PulseAudio.

  Copyright 2012 Peter Meerwald <p.meerwald@bct-electronic.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/rtclock.h>

#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>

#include "cpu-arm.h"
#include "sconv.h"

#include <math.h>
#include <arm_neon.h>

static void pa_sconv_s16le_from_f32ne_neon(unsigned n, const float *src, int16_t *dst) {
    unsigned i = n & 3;

    __asm__ __volatile__ (
        "movs       %[n], %[n], lsr #2      \n\t"
        "beq        2f                      \n\t"

        "vdup.f32   q2, %[plusone]          \n\t"
        "vneg.f32   q3, q2                  \n\t"
        "vdup.f32   q4, %[scale]            \n\t"
        "vdup.u32   q5, %[mask]             \n\t"
        "vdup.f32   q6, %[half]             \n\t"

        "1:                                 \n\t"
        "vld1.32    {q0}, [%[src]]!         \n\t"
        "vmin.f32   q0, q0, q2              \n\t" /* clamp */
        "vmax.f32   q0, q0, q3              \n\t"
        "vmul.f32   q0, q0, q4              \n\t" /* scale */
        "vand.u32   q1, q0, q5              \n\t"
        "vorr.u32   q1, q1, q6              \n\t" /* round */
        "vadd.f32   q0, q0, q1              \n\t"
        "vcvt.s32.f32 q0, q0                \n\t" /* narrow */
        "vmovn.i32  d0, q0                  \n\t"
        "subs       %[n], %[n], #1          \n\t"
        "vst1.16    {d0}, [%[dst]]!         \n\t"
        "bgt        1b                      \n\t"

        "2:                                 \n\t"

        : [dst] "+r" (dst), [src] "+r" (src), [n] "+r" (n) /* output operands (or input operands that get modified) */
        : [plusone] "r" (1.0f), [scale] "r" (32767.0f), [half] "r" (0.5f), [mask] "r" (0x80000000) /* input operands */
        : "memory", "cc", "q0", "q1", "q2", "q3", "q4", "q5", "q6" /* clobber list */
    );

    /* leftovers */
    while (i--) {
        *dst++ = (int16_t) lrintf(PA_CLAMP_UNLIKELY(*src, -1.0f, 1.0f) * 0x7FFF);
        src++;
    }
}

static void pa_sconv_s16le_to_f32ne_neon(unsigned n, const int16_t *src, float *dst) {
    unsigned i = n & 3;

    const float invscale = 1.0f / 0x7FFF;

    __asm__ __volatile__ (
        "movs        %[n], %[n], lsr #2     \n\t"
        "beq        2f                      \n\t"

        "vdup.f32   q1, %[invscale]         \n\t"

        "1:                                 \n\t"
        "vld1.16    {d0}, [%[src]]!         \n\t"
        "vmovl.s16  q0, d0                  \n\t"
        "vcvt.f32.s32 q0, q0                \n\t"
        "vmul.f32   q0, q0, q1              \n\t"
        "subs       %[n], %[n], #1          \n\t"
        "vst1.32    {q0}, [%[dst]]!         \n\t"
        "bgt        1b                      \n\t"

        "2:                                 \n\t"

        : [dst] "+r" (dst), [src] "+r" (src), [n] "+r" (n) /* output operands (or input operands that get modified) */
        : [invscale] "r" (invscale) /* input operands */
        : "memory", "cc", "q0", "q1" /* clobber list */
    );

    /* leftovers */
    while (i--) {
        *dst++ = *src++ * invscale;
    }
}

void pa_convert_func_init_neon(pa_cpu_arm_flag_t flags) {
    pa_log_info("Initialising ARM NEON optimized conversions.");
    pa_set_convert_from_float32ne_function(PA_SAMPLE_S16LE, (pa_convert_func_t) pa_sconv_s16le_from_f32ne_neon);
    pa_set_convert_to_float32ne_function(PA_SAMPLE_S16LE, (pa_convert_func_t) pa_sconv_s16le_to_f32ne_neon);
}
