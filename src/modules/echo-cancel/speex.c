/***
    This file is part of PulseAudio.

    Copyright 2010 Wim Taymans <wim.taymans@gmail.com>

    Contributor: Arun Raghavan <arun.raghavan@collabora.co.uk>

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

#include "echo-cancel.h"

pa_bool_t pa_speex_ec_init(pa_echo_canceller *ec, pa_sample_spec ss, pa_channel_map map, uint32_t filter_size_ms, uint32_t frame_size_ms)
{
    int framelen, y, rate = ss.rate;

    framelen = (rate * frame_size_ms) / 1000;
    /* framelen should be a power of 2, round down to nearest power of two */
    y = 1 << ((8 * sizeof (int)) - 2);
    while (y > framelen)
      y >>= 1;
    framelen = y;

    ec->params.priv.speex.blocksize = framelen * pa_frame_size (&ss);

    pa_log_debug ("Using framelen %d, blocksize %lld, channels %d, rate %d", framelen, (long long) ec->params.priv.speex.blocksize, ss.channels, ss.rate);

    ec->params.priv.speex.state = speex_echo_state_init_mc (framelen, (rate * filter_size_ms) / 1000, ss.channels, ss.channels);

    if (ec->params.priv.speex.state) {
	speex_echo_ctl(ec->params.priv.speex.state, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	return TRUE;
    } else
	return FALSE;
}

void pa_speex_ec_run(pa_echo_canceller *ec, uint8_t *rec, uint8_t *play, uint8_t *out)
{
    speex_echo_cancellation(ec->params.priv.speex.state, (const spx_int16_t *) rec, (const spx_int16_t *) play, (spx_int16_t *) out);
}

void pa_speex_ec_done(pa_echo_canceller *ec)
{
    speex_echo_state_destroy (ec->params.priv.speex.state);
    ec->params.priv.speex.state = NULL;
}

uint32_t pa_speex_ec_get_block_size(pa_echo_canceller *ec)
{
    return ec->params.priv.speex.blocksize;
}
