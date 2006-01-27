#ifndef foovolumehfoo
#define foovolumehfoo

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

#include <inttypes.h>
#include <polyp/cdecl.h>
#include <polyp/sample.h>

/** \file
 * Constants and routines for volume handling */

PA_C_DECL_BEGIN

/** Volume specification:
 *  PA_VOLUME_MUTED: silence;
 * < PA_VOLUME_NORM: decreased volume;
 *   PA_VOLUME_NORM: normal volume;
 * > PA_VOLUME_NORM: increased volume */
typedef uint32_t pa_volume_t;

/** Normal volume (100%) */
#define PA_VOLUME_NORM (0x10000)

/** Muted volume (0%) */
#define PA_VOLUME_MUTED (0)

/** A structure encapsulating a per-channel volume */
typedef struct pa_cvolume {
    uint8_t channels;
    pa_volume_t values[PA_CHANNELS_MAX];
} pa_cvolume;

/** Return non-zero when *a == *b */
int pa_cvolume_equal(const pa_cvolume *a, const pa_cvolume *b);

/** Set the volume of all channels to PA_VOLUME_NORM */
#define pa_cvolume_reset(a, n) pa_cvolume_set((a), (n), PA_VOLUME_NORM)

/** Set the volume of all channels to PA_VOLUME_MUTED */
#define pa_cvolume_mute(a, n) pa_cvolume_set((a), (n), PA_VOLUME_MUTED)

/** Set the volume of all channels to the specified parameter */
pa_cvolume* pa_cvolume_set(pa_cvolume *a, unsigned channels, pa_volume_t v);

/** Pretty print a volume structure */
#define PA_CVOLUME_SNPRINT_MAX 64
char *pa_cvolume_snprint(char *s, size_t l, const pa_cvolume *c);

/** Return the average volume of all channels */
pa_volume_t pa_cvolume_avg(const pa_cvolume *a);

/** Return TRUE when the passed cvolume structure is valid, FALSE otherwise */
int pa_cvolume_valid(const pa_cvolume *v);

/** Return non-zero if the volume of all channels is equal to the specified value */
int pa_cvolume_channels_equal_to(const pa_cvolume *a, pa_volume_t v);

#define pa_cvolume_is_muted(a) pa_cvolume_channels_equal_to((a), PA_VOLUME_MUTED)
#define pa_cvolume_is_norm(a) pa_cvolume_channels_equal_to((a), PA_VOLUME_NORM)

/** Multiply two volumes specifications, return the result. This uses PA_VOLUME_NORM as neutral element of multiplication. */
pa_volume_t pa_sw_volume_multiply(pa_volume_t a, pa_volume_t b);

pa_cvolume *pa_sw_cvolume_multiply(pa_cvolume *dest, const pa_cvolume *a, const pa_cvolume *b);

/** Convert a decibel value to a volume. \since 0.4 */
pa_volume_t pa_sw_volume_from_dB(double f);

/** Convert a volume to a decibel value.  \since 0.4 */
double pa_sw_volume_to_dB(pa_volume_t v);

/** Convert a linear factor to a volume. \since 0.8 */
pa_volume_t pa_sw_volume_from_linear(double v);

/** Convert a volume to a linear factor. \since 0.8 */
double pa_sw_volume_to_linear(pa_volume_t v);

#ifdef INFINITY
#define PA_DECIBEL_MININFTY (-INFINITY)
#else
/** This value is used as minus infinity when using pa_volume_{to,from}_dB(). \since 0.4 */
#define PA_DECIBEL_MININFTY (-200)
#endif

PA_C_DECL_END

#endif
