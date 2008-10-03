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

#include <pulse/i18n.h>
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

pa_cvolume* pa_cvolume_init(pa_cvolume *a) {
    unsigned c;

    pa_assert(a);

    a->channels = 0;

    for (c = 0; c < PA_CHANNELS_MAX; c++)
        a->values[c] = (pa_volume_t) -1;

    return a;
}

pa_cvolume* pa_cvolume_set(pa_cvolume *a, unsigned channels, pa_volume_t v) {
    int i;

    pa_assert(a);
    pa_assert(channels > 0);
    pa_assert(channels <= PA_CHANNELS_MAX);

    a->channels = (uint8_t) channels;

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

pa_volume_t pa_cvolume_max(const pa_cvolume *a) {
    pa_volume_t m = 0;
    int i;
    pa_assert(a);

    for (i = 0; i < a->channels; i++)
        if (a->values[i] > m)
            m = a->values[i];

    return m;
}

pa_volume_t pa_sw_volume_multiply(pa_volume_t a, pa_volume_t b) {
    return pa_sw_volume_from_linear(pa_sw_volume_to_linear(a) * pa_sw_volume_to_linear(b));
}

pa_volume_t pa_sw_volume_divide(pa_volume_t a, pa_volume_t b) {
    double v = pa_sw_volume_to_linear(b);

    if (v <= 0)
        return 0;

    return pa_sw_volume_from_linear(pa_sw_volume_to_linear(a) / v);
}

#define USER_DECIBEL_RANGE 60

pa_volume_t pa_sw_volume_from_dB(double dB) {
    if (isinf(dB) < 0 || dB <= -USER_DECIBEL_RANGE)
        return PA_VOLUME_MUTED;

    return (pa_volume_t) lrint((dB/USER_DECIBEL_RANGE+1)*PA_VOLUME_NORM);
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

    return pow(10.0, pa_sw_volume_to_dB(v)/20.0);
}

char *pa_cvolume_snprint(char *s, size_t l, const pa_cvolume *c) {
    unsigned channel;
    pa_bool_t first = TRUE;
    char *e;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(c);

    pa_init_i18n();

    if (!pa_cvolume_valid(c)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    *(e = s) = 0;

    for (channel = 0; channel < c->channels && l > 1; channel++) {
        l -= pa_snprintf(e, l, "%s%u: %3u%%",
                      first ? "" : " ",
                      channel,
                      (c->values[channel]*100)/PA_VOLUME_NORM);

        e = strchr(e, 0);
        first = FALSE;
    }

    return s;
}

char *pa_sw_cvolume_snprint_dB(char *s, size_t l, const pa_cvolume *c) {
    unsigned channel;
    pa_bool_t first = TRUE;
    char *e;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(c);

    pa_init_i18n();

    if (!pa_cvolume_valid(c)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    *(e = s) = 0;

    for (channel = 0; channel < c->channels && l > 1; channel++) {
        l -= pa_snprintf(e, l, "%s%u: %0.2f dB",
                      first ? "" : " ",
                      channel,
                      pa_sw_volume_to_dB(c->values[channel]));

        e = strchr(e, 0);
        first = FALSE;
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

    for (i = 0; i < a->channels && i < b->channels && i < PA_CHANNELS_MAX; i++)
        dest->values[i] = pa_sw_volume_multiply(a->values[i], b->values[i]);

    dest->channels = (uint8_t) i;

    return dest;
}

pa_cvolume *pa_sw_cvolume_divide(pa_cvolume *dest, const pa_cvolume *a, const pa_cvolume *b) {
    unsigned i;

    pa_assert(dest);
    pa_assert(a);
    pa_assert(b);

    for (i = 0; i < a->channels && i < b->channels && i < PA_CHANNELS_MAX; i++)
        dest->values[i] = pa_sw_volume_divide(a->values[i], b->values[i]);

    dest->channels = (uint8_t) i;

    return dest;
}

int pa_cvolume_valid(const pa_cvolume *v) {
    unsigned c;

    pa_assert(v);

    if (v->channels <= 0 || v->channels > PA_CHANNELS_MAX)
        return 0;

    for (c = 0; c < v->channels; c++)
        if (v->values[c] == (pa_volume_t) -1)
            return 0;

    return 1;
}

static pa_bool_t on_left(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_SIDE_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_LEFT;
}

static pa_bool_t on_right(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_SIDE_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_RIGHT;
}

static pa_bool_t on_center(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_REAR_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_REAR_CENTER;
}

static pa_bool_t on_lfe(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_LFE;
}

pa_cvolume *pa_cvolume_remap(pa_cvolume *v, pa_channel_map *from, pa_channel_map *to) {
    int a, b;
    pa_cvolume result;

    pa_assert(v);
    pa_assert(from);
    pa_assert(to);
    pa_assert(v->channels == from->channels);

    if (pa_channel_map_equal(from, to))
        return v;

    result.channels = to->channels;

    for (b = 0; b < to->channels; b++) {
        pa_volume_t k = 0;
        int n = 0;

        for (a = 0; a < from->channels; a++)
            if (from->map[a] == to->map[b]) {
                k += v->values[a];
                n ++;
            }

        if (n <= 0) {
            for (a = 0; a < from->channels; a++)
                if ((on_left(from->map[a]) && on_left(to->map[b])) ||
                    (on_right(from->map[a]) && on_right(to->map[b])) ||
                    (on_center(from->map[a]) && on_center(to->map[b])) ||
                    (on_lfe(from->map[a]) && on_lfe(to->map[b]))) {

                    k += v->values[a];
                    n ++;
                }
        }

        if (n <= 0)
            k = pa_cvolume_avg(v);
        else
            k /= n;

        result.values[b] = k;
    }

    *v = result;
    return v;
}

int pa_cvolume_compatible(const pa_cvolume *v, const pa_sample_spec *ss) {

    pa_assert(v);
    pa_assert(ss);

    if (!pa_cvolume_valid(v))
        return 0;

    if (!pa_sample_spec_valid(ss))
        return 0;

    return v->channels == ss->channels;
}
