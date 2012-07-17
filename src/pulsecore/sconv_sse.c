/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdio.h>
#include <stdlib.h>

#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>

#include "cpu-x86.h"
#include "sconv.h"

#if !defined(__APPLE__) && defined (__i386__) || defined (__amd64__)

static const PA_DECLARE_ALIGNED (16, float, one[4]) = { 1.0, 1.0, 1.0, 1.0 };
static const PA_DECLARE_ALIGNED (16, float, mone[4]) = { -1.0, -1.0, -1.0, -1.0 };
static const PA_DECLARE_ALIGNED (16, float, scale[4]) = { 0x7fff, 0x7fff, 0x7fff, 0x7fff };

static void pa_sconv_s16le_from_f32ne_sse(unsigned n, const float *a, int16_t *b) {
    pa_reg_x86 temp, i;

    __asm__ __volatile__ (
        " movaps %5, %%xmm5             \n\t"
        " movaps %6, %%xmm6             \n\t"
        " movaps %7, %%xmm7             \n\t"
        " xor %0, %0                    \n\t"

        " mov %4, %1                    \n\t"
        " sar $3, %1                    \n\t" /* 8 floats at a time */
        " cmp $0, %1                    \n\t"
        " je 2f                         \n\t"

        "1:                             \n\t"
        " movups (%q2, %0, 2), %%xmm0   \n\t" /* read 8 floats */
        " movups 16(%q2, %0, 2), %%xmm2 \n\t"
        " minps  %%xmm5, %%xmm0         \n\t" /* clamp to 1.0 */
        " minps  %%xmm5, %%xmm2         \n\t"
        " maxps  %%xmm6, %%xmm0         \n\t" /* clamp to -1.0 */
        " maxps  %%xmm6, %%xmm2         \n\t"
        " mulps  %%xmm7, %%xmm0         \n\t" /* *= 0x7fff */
        " mulps  %%xmm7, %%xmm2         \n\t"

        " cvtps2pi %%xmm0, %%mm0        \n\t" /* low part to int */
        " cvtps2pi %%xmm2, %%mm2        \n\t"
        " movhlps  %%xmm0, %%xmm0       \n\t" /* bring high part in position */
        " movhlps  %%xmm2, %%xmm2       \n\t"
        " cvtps2pi %%xmm0, %%mm1        \n\t" /* high part to int */
        " cvtps2pi %%xmm2, %%mm3        \n\t"

        " packssdw %%mm1, %%mm0         \n\t" /* pack parts */
        " packssdw %%mm3, %%mm2         \n\t"
        " movq     %%mm0, (%q3, %0)     \n\t"
        " movq    %%mm2, 8(%q3, %0)     \n\t"

        " add $16, %0                   \n\t"
        " dec %1                        \n\t"
        " jne 1b                        \n\t"

        "2:                             \n\t"
        " mov %4, %1                    \n\t" /* prepare for leftovers */
        " and $7, %1                    \n\t"
        " je 4f                         \n\t"

        "3:                             \n\t"
        " movss (%q2, %0, 2), %%xmm0    \n\t"
        " minss  %%xmm5, %%xmm0         \n\t"
        " maxss  %%xmm6, %%xmm0         \n\t"
        " mulss  %%xmm7, %%xmm0         \n\t"
        " cvtss2si %%xmm0, %4           \n\t"
        " movw  %w4, (%q3, %0)          \n\t"
        " add $2, %0                    \n\t"
        " dec %1                        \n\t"
        " jne 3b                        \n\t"

        "4:                             \n\t"
        " emms                          \n\t"

        : "=&r" (i), "=&r" (temp)
        : "r" (a), "r" (b), "r" ((pa_reg_x86)n), "m" (*one), "m" (*mone), "m" (*scale)
        : "cc", "memory"
    );
}

static void pa_sconv_s16le_from_f32ne_sse2(unsigned n, const float *a, int16_t *b) {
    pa_reg_x86 temp, i;

    __asm__ __volatile__ (
        " movaps %5, %%xmm5             \n\t"
        " movaps %6, %%xmm6             \n\t"
        " movaps %7, %%xmm7             \n\t"
        " xor %0, %0                    \n\t"

        " mov %4, %1                    \n\t"
        " sar $3, %1                    \n\t" /* 8 floats at a time */
        " cmp $0, %1                    \n\t"
        " je 2f                         \n\t"

        "1:                             \n\t"
        " movups (%q2, %0, 2), %%xmm0   \n\t" /* read 8 floats */
        " movups 16(%q2, %0, 2), %%xmm2 \n\t"
        " minps  %%xmm5, %%xmm0         \n\t" /* clamp to 1.0 */
        " minps  %%xmm5, %%xmm2         \n\t"
        " maxps  %%xmm6, %%xmm0         \n\t" /* clamp to -1.0 */
        " maxps  %%xmm6, %%xmm2         \n\t"
        " mulps  %%xmm7, %%xmm0         \n\t" /* *= 0x7fff */
        " mulps  %%xmm7, %%xmm2         \n\t"

        " cvtps2dq %%xmm0, %%xmm0       \n\t"
        " cvtps2dq %%xmm2, %%xmm2       \n\t"

        " packssdw %%xmm2, %%xmm0       \n\t"
        " movdqu   %%xmm0, (%q3, %0)    \n\t"

        " add $16, %0                   \n\t"
        " dec %1                        \n\t"
        " jne 1b                        \n\t"

        "2:                             \n\t"
        " mov %4, %1                    \n\t" /* prepare for leftovers */
        " and $7, %1                    \n\t"
        " je 4f                         \n\t"

        "3:                             \n\t"
        " movss (%q2, %0, 2), %%xmm0    \n\t"
        " minss  %%xmm5, %%xmm0         \n\t"
        " maxss  %%xmm6, %%xmm0         \n\t"
        " mulss  %%xmm7, %%xmm0         \n\t"
        " cvtss2si %%xmm0, %4           \n\t"
        " movw  %w4, (%q3, %0)          \n\t"
        " add $2, %0                    \n\t"
        " dec %1                        \n\t"
        " jne 3b                        \n\t"

        "4:                             \n\t"

        : "=&r" (i), "=&r" (temp)
        : "r" (a), "r" (b), "r" ((pa_reg_x86)n), "m" (*one), "m" (*mone), "m" (*scale)
        : "cc", "memory"
    );
}

#endif /* defined (__i386__) || defined (__amd64__) */

void pa_convert_func_init_sse(pa_cpu_x86_flag_t flags) {
#if !defined(__APPLE__) && defined (__i386__) || defined (__amd64__)

    if (flags & PA_CPU_X86_SSE2) {
        pa_log_info("Initialising SSE2 optimized conversions.");
        pa_set_convert_from_float32ne_function(PA_SAMPLE_S16LE, (pa_convert_func_t) pa_sconv_s16le_from_f32ne_sse2);
    } else {
        pa_log_info("Initialising SSE optimized conversions.");
        pa_set_convert_from_float32ne_function(PA_SAMPLE_S16LE, (pa_convert_func_t) pa_sconv_s16le_from_f32ne_sse);
    }

#endif /* defined (__i386__) || defined (__amd64__) */
}
