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
#include <string.h>
#include <assert.h>

#include "sample-util.h"

struct pa_memblock *pa_silence_memblock(struct pa_memblock* b, const struct pa_sample_spec *spec) {
    assert(b && b->data && spec);
    pa_silence_memory(b->data, b->length, spec);
    return b;
}

void pa_silence_memchunk(struct pa_memchunk *c, const struct pa_sample_spec *spec) {
    assert(c && c->memblock && c->memblock->data && spec && c->length);
    pa_silence_memory(c->memblock->data+c->index, c->length, spec);
}

void pa_silence_memory(void *p, size_t length, const struct pa_sample_spec *spec) {
    char c = 0;
    assert(p && length && spec);

    switch (spec->format) {
        case PA_SAMPLE_U8:
            c = 127;
            break;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_FLOAT32:
            c = 0;
            break;
        case PA_SAMPLE_ALAW:
        case PA_SAMPLE_ULAW:
            c = 80;
            break;
        default:
            assert(0);
    }
                
    memset(p, c, length);
}

size_t pa_mix(struct pa_mix_info channels[], unsigned nchannels, void *data, size_t length, const struct pa_sample_spec *spec, pa_volume_t volume) {
    unsigned c, d;
    assert(channels && data && length && spec);
    assert(spec->format == PA_SAMPLE_S16NE);

    for (d = 0;; d += sizeof(int16_t)) {
        int32_t sum = 0;

        if (d >= length)
            return d;
        
        for (c = 0; c < nchannels; c++) {
            int32_t v;
            uint32_t volume = channels[c].volume;
            
            if (d >= channels[c].chunk.length)
                return d;

            if (volume == PA_VOLUME_MUTED)
                v = 0;
            else {
                v = *((int16_t*) (channels[c].chunk.memblock->data + channels[c].chunk.index + d));

                if (volume != PA_VOLUME_NORM)
                    v = (int32_t) ((float)v*volume/PA_VOLUME_NORM);
            }

            sum += v;
        }

        if (volume == PA_VOLUME_MUTED)
            sum = 0;
        else if (volume != PA_VOLUME_NORM)
            sum = (int32_t) ((float) sum*volume/PA_VOLUME_NORM);
        
        if (sum < -0x8000) sum = -0x8000;
        if (sum > 0x7FFF) sum = 0x7FFF;
        
        *((int16_t*) data) = sum;
        data += sizeof(int16_t);
    }
}


void pa_volume_memchunk(struct pa_memchunk*c, const struct pa_sample_spec *spec, pa_volume_t volume) {
    int16_t *d;
    size_t n;
    assert(c && spec && (c->length % pa_frame_size(spec) == 0));
    assert(spec->format == PA_SAMPLE_S16NE);

    if (volume == PA_VOLUME_NORM)
        return;

    if (volume == PA_VOLUME_MUTED) {
        pa_silence_memchunk(c, spec);
        return;
    }

    for (d = (c->memblock->data+c->index), n = c->length/sizeof(int16_t); n > 0; d++, n--) {
        int32_t t = (int32_t)(*d);

        t *= volume;
        t /= PA_VOLUME_NORM;

        if (t < -0x8000) t = -0x8000;
        if (t > 0x7FFF) t = 0x7FFF;
        
        *d = (int16_t) t;
    }
}

