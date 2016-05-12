/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "rtp.h"

#include <pulsecore/core-util.h>

uint8_t pa_rtp_payload_from_sample_spec(const pa_sample_spec *ss) {
    pa_assert(ss);

    if (ss->format == PA_SAMPLE_S16BE && ss->rate == 44100 && ss->channels == 2)
        return 10;
    if (ss->format == PA_SAMPLE_S16BE && ss->rate == 44100 && ss->channels == 1)
        return 11;

    return 127;
}

pa_sample_spec *pa_rtp_sample_spec_from_payload(uint8_t payload, pa_sample_spec *ss) {
    pa_assert(ss);

    switch (payload) {
        case 10:
            ss->channels = 2;
            ss->format = PA_SAMPLE_S16BE;
            ss->rate = 44100;
            break;

        case 11:
            ss->channels = 1;
            ss->format = PA_SAMPLE_S16BE;
            ss->rate = 44100;
            break;

        default:
            return NULL;
    }

    return ss;
}

pa_sample_spec *pa_rtp_sample_spec_fixup(pa_sample_spec * ss) {
    pa_assert(ss);

    if (!pa_rtp_sample_spec_valid(ss))
        ss->format = PA_SAMPLE_S16BE;

    pa_assert(pa_rtp_sample_spec_valid(ss));
    return ss;
}

int pa_rtp_sample_spec_valid(const pa_sample_spec *ss) {
    pa_assert(ss);

    if (!pa_sample_spec_valid(ss))
        return 0;

    return ss->format == PA_SAMPLE_S16BE;
}

const char* pa_rtp_format_to_string(pa_sample_format_t f) {
    switch (f) {
        case PA_SAMPLE_S16BE:
            return "L16";
        default:
            return NULL;
    }
}

pa_sample_format_t pa_rtp_string_to_format(const char *s) {
    pa_assert(s);

    if (pa_streq(s, "L16"))
        return PA_SAMPLE_S16BE;
    else
        return PA_SAMPLE_INVALID;
}
