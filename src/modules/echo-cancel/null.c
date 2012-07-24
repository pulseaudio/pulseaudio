/***
    Copyright 2012 Peter Meerwald <p.meerwald@bct-electronic.com>

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/cdecl.h>

PA_C_DECL_BEGIN
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include "echo-cancel.h"
PA_C_DECL_END

pa_bool_t pa_null_ec_init(pa_core *c, pa_echo_canceller *ec,
                           pa_sample_spec *source_ss, pa_channel_map *source_map,
                           pa_sample_spec *sink_ss, pa_channel_map *sink_map,
                           uint32_t *blocksize, const char *args) {
    unsigned framelen = 256;

    source_ss->format = PA_SAMPLE_S16NE;
    *sink_ss = *source_ss;
    *sink_map = *source_map;

    *blocksize = framelen * pa_frame_size(source_ss);

    pa_log_debug("null AEC: framelen %u, blocksize %u, channels %d, rate %d", framelen, *blocksize, source_ss->channels, source_ss->rate);

    return TRUE;
}

void pa_null_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out) {
    memcpy(out, rec, 256 * 2);
}

void pa_null_ec_done(pa_echo_canceller *ec) {
}
