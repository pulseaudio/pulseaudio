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
struct pa_cvolume {
    uint8_t channels;
    pa_volume_t values[PA_CHANNELS_MAX];
};

/** Return non-zero when *a == *b */
int pa_cvolume_equal(const struct pa_cvolume *a, const struct pa_cvolume *b);

/** Set the volume of all channels to PA_VOLUME_NORM */
void pa_cvolume_reset(struct pa_cvolume *a);

/** Set the volume of all channels to PA_VOLUME_MUTED */
void pa_cvolume_mute(struct pa_cvolume *a);

/** Set the volume of all channels to the specified parameter */
void pa_cvolume_set(struct pa_cvolume *a, pa_volume_t v);

/** Pretty print a volume structure */
char *pa_cvolume_snprintf(char *s, size_t l, const struct pa_cvolume *c, unsigned channels);

/** Return the average volume of all channels */
pa_volume_t pa_cvolume_avg(const struct pa_cvolume *a);

/** Return non-zero if the volume of all channels is equal to the specified value */
int pa_cvolume_channels_equal_to(const struct pa_cvolume *a, uint8_t channels, pa_volume_t v);

/** Multiply two volumes specifications, return the result. This uses PA_VOLUME_NORM as neutral element of multiplication. */
pa_volume_t pa_sw_volume_multiply(pa_volume_t a, pa_volume_t b);

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
