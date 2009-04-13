/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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
#include <string.h>

#include <pulse/i18n.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "volume.h"

int pa_cvolume_equal(const pa_cvolume *a, const pa_cvolume *b) {
    int i;
    pa_assert(a);
    pa_assert(b);

    pa_return_val_if_fail(pa_cvolume_valid(a), 0);
    pa_return_val_if_fail(pa_cvolume_valid(b), 0);

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
    pa_return_val_if_fail(pa_cvolume_valid(a), PA_VOLUME_MUTED);

    for (i = 0; i < a->channels; i++)
        sum += a->values[i];

    sum /= a->channels;

    return (pa_volume_t) sum;
}

pa_volume_t pa_cvolume_max(const pa_cvolume *a) {
    pa_volume_t m = 0;
    int i;

    pa_assert(a);
    pa_return_val_if_fail(pa_cvolume_valid(a), PA_VOLUME_MUTED);

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

#define USER_DECIBEL_RANGE 90

pa_volume_t pa_sw_volume_from_dB(double dB) {
    if (isinf(dB) < 0 || dB <= -USER_DECIBEL_RANGE)
        return PA_VOLUME_MUTED;

    return (pa_volume_t) lrint(ceil((dB/USER_DECIBEL_RANGE+1.0)*PA_VOLUME_NORM));
}

double pa_sw_volume_to_dB(pa_volume_t v) {
    if (v == PA_VOLUME_MUTED)
        return PA_DECIBEL_MININFTY;

    return ((double) v/PA_VOLUME_NORM-1)*USER_DECIBEL_RANGE;
}

pa_volume_t pa_sw_volume_from_linear(double v) {

    if (v <= 0.0)
        return PA_VOLUME_MUTED;

    if (v > .999 && v < 1.001)
        return PA_VOLUME_NORM;

    return pa_sw_volume_from_dB(20.0*log10(v));
}

double pa_sw_volume_to_linear(pa_volume_t v) {

    if (v == PA_VOLUME_MUTED)
        return 0.0;

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

char *pa_volume_snprint(char *s, size_t l, pa_volume_t v) {
    pa_assert(s);
    pa_assert(l > 0);

    pa_init_i18n();

    if (v == (pa_volume_t) -1) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    pa_snprintf(s, l, "%3u%%", (v*100)/PA_VOLUME_NORM);
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
        double f = pa_sw_volume_to_dB(c->values[channel]);

        l -= pa_snprintf(e, l, "%s%u: %0.2f dB",
                         first ? "" : " ",
                         channel,
                         isinf(f) < 0 || f <= -USER_DECIBEL_RANGE ? -INFINITY : f);

        e = strchr(e, 0);
        first = FALSE;
    }

    return s;
}

char *pa_sw_volume_snprint_dB(char *s, size_t l, pa_volume_t v) {
    double f;

    pa_assert(s);
    pa_assert(l > 0);

    pa_init_i18n();

    if (v == (pa_volume_t) -1) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    f = pa_sw_volume_to_dB(v);
    pa_snprintf(s, l, "%0.2f dB",
                isinf(f) < 0 || f <= -USER_DECIBEL_RANGE ?  -INFINITY : f);

    return s;
}

int pa_cvolume_channels_equal_to(const pa_cvolume *a, pa_volume_t v) {
    unsigned c;
    pa_assert(a);

    pa_return_val_if_fail(pa_cvolume_valid(a), 0);

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

    pa_return_val_if_fail(pa_cvolume_valid(a), NULL);
    pa_return_val_if_fail(pa_cvolume_valid(b), NULL);

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

    pa_return_val_if_fail(pa_cvolume_valid(a), NULL);
    pa_return_val_if_fail(pa_cvolume_valid(b), NULL);

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

static pa_bool_t on_front(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_CENTER;
}

static pa_bool_t on_rear(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_REAR_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_CENTER;
}

pa_cvolume *pa_cvolume_remap(pa_cvolume *v, const pa_channel_map *from, const pa_channel_map *to) {
    int a, b;
    pa_cvolume result;

    pa_assert(v);
    pa_assert(from);
    pa_assert(to);

    pa_return_val_if_fail(pa_cvolume_valid(v), NULL);
    pa_return_val_if_fail(pa_channel_map_valid(from), NULL);
    pa_return_val_if_fail(pa_channel_map_valid(to), NULL);
    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, from), NULL);

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

    pa_return_val_if_fail(pa_cvolume_valid(v), 0);
    pa_return_val_if_fail(pa_sample_spec_valid(ss), 0);

    return v->channels == ss->channels;
}

int pa_cvolume_compatible_with_channel_map(const pa_cvolume *v, const pa_channel_map *cm) {
    pa_assert(v);
    pa_assert(cm);

    pa_return_val_if_fail(pa_cvolume_valid(v), 0);
    pa_return_val_if_fail(pa_channel_map_valid(cm), 0);

    return v->channels == cm->channels;
}

static void get_avg_lr(const pa_channel_map *map, const pa_cvolume *v, pa_volume_t *l, pa_volume_t *r) {
    int c;
    pa_volume_t left = 0, right = 0;
    unsigned n_left = 0, n_right = 0;

    pa_assert(v);
    pa_assert(map);
    pa_assert(map->channels == v->channels);
    pa_assert(l);
    pa_assert(r);

    for (c = 0; c < map->channels; c++) {
        if (on_left(map->map[c])) {
            left += v->values[c];
            n_left++;
        } else if (on_right(map->map[c])) {
            right += v->values[c];
            n_right++;
        }
    }

    if (n_left <= 0)
        *l = PA_VOLUME_NORM;
    else
        *l = left / n_left;

    if (n_right <= 0)
        *r = PA_VOLUME_NORM;
    else
        *r = right / n_right;
}

float pa_cvolume_get_balance(const pa_cvolume *v, const pa_channel_map *map) {
    pa_volume_t left, right;

    pa_assert(v);
    pa_assert(map);

    pa_return_val_if_fail(pa_cvolume_valid(v), 0.0f);
    pa_return_val_if_fail(pa_channel_map_valid(map), 0.0f);
    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), 0.0f);

    if (!pa_channel_map_can_balance(map))
        return 0.0f;

    get_avg_lr(map, v, &left, &right);

    if (left == right)
        return 0.0f;

    /*   1.0,  0.0  =>  -1.0
         0.0,  1.0  =>   1.0
         0.0,  0.0  =>   0.0
         0.5,  0.5  =>   0.0
         1.0,  0.5  =>  -0.5
         1.0,  0.25 => -0.75
         0.75, 0.25 => -0.66
         0.5,  0.25 => -0.5   */

    if (left > right)
        return -1.0f + ((float) right / (float) left);
    else
        return 1.0f - ((float) left / (float) right);
}

pa_cvolume* pa_cvolume_set_balance(pa_cvolume *v, const pa_channel_map *map, float new_balance) {
    pa_volume_t left, nleft, right, nright, m;
    unsigned c;

    pa_assert(map);
    pa_assert(v);
    pa_assert(new_balance >= -1.0f);
    pa_assert(new_balance <= 1.0f);

    pa_return_val_if_fail(pa_cvolume_valid(v), NULL);
    pa_return_val_if_fail(pa_channel_map_valid(map), NULL);
    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), NULL);

    if (!pa_channel_map_can_balance(map))
        return v;

    get_avg_lr(map, v, &left, &right);

    m = PA_MAX(left, right);

    if (new_balance <= 0) {
        nright  = (new_balance + 1.0f) * m;
        nleft = m;
    } else  {
        nleft = (1.0f - new_balance) * m;
        nright = m;
    }

    for (c = 0; c < map->channels; c++) {
        if (on_left(map->map[c])) {
            if (left == 0)
                v->values[c] = nleft;
            else
                v->values[c] = (pa_volume_t) (((uint64_t) v->values[c] * (uint64_t) nleft) / (uint64_t) left);
        } else if (on_right(map->map[c])) {
            if (right == 0)
                v->values[c] = nright;
            else
                v->values[c] = (pa_volume_t) (((uint64_t) v->values[c] * (uint64_t) nright) / (uint64_t) right);
        }
    }

    return v;
}

pa_cvolume* pa_cvolume_scale(pa_cvolume *v, pa_volume_t max) {
    unsigned c;
    pa_volume_t t = 0;

    pa_assert(v);

    pa_return_val_if_fail(pa_cvolume_valid(v), NULL);
    pa_return_val_if_fail(max != (pa_volume_t) -1, NULL);

    for (c = 0; c < v->channels; c++)
        if (v->values[c] > t)
            t = v->values[c];

    if (t <= 0)
        return pa_cvolume_set(v, v->channels, max);

    for (c = 0; c < v->channels; c++)
        v->values[c] = (pa_volume_t) (((uint64_t)  v->values[c] * (uint64_t) max) / (uint64_t) t);

    return v;
}

static void get_avg_fr(const pa_channel_map *map, const pa_cvolume *v, pa_volume_t *f, pa_volume_t *r) {
    int c;
    pa_volume_t front = 0, rear = 0;
    unsigned n_front = 0, n_rear = 0;

    pa_assert(v);
    pa_assert(map);
    pa_assert(map->channels == v->channels);
    pa_assert(f);
    pa_assert(r);

    for (c = 0; c < map->channels; c++) {
        if (on_front(map->map[c])) {
            front += v->values[c];
            n_front++;
        } else if (on_rear(map->map[c])) {
            rear += v->values[c];
            n_rear++;
        }
    }

    if (n_front <= 0)
        *f = PA_VOLUME_NORM;
    else
        *f = front / n_front;

    if (n_rear <= 0)
        *r = PA_VOLUME_NORM;
    else
        *r = rear / n_rear;
}

float pa_cvolume_get_fade(const pa_cvolume *v, const pa_channel_map *map) {
    pa_volume_t front, rear;

    pa_assert(v);
    pa_assert(map);

    pa_return_val_if_fail(pa_cvolume_valid(v), 0.0f);
    pa_return_val_if_fail(pa_channel_map_valid(map), 0.0f);
    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), 0.0f);

    if (!pa_channel_map_can_fade(map))
        return 0.0f;

    get_avg_fr(map, v, &front, &rear);

    if (front == rear)
        return 0.0f;

    if (rear > front)
        return -1.0f + ((float) front / (float) rear);
    else
        return 1.0f - ((float) rear / (float) front);
}

pa_cvolume* pa_cvolume_set_fade(pa_cvolume *v, const pa_channel_map *map, float new_fade) {
    pa_volume_t front, nfront, rear, nrear, m;
    unsigned c;

    pa_assert(map);
    pa_assert(v);
    pa_assert(new_fade >= -1.0f);
    pa_assert(new_fade <= 1.0f);

    pa_return_val_if_fail(pa_cvolume_valid(v), NULL);
    pa_return_val_if_fail(pa_channel_map_valid(map), NULL);
    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), NULL);

    if (!pa_channel_map_can_fade(map))
        return v;

    get_avg_fr(map, v, &front, &rear);

    m = PA_MAX(front, rear);

    if (new_fade <= 0) {
        nfront  = (new_fade + 1.0f) * m;
        nrear = m;
    } else  {
        nrear = (1.0f - new_fade) * m;
        nfront = m;
    }

    for (c = 0; c < map->channels; c++) {
        if (on_front(map->map[c])) {
            if (front == 0)
                v->values[c] = nfront;
            else
                v->values[c] = (pa_volume_t) (((uint64_t) v->values[c] * (uint64_t) nfront) / (uint64_t) front);
        } else if (on_rear(map->map[c])) {
            if (rear == 0)
                v->values[c] = nrear;
            else
                v->values[c] = (pa_volume_t) (((uint64_t) v->values[c] * (uint64_t) nrear) / (uint64_t) rear);
        }
    }

    return v;
}
