/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
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

#include "sample.h"

size_t pa_frame_size(const struct pa_sample_spec *spec) {
    size_t b = 1;
    assert(spec);

    switch (spec->format) {
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ULAW:
        case PA_SAMPLE_ALAW:
            b = 1;
            break;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
            b = 2;
            break;
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
            b = 4;
            break;
        default:
            assert(0);
    }

    return b * spec->channels;
}

size_t pa_bytes_per_second(const struct pa_sample_spec *spec) {
    assert(spec);
    return spec->rate*pa_frame_size(spec);
}

pa_usec_t pa_bytes_to_usec(size_t length, const struct pa_sample_spec *spec) {
    assert(spec);

    return (pa_usec_t) (((double) length/pa_frame_size(spec)*1000000)/spec->rate);
}

int pa_sample_spec_valid(const struct pa_sample_spec *spec) {
    assert(spec);

    if (!spec->rate || !spec->channels)
        return 0;

    if (spec->format >= PA_SAMPLE_MAX)
        return 0;

    return 1;
}

int pa_sample_spec_equal(const struct pa_sample_spec*a, const struct pa_sample_spec*b) {
    assert(a && b);

    return (a->format == b->format) && (a->rate == b->rate) && (a->channels == b->channels);
}

void pa_sample_spec_snprint(char *s, size_t l, const struct pa_sample_spec *spec) {
    static const char* const table[]= {
        [PA_SAMPLE_U8] = "U8",
        [PA_SAMPLE_ALAW] = "ALAW",
        [PA_SAMPLE_ULAW] = "ULAW",
        [PA_SAMPLE_S16LE] = "S16LE",
        [PA_SAMPLE_S16BE] = "S16BE",
        [PA_SAMPLE_FLOAT32LE] = "FLOAT32LE",
        [PA_SAMPLE_FLOAT32BE] = "FLOAT32BE",
    };

    if (!pa_sample_spec_valid(spec)) {
        snprintf(s, l, "Invalid");
        return;
    }
    
    snprintf(s, l, "%s %uch %uHz", table[spec->format], spec->channels, spec->rate);
}

pa_volume_t pa_volume_multiply(pa_volume_t a, pa_volume_t b) {
    uint64_t p = a;
    p *= b;
    p /= PA_VOLUME_NORM;

    return (pa_volume_t) p;
}

pa_volume_t pa_volume_from_dB(double f) {
    if (f <= PA_DECIBEL_MININFTY)
        return PA_VOLUME_MUTED;

    return (pa_volume_t) (pow(10, f/20)*PA_VOLUME_NORM);
}

double pa_volume_to_dB(pa_volume_t v) {
    if (v == PA_VOLUME_MUTED)
        return PA_DECIBEL_MININFTY;

    return 20*log10((double) v/PA_VOLUME_NORM);
}

void pa_bytes_snprint(char *s, size_t l, unsigned v) {
    if (v >= ((unsigned) 1024)*1024*1024)
        snprintf(s, l, "%0.1f GB", ((double) v)/1024/1024/1024);
    else if (v >= ((unsigned) 1024)*1024)
        snprintf(s, l, "%0.1f MB", ((double) v)/1024/1024);
    else if (v >= (unsigned) 1024)
        snprintf(s, l, "%0.1f KB", ((double) v)/1024);
    else
        snprintf(s, l, "%u B", (unsigned) v);
}
