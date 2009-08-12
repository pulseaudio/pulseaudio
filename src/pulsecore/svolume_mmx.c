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

#include "sample-util.h"
#include "endianmacros.h"

#if 0
static void
pa_volume_u8_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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
pa_volume_alaw_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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
pa_volume_ulaw_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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

static void
pa_volume_s16ne_mmx (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  int64_t channel, temp;

  /* the max number of samples we process at a time, this is also the max amount
   * we overread the volume array, which should have enough padding. */
  channels = MAX (4, channels);

#define VOLUME_32x16(s,v)                  /* v1_h    | v1_l    | v0_h    | v0_l      */    \
      " pxor %%mm4, %%mm4            \n\t"                                                  \
      " punpcklwd %%mm4, "#s"        \n\t" /* 0       |  p1     | 0       | p0        */    \
      " pcmpgtw "#s", %%mm4          \n\t" /* select sign from sample                 */    \
      " pand "#v", %%mm4             \n\t" /* extract sign correction factors         */    \
      " movq "#s", %%mm5             \n\t"                                                  \
      " pmulhuw "#v", "#s"           \n\t" /*   0     | p1*v1lh |    0    | p0*v0lh   */    \
      " psubd %%mm4, "#s"            \n\t" /* sign correction                         */    \
      " psrld $16, "#v"              \n\t" /*  0      | v1h     |  0      | v0h       */    \
      " pmaddwd %%mm5, "#v"          \n\t" /*      p1 * v1h     |      p0 * v0h       */    \
      " paddd "#s", "#v"             \n\t" /*      p1 * v1      |      p0 * v0        */    \
      " packssdw "#v", "#v"          \n\t" /* p0*v0   | p1*v1   | p0*v0   | p1*v1     */         

#define MOD_ADD(a,b) \
      " add "#a", %3                 \n\t" \
      " mov %3, %4                   \n\t" \
      " sub "#b", %4                 \n\t" \
      " cmp "#b", %3                 \n\t" \
      " cmovae %4, %3                \n\t" 

  __asm__ __volatile__ (
    " xor %3, %3                    \n\t"
    " sar $1, %2                    \n\t" /* length /= sizeof (int16_t) */

    " test $1, %2                   \n\t" /* check for odd samples */
    " je 2f                         \n\t" 

    " movd (%1, %3, 4), %%mm0       \n\t" /* do odd samples */
    " movw (%0), %4                 \n\t" 
    " movd %4, %%mm1                \n\t" 
    VOLUME_32x16 (%%mm1, %%mm0)
    " movd %%mm0, %4                \n\t" 
    " movw %4, (%0)                 \n\t" 
    " add $2, %0                    \n\t"
    MOD_ADD ($1, %5)

    "2:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 2 samples at a time */
    " test $1, %2                   \n\t" /* check for odd samples */
    " je 4f                         \n\t" 

    "3:                             \n\t" /* do samples in pairs of 2 */
    " movq (%1, %3, 4), %%mm0       \n\t" /* v1_h  | v1_l  | v0_h  | v0_l      */
    " movd (%0), %%mm1              \n\t" /*  X    |  X    |  p1   |  p0       */ 
    VOLUME_32x16 (%%mm1, %%mm0)
    " movd %%mm0, (%0)              \n\t" 
    " add $4, %0                    \n\t"
    MOD_ADD ($2, %5)

    "4:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 4 samples at a time */
    " cmp $0, %2                    \n\t"
    " je 6f                         \n\t"

    "5:                             \n\t" /* do samples in pairs of 4 */
    " movq (%1, %3, 4), %%mm0       \n\t" /* v1_h  | v1_l  | v0_h  | v0_l      */
    " movq 8(%1, %3, 4), %%mm2      \n\t" /* v3_h  | v3_l  | v2_h  | v2_l      */
    " movd (%0), %%mm1              \n\t" /*  X    |  X    |  p1   |  p0       */
    " movd 4(%0), %%mm3             \n\t" /*  X    |  X    |  p3   |  p2       */
    VOLUME_32x16 (%%mm1, %%mm0)
    VOLUME_32x16 (%%mm3, %%mm2)
    " movd %%mm0, (%0)              \n\t" 
    " movd %%mm2, 4(%0)              \n\t" 
    " add $8, %0                    \n\t"
    MOD_ADD ($4, %5)
    " dec %2                        \n\t"
    " jne 5b                        \n\t"

    "6:                             \n\t"
    " emms                          \n\t"

    : "+r" (samples), "+r" (volumes), "+r" (length), "=D" ((int64_t)channel), "=&r" (temp)
    : "r" ((int64_t)channels)
    : "cc"
  );
}

#if 0
static void
pa_volume_s16re_mmx (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (int16_t);

  for (channel = 0; length; length--) {
    int32_t t, hi, lo;

    hi = volumes[channel] >> 16;
    lo = volumes[channel] & 0xFFFF;

    t = (int32_t) PA_INT16_SWAP(*samples);
    t = ((t * lo) >> 16) + (t * hi);
    t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
    *samples++ = PA_INT16_SWAP((int16_t) t);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_float32ne_mmx (float *samples, float *volumes, unsigned channels, unsigned length)
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
pa_volume_float32re_mmx (float *samples, float *volumes, unsigned channels, unsigned length)
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
pa_volume_s32ne_mmx (int32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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
pa_volume_s32re_mmx (int32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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
pa_volume_s24ne_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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
pa_volume_s24re_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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
pa_volume_s24_32ne_mmx (uint32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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
pa_volume_s24_32re_mmx (uint32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
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

static void run_test (void) {
  int16_t samples[SAMPLES];
  int16_t samples_ref[SAMPLES];
  int16_t samples_orig[SAMPLES];
  int32_t volumes[CHANNELS + 16];
  int i, j, padding;
  pa_do_volume_func_t func;

  func = pa_get_volume_func (PA_SAMPLE_S16NE);

  printf ("checking %d\n", sizeof (samples));

  for (j = 0; j < TIMES; j++) {
    /*
    for (i = 0; i < SAMPLES; i++) {
      samples[i] samples_ref[i] = samples_orig[i] = rand() >> 16;
    }
    */

    pa_random (samples, sizeof (samples));
    memcpy (samples_ref, samples, sizeof (samples));
    memcpy (samples_orig, samples, sizeof (samples));

    for (i = 0; i < CHANNELS; i++)
      volumes[i] = rand() >> 15;
    for (padding = 0; padding < 16; padding++, i++)
      volumes[i] = volumes[padding];

    pa_volume_s16ne_mmx (samples, volumes, CHANNELS, sizeof (samples));
    func (samples_ref, volumes, CHANNELS, sizeof (samples));

    for (i = 0; i < SAMPLES; i++) {
      if (samples[i] != samples_ref[i]) {
        printf ("%d: %04x != %04x (%04x * %04x)\n", i, samples[i], samples_ref[i], 
  		      samples_orig[i], volumes[i % CHANNELS]);
      }
#if 0
      else
        printf ("%d: %04x == %04x (%04x * %04x)\n", i, samples[i], samples_ref[i], 
  		      samples_orig[i], volumes[i % CHANNELS]);
#endif
    }
  }
}
#endif

void pa_volume_func_init_mmx (void) {
  pa_log_info("Initialising MMX optimized functions.");

#ifdef RUN_TEST
  run_test ();
#endif

  pa_set_volume_func (PA_SAMPLE_S16NE,     (pa_do_volume_func_t) pa_volume_s16ne_mmx);
}
