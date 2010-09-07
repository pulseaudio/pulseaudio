/***
    This file is part of PulseAudio.

    Copyright 2010 Arun Raghavan <arun.raghavan@collabora.co.uk>

    Contributor: Wim Taymans <wim.taymans@gmail.com>

    The actual implementation is taken from the sources at
    http://andreadrian.de/intercom/ - for the license, look for
    adrian-license.txt in the same directory as this file.

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

#include <pulsecore/modargs.h>
#include "echo-cancel.h"

/* should be between 10-20 ms */
#define DEFAULT_FRAME_SIZE_MS 20

static const char* const valid_modargs[] = {
    "frame_size_ms",
    NULL
};

static void pa_adrian_ec_fixate_spec(pa_sample_spec *source_ss, pa_channel_map *source_map,
				    pa_sample_spec *sink_ss, pa_channel_map *sink_map)
{
    source_ss->format = PA_SAMPLE_S16LE;
    source_ss->channels = 1;
    pa_channel_map_init_mono(source_map);

    *sink_ss = *source_ss;
    *sink_map = *source_map;
}

pa_bool_t pa_adrian_ec_init(pa_echo_canceller *ec,
                           pa_sample_spec *source_ss, pa_channel_map *source_map,
                           pa_sample_spec *sink_ss, pa_channel_map *sink_map,
                           uint32_t *blocksize, const char *args)
{
    int framelen, rate;
    uint32_t frame_size_ms;
    pa_modargs *ma;

    if (!(ma = pa_modargs_new(args, valid_modargs))) {
        pa_log("Failed to parse submodule arguments.");
        goto fail;
    }

    frame_size_ms = DEFAULT_FRAME_SIZE_MS;
    if (pa_modargs_get_value_u32(ma, "frame_size_ms", &frame_size_ms) < 0 || frame_size_ms < 1 || frame_size_ms > 200) {
        pa_log("Invalid frame_size_ms specification");
        goto fail;
    }

    pa_adrian_ec_fixate_spec(source_ss, source_map, sink_ss, sink_map);

    rate = source_ss->rate;
    framelen = (rate * frame_size_ms) / 1000;

    *blocksize = ec->params.priv.adrian.blocksize = framelen * pa_frame_size (source_ss);

    pa_log_debug ("Using framelen %d, blocksize %u, channels %d, rate %d", framelen, ec->params.priv.adrian.blocksize, source_ss->channels, source_ss->rate);

    ec->params.priv.adrian.aec = AEC_init(rate);
    if (!ec->params.priv.adrian.aec)
	goto fail;

    pa_modargs_free(ma);
    return TRUE;

fail:
    if (ma)
	pa_modargs_free(ma);
    return FALSE;
}

void pa_adrian_ec_run(pa_echo_canceller *ec, uint8_t *rec, uint8_t *play, uint8_t *out)
{
    unsigned int i;

    for (i = 0; i < ec->params.priv.adrian.blocksize; i += 2) {
        /* We know it's S16LE mono data */
        int r = (((int8_t) rec[i + 1]) << 8) | rec[i];
        int p = (((int8_t) play[i + 1]) << 8) | play[i];
        int res;

        res = AEC_doAEC(ec->params.priv.adrian.aec, r, p);
        out[i] = (uint8_t) (res & 0xff);
        out[i + 1] = (uint8_t) ((res >> 8) & 0xff);
    }
}

void pa_adrian_ec_done(pa_echo_canceller *ec)
{
    pa_xfree(ec->params.priv.adrian.aec);
    ec->params.priv.adrian.aec = NULL;
}
