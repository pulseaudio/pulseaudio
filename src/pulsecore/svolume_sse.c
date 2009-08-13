/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk>

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

#include <alloca.h>

#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/g711.h>
#include <pulsecore/core-util.h>

#include "cpu-x86.h"

#include "sample-util.h"
#include "endianmacros.h"

#if 0
static void
pa_volume_u8_sse (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  for (channel = 0; length; length--) {
    int32_t t, hi, lo;

    hi = volumes[channel] >> 16;
    lo = volumes[channel] & 0xFFFF;

    t = (int32_t) *samples - 0x80;
    t = ((t * lo) >> 16) + (t * hi);
    t = PA_CLAMP_UNLIKELY(t, -0x80, 0x7F);
    *samples++ = (uint8_t) (t + 0x80);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_alaw_sse (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  for (channel = 0; length; length--) {
    int32_t t, hi, lo;

    hi = volumes[channel] >> 16;
    lo = volumes[channel] & 0xFFFF;

    t = (int32_t) st_alaw2linear16(*samples);
    t = ((t * lo) >> 16) + (t * hi);
    t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
    *samples++ = (uint8_t) st_13linear2alaw((int16_t) t >> 3);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_ulaw_sse (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  for (channel = 0; length; length--) {
    int32_t t, hi, lo;

    hi = volumes[channel] >> 16;
    lo = volumes[channel] & 0xFFFF;

    t = (int32_t) st_ulaw2linear16(*samples);
    t = ((t * lo) >> 16) + (t * hi);
    t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
    *samples++ = (uint8_t) st_14linear2ulaw((int16_t) t >> 2);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}
#endif

#define VOLUME_32x16(s,v)                  /* .. |   vh  |   vl  | */                   \
      " pxor %%xmm4, %%xmm4          \n\t" /* .. |    0  |    0  | */                   \
      " punpcklwd %%xmm4, "#s"       \n\t" /* .. |    0  |   p0  | */                   \
      " pcmpgtw "#s", %%xmm4         \n\t" /* .. |    0  | s(p0) | */                   \
      " pand "#v", %%xmm4            \n\t" /* .. |    0  |  (vl) | */                   \
      " movdqa "#s", %%xmm5          \n\t"                                              \
      " pmulhuw "#v", "#s"           \n\t" /* .. |    0  | vl*p0 | */                   \
      " psubd %%xmm4, "#s"           \n\t" /* .. |    0  | vl*p0 | + sign correct */    \
      " psrld $16, "#v"              \n\t" /* .. |   p0  |    0  | */                   \
      " pmaddwd %%xmm5, "#v"         \n\t" /* .. |    p0 * vh    | */                   \
      " paddd "#s", "#v"             \n\t" /* .. |    p0 * v0    | */                   \
      " packssdw "#v", "#v"          \n\t" /* .. | p1*v1 | p0*v0 | */         

#define MOD_ADD(a,b) \
      " add "#a", %3                 \n\t" /* channel += inc           */ \
      " mov %3, %4                   \n\t"                                \
      " sub "#b", %4                 \n\t" /* tmp = channel - channels */ \
      " cmp "#b", %3                 \n\t" /* if (channel >= channels) */ \
      " cmovae %4, %3                \n\t" /*   channel = tmp          */

/* swap 16 bits */
#define SWAP_16(s) \
      " movdqa "#s", %%xmm4          \n\t" /* .. |  h  l |  */ \
      " psrlw $8, %%xmm4             \n\t" /* .. |  0  h |  */ \
      " psllw $8, "#s"               \n\t" /* .. |  l  0 |  */ \
      " por %%xmm4, "#s"             \n\t" /* .. |  l  h |  */

/* swap 2 registers 16 bits for better pairing */
#define SWAP_16_2(s1,s2) \
      " movdqa "#s1", %%xmm4         \n\t" /* .. |  h  l |  */ \
      " movdqa "#s2", %%xmm5         \n\t"                     \
      " psrlw $8, %%xmm4             \n\t" /* .. |  0  h |  */ \
      " psrlw $8, %%xmm5             \n\t"                     \
      " psllw $8, "#s1"              \n\t" /* .. |  l  0 |  */ \
      " psllw $8, "#s2"              \n\t"                     \
      " por %%xmm4, "#s1"            \n\t" /* .. |  l  h |  */ \
      " por %%xmm5, "#s2"            \n\t"

static void
pa_volume_s16ne_sse (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  pa_reg_x86 channel, temp;

  /* the max number of samples we process at a time, this is also the max amount
   * we overread the volume array, which should have enough padding. */
  channels = MAX (8, channels);

  __asm__ __volatile__ (
    " xor %3, %3                    \n\t"
    " sar $1, %2                    \n\t" /* length /= sizeof (int16_t) */

    " test $1, %2                   \n\t" /* check for odd samples */
    " je 2f                         \n\t" 

    " movd (%1, %3, 4), %%xmm0      \n\t" /* |  v0h  |  v0l  | */
    " movw (%0), %4                 \n\t" /*     ..  |   p0  | */
    " movd %4, %%xmm1               \n\t" 
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movd %%xmm0, %4               \n\t" /*     ..  | p0*v0 | */
    " movw %4, (%0)                 \n\t" 
    " add $2, %0                    \n\t"
    MOD_ADD ($1, %5)

    "2:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 2 samples at a time */
    " test $1, %2                   \n\t" 
    " je 4f                         \n\t" 

    "3:                             \n\t" /* do samples in groups of 2 */
    " movq (%1, %3, 4), %%xmm0      \n\t" /* |  v1h  |  v1l  |  v0h  |  v0l  | */
    " movd (%0), %%xmm1             \n\t" /*              .. |   p1  |  p0   | */
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movd %%xmm0, (%0)             \n\t" /*              .. | p1*v1 | p0*v0 | */
    " add $4, %0                    \n\t"
    MOD_ADD ($2, %5)

    "4:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 4 samples at a time */
    " test $1, %2                   \n\t" 
    " je 6f                         \n\t" 

    "5:                             \n\t" /* do samples in groups of 4 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* |  v3h  |  v3l  ..  v0h  |  v0l  | */
    " movq (%0), %%xmm1             \n\t" /*              .. |   p3  ..  p0   | */
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movq %%xmm0, (%0)             \n\t" /*              .. | p3*v3 .. p0*v0 | */
    " add $8, %0                    \n\t"
    MOD_ADD ($4, %5)

    "6:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 8 samples at a time */
    " cmp $0, %2                    \n\t"
    " je 8f                         \n\t"

    "7:                             \n\t" /* do samples in groups of 8 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* |  v3h  |  v3l  ..  v0h  |  v0l  | */
    " movdqu 16(%1, %3, 4), %%xmm2  \n\t" /* |  v7h  |  v7l  ..  v4h  |  v4l  | */
    " movq (%0), %%xmm1             \n\t" /*              .. |   p3  ..  p0   | */
    " movq 8(%0), %%xmm3            \n\t" /*              .. |   p7  ..  p4   | */
    VOLUME_32x16 (%%xmm1, %%xmm0)
    VOLUME_32x16 (%%xmm3, %%xmm2)
    " movq %%xmm0, (%0)             \n\t" /*              .. | p3*v3 .. p0*v0 | */
    " movq %%xmm2, 8(%0)            \n\t" /*              .. | p7*v7 .. p4*v4 | */
    " add $16, %0                   \n\t"
    MOD_ADD ($8, %5)
    " dec %2                        \n\t"
    " jne 7b                        \n\t"
    "8:                             \n\t"

    : "+r" (samples), "+r" (volumes), "+r" (length), "=D" (channel), "=&r" (temp)
    : "r" ((pa_reg_x86)channels)
    : "cc"
  );
}

static void
pa_volume_s16re_sse (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  pa_reg_x86 channel, temp;

  /* the max number of samples we process at a time, this is also the max amount
   * we overread the volume array, which should have enough padding. */
  channels = MAX (8, channels);

  __asm__ __volatile__ (
    " xor %3, %3                    \n\t"
    " sar $1, %2                    \n\t" /* length /= sizeof (int16_t) */

    " test $1, %2                   \n\t" /* check for odd samples */
    " je 2f                         \n\t" 

    " movd (%1, %3, 4), %%xmm0      \n\t" /* do odd sample */
    " movw (%0), %4                 \n\t" 
    " rorw $8, %4                   \n\t" 
    " movd %4, %%xmm1               \n\t" 
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movd %%xmm0, %4               \n\t" 
    " rorw $8, %4                   \n\t" 
    " movw %4, (%0)                 \n\t" 
    " add $2, %0                    \n\t"
    MOD_ADD ($1, %5)

    "2:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 2 samples at a time */
    " test $1, %2                   \n\t" /* check for odd samples */
    " je 4f                         \n\t" 

    "3:                             \n\t" /* do samples in pairs of 2 */
    " movq (%1, %3, 4), %%xmm0      \n\t" /* v1_h  | v1_l  | v0_h  | v0_l      */
    " movd (%0), %%xmm1             \n\t" /*  X    |  X    |  p1   |  p0       */ 
    SWAP_16 (%%xmm1)
    VOLUME_32x16 (%%xmm1, %%xmm0)
    SWAP_16 (%%xmm0)
    " movd %%xmm0, (%0)             \n\t" 
    " add $4, %0                    \n\t"
    MOD_ADD ($2, %5)

    "4:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 4 samples at a time */
    " test $1, %2                   \n\t" /* check for odd samples */
    " je 6f                         \n\t" 

    "5:                             \n\t" /* do samples in pairs of 4 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* v1_h  | v1_l  | v0_h  | v0_l      */
    " movq (%0), %%xmm1             \n\t" /*  X    |  X    |  p1   |  p0       */ 
    SWAP_16 (%%xmm1)
    VOLUME_32x16 (%%xmm1, %%xmm0)
    SWAP_16 (%%xmm0)
    " movq %%xmm0, (%0)             \n\t" 
    " add $8, %0                    \n\t"
    MOD_ADD ($4, %5)

    "6:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 8 samples at a time */
    " cmp $0, %2                    \n\t"
    " je 8f                         \n\t"

    "7:                             \n\t" /* do samples in pairs of 8 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* v1_h  | v1_l  | v0_h  | v0_l      */
    " movdqu 16(%1, %3, 4), %%xmm2  \n\t" /* v3_h  | v3_l  | v2_h  | v2_l      */
    " movq (%0), %%xmm1             \n\t" /*  X    |  X    |  p1   |  p0       */
    " movq 8(%0), %%xmm3            \n\t" /*  X    |  X    |  p3   |  p2       */
    SWAP_16_2 (%%xmm1, %%xmm3)
    VOLUME_32x16 (%%xmm1, %%xmm0)
    VOLUME_32x16 (%%xmm3, %%xmm2)
    SWAP_16_2 (%%xmm0, %%xmm2)
    " movq %%xmm0, (%0)             \n\t" 
    " movq %%xmm2, 8(%0)            \n\t" 
    " add $16, %0                   \n\t"
    MOD_ADD ($8, %5)
    " dec %2                        \n\t"
    " jne 7b                        \n\t"
    "8:                             \n\t"

    : "+r" (samples), "+r" (volumes), "+r" (length), "=D" (channel), "=&r" (temp)
    : "r" ((pa_reg_x86)channels)
    : "cc"
  );
}

#if 0
static void
pa_volume_float32ne_sse (float *samples, float *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (float);

  for (channel = 0; length; length--) {
    *samples++ *= volumes[channel];

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_float32re_sse (float *samples, float *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (float);

  for (channel = 0; length; length--) {
    float t;

    t = PA_FLOAT32_SWAP(*samples);
    t *= volumes[channel];
    *samples++ = PA_FLOAT32_SWAP(t);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s32ne_sse (int32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (int32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t)(*samples);
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = (int32_t) t;

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s32re_sse (int32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (int32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t) PA_INT32_SWAP(*samples);
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = PA_INT32_SWAP((int32_t) t);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24ne_sse (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;
  uint8_t *e;

  e = samples + length;

  for (channel = 0; samples < e; samples += 3) {
    int64_t t;

    t = (int64_t)((int32_t) (PA_READ24NE(samples) << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    PA_WRITE24NE(samples, ((uint32_t) (int32_t) t) >> 8);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24re_sse (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;
  uint8_t *e;

  e = samples + length;

  for (channel = 0; samples < e; samples += 3) {
    int64_t t;

    t = (int64_t)((int32_t) (PA_READ24RE(samples) << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    PA_WRITE24RE(samples, ((uint32_t) (int32_t) t) >> 8);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24_32ne_sse (uint32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (uint32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t) ((int32_t) (*samples << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = ((uint32_t) ((int32_t) t)) >> 8;

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24_32re_sse (uint32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (uint32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t) ((int32_t) (PA_UINT32_SWAP(*samples) << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = PA_UINT32_SWAP(((uint32_t) ((int32_t) t)) >> 8);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}
#endif

#undef RUN_TEST

#ifdef RUN_TEST
#define CHANNELS 2
#define SAMPLES 1021
#define TIMES 1000
#define PADDING 16

static void run_test (void) {
  int16_t samples[SAMPLES];
  int16_t samples_ref[SAMPLES];
  int16_t samples_orig[SAMPLES];
  int32_t volumes[CHANNELS + PADDING];
  int i, j, padding;
  pa_do_volume_func_t func;

  func = pa_get_volume_func (PA_SAMPLE_S16RE);

  printf ("checking SSE %d\n", sizeof (samples));

  for (j = 0; j < TIMES; j++) {
    pa_random (samples, sizeof (samples));
    memcpy (samples_ref, samples, sizeof (samples));
    memcpy (samples_orig, samples, sizeof (samples));

    for (i = 0; i < CHANNELS; i++)
      volumes[i] = rand() >> 1;
    for (padding = 0; padding < PADDING; padding++, i++)
      volumes[i] = volumes[padding];

    pa_volume_s16re_sse (samples, volumes, CHANNELS, SAMPLES * sizeof (int16_t));
    func (samples_ref, volumes, CHANNELS, SAMPLES * sizeof (int16_t));

    for (i = 0; i < SAMPLES; i++) {
      if (samples[i] != samples_ref[i]) {
        printf ("%d: %04x != %04x (%04x * %04x)\n", i, samples[i], samples_ref[i], 
  		      samples_orig[i], volumes[i % CHANNELS]);
      }
    }
  }
}
#endif

void pa_volume_func_init_sse (pa_cpu_x86_flag_t flags) {
  pa_log_info("Initialising SSE optimized functions.");

#ifdef RUN_TEST
  run_test ();
#endif

  pa_set_volume_func (PA_SAMPLE_S16NE,     (pa_do_volume_func_t) pa_volume_s16ne_sse);
  pa_set_volume_func (PA_SAMPLE_S16RE,     (pa_do_volume_func_t) pa_volume_s16re_sse);
}
