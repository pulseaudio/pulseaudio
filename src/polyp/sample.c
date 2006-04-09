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

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#include "sample.h"

size_t pa_sample_size(const pa_sample_spec *spec) {
    assert(spec);

    switch (spec->format) {
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ULAW:
        case PA_SAMPLE_ALAW:
            return 1;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
            return 2;
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
            return 4;
        default:
            assert(0);
    }
}

size_t pa_frame_size(const pa_sample_spec *spec) {
    assert(spec);

    return pa_sample_size(spec) * spec->channels;
}

size_t pa_bytes_per_second(const pa_sample_spec *spec) {
    assert(spec);
    return spec->rate*pa_frame_size(spec);
}

pa_usec_t pa_bytes_to_usec(uint64_t length, const pa_sample_spec *spec) {
    assert(spec);

    return (pa_usec_t) (((double) length/pa_frame_size(spec)*1000000)/spec->rate);
}

int pa_sample_spec_valid(const pa_sample_spec *spec) {
    assert(spec);

    if (spec->rate <= 0 ||
        spec->channels <= 0 ||
        spec->channels > PA_CHANNELS_MAX ||
        spec->format >= PA_SAMPLE_MAX ||
        spec->format < 0)
        return 0;

    return 1;
}

int pa_sample_spec_equal(const pa_sample_spec*a, const pa_sample_spec*b) {
    assert(a && b);

    return (a->format == b->format) && (a->rate == b->rate) && (a->channels == b->channels);
}

const char *pa_sample_format_to_string(pa_sample_format_t f) {
    static const char* const table[]= {
        [PA_SAMPLE_U8] = "u8",
        [PA_SAMPLE_ALAW] = "aLaw",
        [PA_SAMPLE_ULAW] = "uLaw",
        [PA_SAMPLE_S16LE] = "s16le",
        [PA_SAMPLE_S16BE] = "s16be",
        [PA_SAMPLE_FLOAT32LE] = "float32le",
        [PA_SAMPLE_FLOAT32BE] = "float32be",
    };

    if (f >= PA_SAMPLE_MAX)
        return NULL;

    return table[f];
}

char *pa_sample_spec_snprint(char *s, size_t l, const pa_sample_spec *spec) {
    assert(s && l && spec);
    
    if (!pa_sample_spec_valid(spec))
        snprintf(s, l, "Invalid");
    else
        snprintf(s, l, "%s %uch %uHz", pa_sample_format_to_string(spec->format), spec->channels, spec->rate);

    return s;
}

void pa_bytes_snprint(char *s, size_t l, unsigned v) {
    if (v >= ((unsigned) 1024)*1024*1024)
        snprintf(s, l, "%0.1f GiB", ((double) v)/1024/1024/1024);
    else if (v >= ((unsigned) 1024)*1024)
        snprintf(s, l, "%0.1f MiB", ((double) v)/1024/1024);
    else if (v >= (unsigned) 1024)
        snprintf(s, l, "%0.1f KiB", ((double) v)/1024);
    else
        snprintf(s, l, "%u B", (unsigned) v);
}

pa_sample_format_t pa_parse_sample_format(const char *format) {
    
    if (strcasecmp(format, "s16le") == 0)
        return PA_SAMPLE_S16LE;
    else if (strcasecmp(format, "s16be") == 0)
        return PA_SAMPLE_S16BE;
    else if (strcasecmp(format, "s16ne") == 0 || strcasecmp(format, "s16") == 0 || strcasecmp(format, "16") == 0)
        return PA_SAMPLE_S16NE;
    else if (strcasecmp(format, "u8") == 0 || strcasecmp(format, "8") == 0)
        return PA_SAMPLE_U8;
    else if (strcasecmp(format, "float32") == 0 || strcasecmp(format, "float32ne") == 0)
        return PA_SAMPLE_FLOAT32;
    else if (strcasecmp(format, "float32le") == 0)
        return PA_SAMPLE_FLOAT32LE;
    else if (strcasecmp(format, "float32be") == 0)
        return PA_SAMPLE_FLOAT32BE;
    else if (strcasecmp(format, "ulaw") == 0)
        return PA_SAMPLE_ULAW;
    else if (strcasecmp(format, "alaw") == 0)
        return PA_SAMPLE_ALAW;

    return -1;
}
