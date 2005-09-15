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

#include <assert.h>

#include "volume.h"

int pa_cvolume_equal(const struct pa_cvolume *a, const struct pa_cvolume *b) {
    int i;
    assert(a);
    assert(b);

    if (a->channels != b->channels)
        return 0;
    
    for (i = 0; i < a->channels; i++)
        if (a->values[i] != b->values[i])
            return 0;

    return 1;
}

void pa_cvolume_set(struct pa_cvolume *a, pa_volume_t v) {
    int i;
    assert(a);

    a->channels = PA_CHANNELS_MAX;

    for (i = 0; i < a->channels; i++)
        a->values[i] = v;
}

void pa_cvolume_reset(struct pa_cvolume *a) {
    assert(a);
    pa_cvolume_set(a, PA_VOLUME_NORM);
}

void pa_cvolume_mute(struct pa_cvolume *a) {
    assert(a);
    pa_cvolume_set(a, PA_VOLUME_MUTED);
}

pa_volume_t pa_cvolume_avg(const struct pa_cvolume *a) {
    uint64_t sum = 0;
    int i;
    assert(a);

    for (i = 0; i < a->channels; i++)
        sum += a->values[i];

    sum /= a->channels;

    return (pa_volume_t) sum;
}

pa_volume_t pa_sw_volume_multiply(pa_volume_t a, pa_volume_t b) {
    uint64_t p = a;
    p *= b;
    p /= PA_VOLUME_NORM;

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

    if (v == 1)
        return PA_VOLUME_NORM;

    return pa_sw_volume_from_dB(20*log10(v));
}

double pa_sw_volume_to_linear(pa_volume_t v) {

    if (v == PA_VOLUME_MUTED)
        return 0;

    return pow(10, pa_sw_volume_to_dB(v)/20);
    
}

char *pa_cvolume_snprintf(char *s, size_t l, const struct pa_cvolume *c, unsigned channels) {
    unsigned c;
    int first = 1;
    
    assert(s);
    assert(l > 0);
    assert(c);

    if (channels > PA_CHANNELS_MAX || channels <= 0)
        channels = PA_CHANNELS_MAX;

    *s = 0;

    for (c = 0; c < channels && l > 1; c++) {
        l -= snprintf(s, l, "%s%u: %3u%%",
                      first ? "" : " ",
                      c,
                      (c->channels[c]*100)/PA_VOLUME_NORM);

        s = strchr(s, 0);
    }

    return s;
}


/** Return non-zero if the volume of all channels is equal to the specified value */
int pa_cvolume_channels_equal_to(const struct pa_cvolume *a, uint8_t channels, pa_volume_t v) {
    unsigned c;
    assert(a);

    if (channels > PA_CHANNELS_MAX)
        channels = PA_CHANNELS_MAX;

    for (c = 0; c < channels; c++)
        if (a->map[c] != v)
            return 0;

    return 1;
}
