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
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include <liboil/liboilfuncs.h>

#include "log.h"
#include "sample-util.h"

pa_memblock *pa_silence_memblock(pa_memblock* b, const pa_sample_spec *spec) {
    assert(b && b->data && spec);
    pa_silence_memory(b->data, b->length, spec);
    return b;
}

void pa_silence_memchunk(pa_memchunk *c, const pa_sample_spec *spec) {
    assert(c && c->memblock && c->memblock->data && spec && c->length);

    pa_silence_memory((uint8_t*) c->memblock->data+c->index, c->length, spec);
}

void pa_silence_memory(void *p, size_t length, const pa_sample_spec *spec) {
    uint8_t c = 0;
    assert(p && length && spec);

    switch (spec->format) {
        case PA_SAMPLE_U8:
            c = 0x80;
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

size_t pa_mix(
    const pa_mix_info streams[],
    unsigned nstreams,
    void *data,
    size_t length,
    const pa_sample_spec *spec,
    const pa_cvolume *volume) {
    
    assert(streams && data && length && spec);

    switch (spec->format) {
        case PA_SAMPLE_S16NE:{
            size_t d;
            unsigned channel = 0;
            
            for (d = 0;; d += sizeof(int16_t)) {
                int32_t sum = 0;
                
                if (d >= length)
                    return d;

                if (volume->values[channel] != PA_VOLUME_MUTED) {
                    unsigned i;
                    
                    for (i = 0; i < nstreams; i++) {
                        int32_t v;
                        pa_volume_t cvolume = streams[i].volume.values[channel];
                        
                        if (d >= streams[i].chunk.length)
                            return d;
                        
                        if (cvolume == PA_VOLUME_MUTED)
                            v = 0;
                        else {
                            v = *((int16_t*) ((uint8_t*) streams[i].chunk.memblock->data + streams[i].chunk.index + d));
                            
                            if (cvolume != PA_VOLUME_NORM) {
                                v *= cvolume;
                                v /= PA_VOLUME_NORM;
                            }
                        }
                        
                        sum += v;
                    }
                
                    if (volume->values[channel] != PA_VOLUME_NORM) {
                        sum *= volume->values[channel];
                        sum /= PA_VOLUME_NORM;
                    }

                    if (sum < -0x8000) sum = -0x8000;
                    if (sum > 0x7FFF) sum = 0x7FFF;

                }
                
                *((int16_t*) data) = sum;
                data = (uint8_t*) data + sizeof(int16_t);
                
                if (++channel >= spec->channels)
                    channel = 0;
            }
        }
            
        case PA_SAMPLE_U8: {
            size_t d;
            unsigned channel = 0;
            
            for (d = 0;; d ++) {
                int32_t sum = 0;
                
                if (d >= length)
                    return d;

                if (volume->values[channel] != PA_VOLUME_MUTED) {
                    unsigned i;
                    
                    for (i = 0; i < nstreams; i++) {
                        int32_t v;
                        pa_volume_t cvolume = streams[i].volume.values[channel];
                        
                        if (d >= streams[i].chunk.length)
                            return d;
                        
                        if (cvolume == PA_VOLUME_MUTED)
                            v = 0;
                        else {
                            v = (int32_t) *((uint8_t*) streams[i].chunk.memblock->data + streams[i].chunk.index + d) - 0x80;
                            
                            if (cvolume != PA_VOLUME_NORM) {
                                v *= cvolume;
                                v /= PA_VOLUME_NORM;
                            }
                        }

                        sum += v;
                    }

                    if (volume->values[channel] != PA_VOLUME_NORM) {
                        sum *= volume->values[channel];
                        sum /= PA_VOLUME_NORM;
                    }

                    if (sum < -0x80) sum = -0x80;
                    if (sum > 0x7F) sum = 0x7F;

                }
                
                *((uint8_t*) data) = (uint8_t) (sum + 0x80);
                data = (uint8_t*) data + 1;
                
                if (++channel >= spec->channels)
                    channel = 0;
            }
        }
            
        case PA_SAMPLE_FLOAT32NE: {
            size_t d;
            unsigned channel = 0;
            
            for (d = 0;; d += sizeof(float)) {
                float sum = 0;
                
                if (d >= length)
                    return d;
                
                if (volume->values[channel] != PA_VOLUME_MUTED) {
                    unsigned i;
                    
                    for (i = 0; i < nstreams; i++) {
                        float v;
                        pa_volume_t cvolume = streams[i].volume.values[channel];
                        
                        if (d >= streams[i].chunk.length)
                            return d;
                        
                        if (cvolume == PA_VOLUME_MUTED)
                            v = 0;
                        else {
                            v = *((float*) ((uint8_t*) streams[i].chunk.memblock->data + streams[i].chunk.index + d));
                            
                            if (cvolume != PA_VOLUME_NORM) {
                                v *= cvolume;
                                v /= PA_VOLUME_NORM;
                            }
                        }
                        
                        sum += v;
                    }
            
                    if (volume->values[channel] != PA_VOLUME_NORM) {
                        sum *= volume->values[channel];
                        sum /= PA_VOLUME_NORM;
                    }
                }
            
                *((float*) data) = sum;
                data = (uint8_t*) data + sizeof(float);

                if (++channel >= spec->channels)
                    channel = 0;
            }
        }
            
        default:
            abort();
    }
}


void pa_volume_memchunk(pa_memchunk*c, const pa_sample_spec *spec, const pa_cvolume *volume) {
    assert(c && spec && (c->length % pa_frame_size(spec) == 0));
    assert(volume);

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_NORM))
        return;

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_MUTED)) {
        pa_silence_memchunk(c, spec);
        return;
    }

    switch (spec->format) {
        case PA_SAMPLE_S16NE: {
            int16_t *d;
            size_t n;
            unsigned channel = 0;
            
            for (d = (int16_t*) ((uint8_t*) c->memblock->data+c->index), n = c->length/sizeof(int16_t); n > 0; d++, n--) {
                int32_t t = (int32_t)(*d);
                
                t *= volume->values[channel];
                t /= PA_VOLUME_NORM;
                
                if (t < -0x8000) t = -0x8000;
                if (t > 0x7FFF) t = 0x7FFF;
                
                *d = (int16_t) t;
                
                if (++channel >= spec->channels)
                    channel = 0;
            }
        }
            
        case PA_SAMPLE_U8: {
            uint8_t *d;
            size_t n;
            unsigned channel = 0;
            
            for (d = (uint8_t*) c->memblock->data + c->index, n = c->length; n > 0; d++, n--) {
                int32_t t = (int32_t) *d - 0x80;
                
                t *= volume->values[channel];
                t /= PA_VOLUME_NORM;
                
                if (t < -0x80) t = -0x80;
                if (t > 0x7F) t = 0x7F;
                
                *d = (uint8_t) (t + 0x80);
                
                if (++channel >= spec->channels)
                    channel = 0;
            }
        }
            
        case PA_SAMPLE_FLOAT32NE: {
            float *d;
            int skip;
            unsigned n;
            unsigned channel;
        
            d = (float*) ((uint8_t*) c->memblock->data + c->index);
            skip = spec->channels * sizeof(float);
            n = c->length/sizeof(float)/spec->channels;
            
            for (channel = 0; channel < spec->channels ; channel ++) {
                float v, *t;
                
                if (volume->values[channel] == PA_VOLUME_NORM)
                    continue;
                
                v = (float) volume->values[channel] / PA_VOLUME_NORM;
                
                t = d + channel;
                oil_scalarmult_f32(t, skip, t, skip, &v, n);
            }
        }

        default:
            pa_log_error(__FILE__": ERROR: Unable to change volume of format %s.\n",
                pa_sample_format_to_string(spec->format));
            abort();
    }
}

