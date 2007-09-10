/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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
#include <string.h>

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "volume.h"

int pa_cvolume_equal(const pa_cvolume *a, const pa_cvolume *b) {
    int i;
    pa_assert(a);
    pa_assert(b);

    if (a->channels != b->channels)
        return 0;

    for (i = 0; i < a->channels; i++)
        if (a->values[i] != b->values[i])
            return 0;

    return 1;
}

pa_cvolume* pa_cvolume_set(pa_cvolume *a, unsigned channels, pa_volume_t v) {
    int i;

    pa_assert(a);
    pa_assert(channels > 0);
    pa_assert(channels <= PA_CHANNELS_MAX);

    a->channels = channels;

    for (i = 0; i < a->channels; i++)
        a->values[i] = v;

    return a;
}

pa_volume_t pa_cvolume_avg(const pa_cvolume *a) {
    uint64_t sum = 0;
    int i;
    pa_assert(a);

    for (i = 0; i < a->channels; i++)
        sum += a->values[i];

    sum /= a->channels;

    return (pa_volume_t) sum;
}

pa_volume_t pa_sw_volume_multiply(pa_volume_t a, pa_volume_t b) {
    return pa_sw_volume_from_linear(pa_sw_volume_to_linear(a)* pa_sw_volume_to_linear(b));
}

#define USER_DECIBEL_RANGE 30

pa_volume_t pa_sw_volume_from_dB(double dB) {
    if (dB <= -USER_DECIBEL_RANGE)
        return PA_VOLUME_MUTED;

    return (pa_volume_t) ((dB/USER_DECIBEL_RANGE+1)*PA_VOLUME_NORM);
}

double pa_sw_volume_to_dB(pa_volume_t v) {
    if (v == PA_VOLUME_MUTED)
        return PA_DECIBEL_MININFTY;

    return ((double) v/PA_VOLUME_NORM-1)*USER_DECIBEL_RANGE;
}

pa_volume_t pa_sw_volume_from_linear(double v) {

    if (v <= 0)
        return PA_VOLUME_MUTED;

    if (v > .999 && v < 1.001)
        return PA_VOLUME_NORM;

    return pa_sw_volume_from_dB(20*log10(v));
}

double pa_sw_volume_to_linear(pa_volume_t v) {

    if (v == PA_VOLUME_MUTED)
        return 0;

    return pow(10, pa_sw_volume_to_dB(v)/20);
}

char *pa_cvolume_snprint(char *s, size_t l, const pa_cvolume *c) {
    unsigned channel;
    int first = 1;
    char *e;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(c);

    *(e = s) = 0;

    for (channel = 0; channel < c->channels && l > 1; channel++) {
        l -= pa_snprintf(e, l, "%s%u: %3u%%",
                      first ? "" : " ",
                      channel,
                      (c->values[channel]*100)/PA_VOLUME_NORM);

        e = strchr(e, 0);
        first = 0;
    }

    return s;
}

/** Return non-zero if the volume of all channels is equal to the specified value */
int pa_cvolume_channels_equal_to(const pa_cvolume *a, pa_volume_t v) {
    unsigned c;
    pa_assert(a);

    for (c = 0; c < a->channels; c++)
        if (a->values[c] != v)
            return 0;

    return 1;
}

pa_cvolume *pa_sw_cvolume_multiply(pa_cvolume *dest, const pa_cvolume *a, const pa_cvolume *b) {
    unsigned i;

    pa_assert(dest);
    pa_assert(a);
    pa_assert(b);

    for (i = 0; i < a->channels && i < b->channels && i < PA_CHANNELS_MAX; i++) {

        dest->values[i] = pa_sw_volume_multiply(
            i < a->channels ? a->values[i] : PA_VOLUME_NORM,
            i < b->channels ? b->values[i] : PA_VOLUME_NORM);
    }

    dest->channels = i;

    return dest;
}

int pa_cvolume_valid(const pa_cvolume *v) {
    pa_assert(v);

    if (v->channels <= 0 || v->channels > PA_CHANNELS_MAX)
        return 0;

    return 1;
}
