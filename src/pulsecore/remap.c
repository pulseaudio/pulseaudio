/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk.com>

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

#include <string.h>

#include <pulse/sample.h>
#include <pulse/volume.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "remap.h"

static void remap_mono_to_stereo_s16ne_c(pa_remap_t *m, int16_t *dst, const int16_t *src, unsigned n) {
    unsigned i;

    for (i = n >> 2; i; i--) {
        dst[0] = dst[1] = src[0];
        dst[2] = dst[3] = src[1];
        dst[4] = dst[5] = src[2];
        dst[6] = dst[7] = src[3];
        src += 4;
        dst += 8;
    }
    for (i = n & 3; i; i--) {
        dst[0] = dst[1] = src[0];
        src++;
        dst += 2;
    }
}

static void remap_mono_to_stereo_float32ne_c(pa_remap_t *m, float *dst, const float *src, unsigned n) {
    unsigned i;

    for (i = n >> 2; i; i--) {
        dst[0] = dst[1] = src[0];
        dst[2] = dst[3] = src[1];
        dst[4] = dst[5] = src[2];
        dst[6] = dst[7] = src[3];
        src += 4;
        dst += 8;
    }
    for (i = n & 3; i; i--) {
        dst[0] = dst[1] = src[0];
        src++;
        dst += 2;
    }
}

static void remap_channels_matrix_s16ne_c(pa_remap_t *m, int16_t *dst, const int16_t *src, unsigned n) {
    unsigned oc, ic, i;
    unsigned n_ic, n_oc;

    n_ic = m->i_ss.channels;
    n_oc = m->o_ss.channels;

    memset(dst, 0, n * sizeof(int16_t) * n_oc);

    for (oc = 0; oc < n_oc; oc++) {

        for (ic = 0; ic < n_ic; ic++) {
            int16_t *d = dst + oc;
            const int16_t *s = src + ic;
            int32_t vol = m->map_table_i[oc][ic];

            if (vol <= 0)
                continue;

            if (vol >= 0x10000) {
                for (i = n; i > 0; i--, s += n_ic, d += n_oc)
                    *d += *s;
            } else {
                for (i = n; i > 0; i--, s += n_ic, d += n_oc)
                    *d += (int16_t) (((int32_t)*s * vol) >> 16);
            }
        }
    }
}

static void remap_channels_matrix_float32ne_c(pa_remap_t *m, float *dst, const float *src, unsigned n) {
    unsigned oc, ic, i;
    unsigned n_ic, n_oc;

    n_ic = m->i_ss.channels;
    n_oc = m->o_ss.channels;

    memset(dst, 0, n * sizeof(float) * n_oc);

    for (oc = 0; oc < n_oc; oc++) {

        for (ic = 0; ic < n_ic; ic++) {
            float *d = dst + oc;
            const float *s = src + ic;
            float vol = m->map_table_f[oc][ic];

            if (vol <= 0.0f)
                continue;

            if (vol >= 1.0f) {
                for (i = n; i > 0; i--, s += n_ic, d += n_oc)
                    *d += *s;
            } else {
                for (i = n; i > 0; i--, s += n_ic, d += n_oc)
                    *d += *s * vol;
            }
        }
    }
}

bool pa_setup_remap_arrange(const pa_remap_t *m, int8_t arrange[PA_CHANNELS_MAX]) {
    unsigned ic, oc;
    unsigned n_ic, n_oc;

    pa_assert(m);

    n_ic = m->i_ss.channels;
    n_oc = m->o_ss.channels;

    for (oc = 0; oc < n_oc; oc++) {
        arrange[oc] = -1;
        for (ic = 0; ic < n_ic; ic++) {
            int32_t vol = m->map_table_i[oc][ic];

            /* input channel is not used */
            if (vol == 0)
                continue;

            /* if mixing this channel, we cannot just rearrange */
            if (vol != 0x10000 || arrange[oc] >= 0)
                return false;

            arrange[oc] = ic;
        }
    }

    return true;
}

void pa_set_remap_func(pa_remap_t *m, pa_do_remap_func_t func_s16,
    pa_do_remap_func_t func_float) {

    pa_assert(m);

    if (m->format == PA_SAMPLE_S16NE)
        m->do_remap = func_s16;
    else if (m->format == PA_SAMPLE_FLOAT32NE)
        m->do_remap = func_float;
    else
        pa_assert_not_reached();
}

/* set the function that will execute the remapping based on the matrices */
static void init_remap_c(pa_remap_t *m) {
    unsigned n_oc, n_ic;

    n_oc = m->o_ss.channels;
    n_ic = m->i_ss.channels;

    /* find some common channel remappings, fall back to full matrix operation. */
    if (n_ic == 1 && n_oc == 2 &&
            m->map_table_i[0][0] == 0x10000 && m->map_table_i[1][0] == 0x10000) {

        pa_log_info("Using mono to stereo remapping");
        pa_set_remap_func(m, (pa_do_remap_func_t) remap_mono_to_stereo_s16ne_c,
            (pa_do_remap_func_t) remap_mono_to_stereo_float32ne_c);
    } else {
        pa_log_info("Using generic matrix remapping");

        pa_set_remap_func(m, (pa_do_remap_func_t) remap_channels_matrix_s16ne_c,
            (pa_do_remap_func_t) remap_channels_matrix_float32ne_c);
    }
}

/* default C implementation */
static pa_init_remap_func_t init_remap_func = init_remap_c;

void pa_init_remap_func(pa_remap_t *m) {
    pa_assert(init_remap_func);

    m->do_remap = NULL;

    /* call the installed remap init function */
    init_remap_func(m);

    if (m->do_remap == NULL) {
        /* nothing was installed, fallback to C version */
        init_remap_c(m);
    }
}

pa_init_remap_func_t pa_get_init_remap_func(void) {
    return init_remap_func;
}

void pa_set_init_remap_func(pa_init_remap_func_t func) {
    init_remap_func = func;
}
