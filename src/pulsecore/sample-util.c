/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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
#include <stdlib.h>

#include <liboil/liboilfuncs.h>
#include <liboil/liboil.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "sample-util.h"
#include "endianmacros.h"

#define PA_SILENCE_MAX (PA_PAGE_SIZE*16)

pa_memblock *pa_silence_memblock_new(pa_mempool *pool, const pa_sample_spec *spec, size_t length) {
    size_t fs;
    pa_assert(pool);
    pa_assert(spec);

    if (length <= 0)
        length = pa_bytes_per_second(spec)/20; /* 50 ms */

    if (length > PA_SILENCE_MAX)
        length = PA_SILENCE_MAX;

    fs = pa_frame_size(spec);

    length = (length+fs-1)/fs;

    if (length <= 0)
        length = 1;

    length *= fs;

    return pa_silence_memblock(pa_memblock_new(pool, length), spec);
}

pa_memblock *pa_silence_memblock(pa_memblock* b, const pa_sample_spec *spec) {
    void *data;

    pa_assert(b);
    pa_assert(spec);

    data = pa_memblock_acquire(b);
    pa_silence_memory(data, pa_memblock_get_length(b), spec);
    pa_memblock_release(b);
    return b;
}

void pa_silence_memchunk(pa_memchunk *c, const pa_sample_spec *spec) {
    void *data;

    pa_assert(c);
    pa_assert(c->memblock);
    pa_assert(spec);

    data = pa_memblock_acquire(c->memblock);
    pa_silence_memory((uint8_t*) data+c->index, c->length, spec);
    pa_memblock_release(c->memblock);
}

void pa_silence_memory(void *p, size_t length, const pa_sample_spec *spec) {
    uint8_t c = 0;
    pa_assert(p);
    pa_assert(length > 0);
    pa_assert(spec);

    switch (spec->format) {
        case PA_SAMPLE_U8:
            c = 0x80;
            break;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_FLOAT32:
        case PA_SAMPLE_FLOAT32RE:
            c = 0;
            break;
        case PA_SAMPLE_ALAW:
            c = 0xd5;
            break;
        case PA_SAMPLE_ULAW:
            c = 0xff;
            break;
        default:
            pa_assert_not_reached();
    }

    memset(p, c, length);
}

size_t pa_mix(
        pa_mix_info streams[],
        unsigned nstreams,
        void *data,
        size_t length,
        const pa_sample_spec *spec,
        const pa_cvolume *volume,
        int mute) {

    pa_cvolume full_volume;
    size_t d = 0;
    unsigned k;

    pa_assert(streams);
    pa_assert(data);
    pa_assert(length);
    pa_assert(spec);

    if (!volume)
        volume = pa_cvolume_reset(&full_volume, spec->channels);

    for (k = 0; k < nstreams; k++)
        streams[k].internal = pa_memblock_acquire(streams[k].chunk.memblock);

    switch (spec->format) {
        case PA_SAMPLE_S16NE:{
            unsigned channel = 0;

            for (d = 0;; d += sizeof(int16_t)) {
                int32_t sum = 0;

                if (d >= length)
                    goto finish;

                if (!mute && volume->values[channel] != PA_VOLUME_MUTED) {
                    unsigned i;

                    for (i = 0; i < nstreams; i++) {
                        int32_t v;
                        pa_volume_t cvolume = streams[i].volume.values[channel];

                        if (d >= streams[i].chunk.length)
                            goto finish;

                        if (cvolume == PA_VOLUME_MUTED)
                            v = 0;
                        else {
                            v = *((int16_t*) ((uint8_t*) streams[i].internal + streams[i].chunk.index + d));

                            if (cvolume != PA_VOLUME_NORM)
                                v = (int32_t) (v * pa_sw_volume_to_linear(cvolume));
                        }

                        sum += v;
                    }

                    if (volume->values[channel] != PA_VOLUME_NORM)
                        sum = (int32_t) (sum * pa_sw_volume_to_linear(volume->values[channel]));

                    sum = CLAMP(sum, -0x8000, 0x7FFF);
                }

                *((int16_t*) data) = (int16_t) sum;
                data = (uint8_t*) data + sizeof(int16_t);

                if (++channel >= spec->channels)
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S16RE:{
            unsigned channel = 0;

            for (d = 0;; d += sizeof(int16_t)) {
                int32_t sum = 0;

                if (d >= length)
                    goto finish;

                if (!mute && volume->values[channel] != PA_VOLUME_MUTED) {
                    unsigned i;

                    for (i = 0; i < nstreams; i++) {
                        int32_t v;
                        pa_volume_t cvolume = streams[i].volume.values[channel];

                        if (d >= streams[i].chunk.length)
                            goto finish;

                        if (cvolume == PA_VOLUME_MUTED)
                            v = 0;
                        else {
                            v = PA_INT16_SWAP(*((int16_t*) ((uint8_t*) streams[i].internal + streams[i].chunk.index + d)));

                            if (cvolume != PA_VOLUME_NORM)
                                v = (int32_t) (v * pa_sw_volume_to_linear(cvolume));
                        }

                        sum += v;
                    }

                    if (volume->values[channel] != PA_VOLUME_NORM)
                        sum = (int32_t) (sum * pa_sw_volume_to_linear(volume->values[channel]));

                    sum = CLAMP(sum, -0x8000, 0x7FFF);
                }

                *((int16_t*) data) = PA_INT16_SWAP((int16_t) sum);
                data = (uint8_t*) data + sizeof(int16_t);

                if (++channel >= spec->channels)
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_U8: {
            unsigned channel = 0;

            for (d = 0;; d ++) {
                int32_t sum = 0;

                if (d >= length)
                    goto finish;

                if (!mute && volume->values[channel] != PA_VOLUME_MUTED) {
                    unsigned i;

                    for (i = 0; i < nstreams; i++) {
                        int32_t v;
                        pa_volume_t cvolume = streams[i].volume.values[channel];

                        if (d >= streams[i].chunk.length)
                            goto finish;

                        if (cvolume == PA_VOLUME_MUTED)
                            v = 0;
                        else {
                            v = (int32_t) *((uint8_t*) streams[i].internal + streams[i].chunk.index + d) - 0x80;

                            if (cvolume != PA_VOLUME_NORM)
                                v = (int32_t) (v * pa_sw_volume_to_linear(cvolume));
                        }

                        sum += v;
                    }

                    if (volume->values[channel] != PA_VOLUME_NORM)
                        sum = (int32_t) (sum * pa_sw_volume_to_linear(volume->values[channel]));

                    sum = CLAMP(sum, -0x80, 0x7F);
                }

                *((uint8_t*) data) = (uint8_t) (sum + 0x80);
                data = (uint8_t*) data + 1;

                if (++channel >= spec->channels)
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_FLOAT32NE: {
            unsigned channel = 0;

            for (d = 0;; d += sizeof(float)) {
                float sum = 0;

                if (d >= length)
                    goto finish;

                if (!mute && volume->values[channel] != PA_VOLUME_MUTED) {
                    unsigned i;

                    for (i = 0; i < nstreams; i++) {
                        float v;
                        pa_volume_t cvolume = streams[i].volume.values[channel];

                        if (d >= streams[i].chunk.length)
                            goto finish;

                        if (cvolume == PA_VOLUME_MUTED)
                            v = 0;
                        else {
                            v = *((float*) ((uint8_t*) streams[i].internal + streams[i].chunk.index + d));

                            if (cvolume != PA_VOLUME_NORM)
                                v *= pa_sw_volume_to_linear(cvolume);
                        }

                        sum += v;
                    }

                    if (volume->values[channel] != PA_VOLUME_NORM)
                        sum *= pa_sw_volume_to_linear(volume->values[channel]);
                }

                *((float*) data) = sum;
                data = (uint8_t*) data + sizeof(float);

                if (++channel >= spec->channels)
                    channel = 0;
            }

            break;
        }

        default:
            pa_log_error("ERROR: Unable to mix audio data of format %s.", pa_sample_format_to_string(spec->format));
            abort();
    }

finish:

    for (k = 0; k < nstreams; k++)
        pa_memblock_release(streams[k].chunk.memblock);

    return d;
}


void pa_volume_memchunk(
        pa_memchunk*c,
        const pa_sample_spec *spec,
        const pa_cvolume *volume) {

    void *ptr;

    pa_assert(c);
    pa_assert(spec);
    pa_assert(c->length % pa_frame_size(spec) == 0);
    pa_assert(volume);

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_NORM))
        return;

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_MUTED)) {
        pa_silence_memchunk(c, spec);
        return;
    }

    ptr = pa_memblock_acquire(c->memblock);

    switch (spec->format) {

        case PA_SAMPLE_S16NE: {
            int16_t *d;
            size_t n;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            for (channel = 0; channel < spec->channels; channel++)
                linear[channel] = (int32_t) (pa_sw_volume_to_linear(volume->values[channel]) * 0x10000);

            for (channel = 0, d = (int16_t*) ((uint8_t*) ptr + c->index), n = c->length/sizeof(int16_t); n > 0; d++, n--) {
                int32_t t;

                t = (int32_t)(*d);
                t = (t * linear[channel]) / 0x10000;
                t = CLAMP(t, -0x8000, 0x7FFF);
                *d = (int16_t) t;

                if (++channel >= spec->channels)
                    channel = 0;
            }
            break;
        }

        case PA_SAMPLE_S16RE: {
            int16_t *d;
            size_t n;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            for (channel = 0; channel < spec->channels; channel++)
                linear[channel] = (int32_t) (pa_sw_volume_to_linear(volume->values[channel]) * 0x10000);

            for (channel = 0, d = (int16_t*) ((uint8_t*) ptr + c->index), n = c->length/sizeof(int16_t); n > 0; d++, n--) {
                int32_t t;

                t = (int32_t)(PA_INT16_SWAP(*d));
                t = (t * linear[channel]) / 0x10000;
                t = CLAMP(t, -0x8000, 0x7FFF);
                *d = PA_INT16_SWAP((int16_t) t);

                if (++channel >= spec->channels)
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_U8: {
            uint8_t *d;
            size_t n;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            for (channel = 0; channel < spec->channels; channel++)
                linear[channel] = (int32_t) (pa_sw_volume_to_linear(volume->values[channel]) * 0x10000);

            for (channel = 0, d = (uint8_t*) ptr + c->index, n = c->length; n > 0; d++, n--) {
                int32_t t;

                t = (int32_t) *d - 0x80;
                t = (t * linear[channel]) / 0x10000;
                t = CLAMP(t, -0x80, 0x7F);
                *d = (uint8_t) (t + 0x80);

                if (++channel >= spec->channels)
                    channel = 0;
            }
            break;
        }

        case PA_SAMPLE_FLOAT32NE: {
            float *d;
            int skip;
            unsigned n;
            unsigned channel;

            d = (float*) ((uint8_t*) ptr + c->index);
            skip = spec->channels * sizeof(float);
            n = c->length/sizeof(float)/spec->channels;

            for (channel = 0; channel < spec->channels ; channel ++) {
                float v, *t;

                if (volume->values[channel] == PA_VOLUME_NORM)
                    continue;

                v = (float) pa_sw_volume_to_linear(volume->values[channel]);
                t = d + channel;
                oil_scalarmult_f32(t, skip, t, skip, &v, n);
            }
            break;
        }

        default:
            pa_log_warn(" Unable to change volume of format %s.", pa_sample_format_to_string(spec->format));
            /* If we cannot change the volume, we just don't do it */
    }

    pa_memblock_release(c->memblock);
}

size_t pa_frame_align(size_t l, const pa_sample_spec *ss) {
    size_t fs;

    pa_assert(ss);

    fs = pa_frame_size(ss);

    return (l/fs) * fs;
}

int pa_frame_aligned(size_t l, const pa_sample_spec *ss) {
    size_t fs;

    pa_assert(ss);

    fs = pa_frame_size(ss);

    return l % fs == 0;
}

void pa_interleave(const void *src[], unsigned channels, void *dst, size_t ss, unsigned n) {
    unsigned c;
    size_t fs;

    pa_assert(src);
    pa_assert(channels > 0);
    pa_assert(dst);
    pa_assert(ss > 0);
    pa_assert(n > 0);

    fs = ss * channels;

    for (c = 0; c < channels; c++) {
        unsigned j;
        void *d;
        const void *s;

        s = src[c];
        d = (uint8_t*) dst + c * ss;

        for (j = 0; j < n; j ++) {
            oil_memcpy(d, s, ss);
            s = (uint8_t*) s + ss;
            d = (uint8_t*) d + fs;
        }
    }
}

void pa_deinterleave(const void *src, void *dst[], unsigned channels, size_t ss, unsigned n) {
    size_t fs;
    unsigned c;

    pa_assert(src);
    pa_assert(dst);
    pa_assert(channels > 0);
    pa_assert(ss > 0);
    pa_assert(n > 0);

    fs = ss * channels;

    for (c = 0; c < channels; c++) {
        unsigned j;
        const void *s;
        void *d;

        s = (uint8_t*) src + c * ss;
        d = dst[c];

        for (j = 0; j < n; j ++) {
            oil_memcpy(d, s, ss);
            s = (uint8_t*) s + fs;
            d = (uint8_t*) d + ss;
        }
    }
}
