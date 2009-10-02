/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <pulse/timeval.h>

#include <pulsecore/log.h>
#include <pulsecore/core-error.h>
#include <pulsecore/macro.h>
#include <pulsecore/g711.h>
#include <pulsecore/core-util.h>

#include "sample-util.h"
#include "endianmacros.h"

#define PA_SILENCE_MAX (PA_PAGE_SIZE*16)

pa_memblock *pa_silence_memblock(pa_memblock* b, const pa_sample_spec *spec) {
    void *data;

    pa_assert(b);
    pa_assert(spec);

    data = pa_memblock_acquire(b);
    pa_silence_memory(data, pa_memblock_get_length(b), spec);
    pa_memblock_release(b);

    return b;
}

pa_memchunk* pa_silence_memchunk(pa_memchunk *c, const pa_sample_spec *spec) {
    void *data;

    pa_assert(c);
    pa_assert(c->memblock);
    pa_assert(spec);

    data = pa_memblock_acquire(c->memblock);
    pa_silence_memory((uint8_t*) data+c->index, c->length, spec);
    pa_memblock_release(c->memblock);

    return c;
}

static uint8_t silence_byte(pa_sample_format_t format) {
    switch (format) {
        case PA_SAMPLE_U8:
            return 0x80;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
            return 0;
        case PA_SAMPLE_ALAW:
            return 0xd5;
        case PA_SAMPLE_ULAW:
            return 0xff;
        default:
            pa_assert_not_reached();
    }
}

void* pa_silence_memory(void *p, size_t length, const pa_sample_spec *spec) {
    pa_assert(p);
    pa_assert(length > 0);
    pa_assert(spec);

    memset(p, silence_byte(spec->format), length);
    return p;
}

#define VOLUME_PADDING 32

static void calc_linear_integer_volume(int32_t linear[], const pa_cvolume *volume) {
    unsigned channel, nchannels, padding;

    pa_assert(linear);
    pa_assert(volume);

    nchannels = volume->channels;

    for (channel = 0; channel < nchannels; channel++)
        linear[channel] = (int32_t) lrint(pa_sw_volume_to_linear(volume->values[channel]) * 0x10000);

    for (padding = 0; padding < VOLUME_PADDING; padding++, channel++)
        linear[channel] = linear[padding];
}

static void calc_linear_float_volume(float linear[], const pa_cvolume *volume) {
    unsigned channel, nchannels, padding;

    pa_assert(linear);
    pa_assert(volume);

    nchannels = volume->channels;

    for (channel = 0; channel < nchannels; channel++)
        linear[channel] = (float) pa_sw_volume_to_linear(volume->values[channel]);

    for (padding = 0; padding < VOLUME_PADDING; padding++, channel++)
        linear[channel] = linear[padding];
}

static void calc_linear_integer_stream_volumes(pa_mix_info streams[], unsigned nstreams, const pa_cvolume *volume, const pa_sample_spec *spec) {
    unsigned k, channel;
    float linear[PA_CHANNELS_MAX + VOLUME_PADDING];

    pa_assert(streams);
    pa_assert(spec);
    pa_assert(volume);

    calc_linear_float_volume(linear, volume);

    for (k = 0; k < nstreams; k++) {

        for (channel = 0; channel < spec->channels; channel++) {
            pa_mix_info *m = streams + k;
            m->linear[channel].i = (int32_t) lrint(pa_sw_volume_to_linear(m->volume.values[channel]) * linear[channel] * 0x10000);
        }
    }
}

static void calc_linear_float_stream_volumes(pa_mix_info streams[], unsigned nstreams, const pa_cvolume *volume, const pa_sample_spec *spec) {
    unsigned k, channel;
    float linear[PA_CHANNELS_MAX + VOLUME_PADDING];

    pa_assert(streams);
    pa_assert(spec);
    pa_assert(volume);

    calc_linear_float_volume(linear, volume);

    for (k = 0; k < nstreams; k++) {

        for (channel = 0; channel < spec->channels; channel++) {
            pa_mix_info *m = streams + k;
            m->linear[channel].f = (float) (pa_sw_volume_to_linear(m->volume.values[channel]) * linear[channel]);
        }
    }
}

size_t pa_mix(
        pa_mix_info streams[],
        unsigned nstreams,
        void *data,
        size_t length,
        const pa_sample_spec *spec,
        const pa_cvolume *volume,
        pa_bool_t mute) {

    pa_cvolume full_volume;
    unsigned k;
    unsigned z;
    void *end;

    pa_assert(streams);
    pa_assert(data);
    pa_assert(length);
    pa_assert(spec);

    if (!volume)
        volume = pa_cvolume_reset(&full_volume, spec->channels);

    if (mute || pa_cvolume_is_muted(volume) || nstreams <= 0) {
        pa_silence_memory(data, length, spec);
        return length;
    }

    for (k = 0; k < nstreams; k++)
        streams[k].ptr = (uint8_t*) pa_memblock_acquire(streams[k].chunk.memblock) + streams[k].chunk.index;

    for (z = 0; z < nstreams; z++)
        if (length > streams[z].chunk.length)
            length = streams[z].chunk.length;

    end = (uint8_t*) data + length;

    switch (spec->format) {

        case PA_SAMPLE_S16NE:{
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int32_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, lo, hi, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    /* Multiplying the 32bit volume factor with the
                     * 16bit sample might result in an 48bit value. We
                     * want to do without 64 bit integers and hence do
                     * the multiplication independantly for the HI and
                     * LO part of the volume. */

                    hi = cv >> 16;
                    lo = cv & 0xFFFF;

                    v = *((int16_t*) m->ptr);
                    v = ((v * lo) >> 16) + (v * hi);
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + sizeof(int16_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                *((int16_t*) data) = (int16_t) sum;

                data = (uint8_t*) data + sizeof(int16_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S16RE:{
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int32_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, lo, hi, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    hi = cv >> 16;
                    lo = cv & 0xFFFF;

                    v = PA_INT16_SWAP(*((int16_t*) m->ptr));
                    v = ((v * lo) >> 16) + (v * hi);
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + sizeof(int16_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                *((int16_t*) data) = PA_INT16_SWAP((int16_t) sum);

                data = (uint8_t*) data + sizeof(int16_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S32NE:{
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int64_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t cv = m->linear[channel].i;
                    int64_t v;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = *((int32_t*) m->ptr);
                    v = (v * cv) >> 16;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + sizeof(int32_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                *((int32_t*) data) = (int32_t) sum;

                data = (uint8_t*) data + sizeof(int32_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S32RE:{
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int64_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t cv = m->linear[channel].i;
                    int64_t v;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = PA_INT32_SWAP(*((int32_t*) m->ptr));
                    v = (v * cv) >> 16;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + sizeof(int32_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                *((int32_t*) data) = PA_INT32_SWAP((int32_t) sum);

                data = (uint8_t*) data + sizeof(int32_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S24NE: {
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int64_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t cv = m->linear[channel].i;
                    int64_t v;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = (int32_t) (PA_READ24NE(m->ptr) << 8);
                    v = (v * cv) >> 16;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + 3;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                PA_WRITE24NE(data, ((uint32_t) sum) >> 8);

                data = (uint8_t*) data + 3;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S24RE: {
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int64_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t cv = m->linear[channel].i;
                    int64_t v;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = (int32_t) (PA_READ24RE(m->ptr) << 8);
                    v = (v * cv) >> 16;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + 3;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                PA_WRITE24RE(data, ((uint32_t) sum) >> 8);

                data = (uint8_t*) data + 3;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S24_32NE: {
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int64_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t cv = m->linear[channel].i;
                    int64_t v;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = (int32_t) (*((uint32_t*)m->ptr) << 8);
                    v = (v * cv) >> 16;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + sizeof(int32_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                *((uint32_t*) data) = ((uint32_t) (int32_t) sum) >> 8;

                data = (uint8_t*) data + sizeof(uint32_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S24_32RE: {
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int64_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t cv = m->linear[channel].i;
                    int64_t v;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = (int32_t) (PA_UINT32_SWAP(*((uint32_t*) m->ptr)) << 8);
                    v = (v * cv) >> 16;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + 3;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                *((uint32_t*) data) = PA_INT32_SWAP(((uint32_t) (int32_t) sum) >> 8);

                data = (uint8_t*) data + sizeof(uint32_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_U8: {
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int32_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = (int32_t) *((uint8_t*) m->ptr) - 0x80;
                    v = (v * cv) >> 16;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + 1;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80, 0x7F);
                *((uint8_t*) data) = (uint8_t) (sum + 0x80);

                data = (uint8_t*) data + 1;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_ULAW: {
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int32_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, hi, lo, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    hi = cv >> 16;
                    lo = cv & 0xFFFF;

                    v = (int32_t) st_ulaw2linear16(*((uint8_t*) m->ptr));
                    v = ((v * lo) >> 16) + (v * hi);
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + 1;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                *((uint8_t*) data) = (uint8_t) st_14linear2ulaw((int16_t) sum >> 2);

                data = (uint8_t*) data + 1;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_ALAW: {
            unsigned channel = 0;

            calc_linear_integer_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                int32_t sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, hi, lo, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    hi = cv >> 16;
                    lo = cv & 0xFFFF;

                    v = (int32_t) st_alaw2linear16(*((uint8_t*) m->ptr));
                    v = ((v * lo) >> 16) + (v * hi);
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + 1;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                *((uint8_t*) data) = (uint8_t) st_13linear2alaw((int16_t) sum >> 3);

                data = (uint8_t*) data + 1;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_FLOAT32NE: {
            unsigned channel = 0;

            calc_linear_float_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                float sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    float v, cv = m->linear[channel].f;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = *((float*) m->ptr);
                    v *= cv;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + sizeof(float);
                }

                *((float*) data) = sum;

                data = (uint8_t*) data + sizeof(float);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_FLOAT32RE: {
            unsigned channel = 0;

            calc_linear_float_stream_volumes(streams, nstreams, volume, spec);

            while (data < end) {
                float sum = 0;
                unsigned i;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    float v, cv = m->linear[channel].f;

                    if (PA_UNLIKELY(cv <= 0))
                        continue;

                    v = PA_FLOAT32_SWAP(*(float*) m->ptr);
                    v *= cv;
                    sum += v;

                    m->ptr = (uint8_t*) m->ptr + sizeof(float);
                }

                *((float*) data) = PA_FLOAT32_SWAP(sum);

                data = (uint8_t*) data + sizeof(float);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        default:
            pa_log_error("Unable to mix audio data of format %s.", pa_sample_format_to_string(spec->format));
            pa_assert_not_reached();
    }

    for (k = 0; k < nstreams; k++)
        pa_memblock_release(streams[k].chunk.memblock);

    return length;
}

typedef union {
  float f;
  uint32_t i;
} volume_val;

typedef void (*pa_calc_volume_func_t) (void *volumes, const pa_cvolume *volume);

static const pa_calc_volume_func_t calc_volume_table[] = {
  [PA_SAMPLE_U8]        = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_ALAW]      = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_ULAW]      = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_S16LE]     = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_S16BE]     = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_FLOAT32LE] = (pa_calc_volume_func_t) calc_linear_float_volume,
  [PA_SAMPLE_FLOAT32BE] = (pa_calc_volume_func_t) calc_linear_float_volume,
  [PA_SAMPLE_S32LE]     = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_S32BE]     = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_S24LE]     = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_S24BE]     = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_S24_32LE]  = (pa_calc_volume_func_t) calc_linear_integer_volume,
  [PA_SAMPLE_S24_32BE]  = (pa_calc_volume_func_t) calc_linear_integer_volume
};

void pa_volume_memchunk(
        pa_memchunk*c,
        const pa_sample_spec *spec,
        const pa_cvolume *volume) {

    void *ptr;
    volume_val linear[PA_CHANNELS_MAX + VOLUME_PADDING];
    pa_do_volume_func_t do_volume;

    pa_assert(c);
    pa_assert(spec);
    pa_assert(c->length % pa_frame_size(spec) == 0);
    pa_assert(volume);

    if (pa_memblock_is_silence(c->memblock))
        return;

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_NORM))
        return;

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_MUTED)) {
        pa_silence_memchunk(c, spec);
        return;
    }

    if (spec->format < 0 || spec->format > PA_SAMPLE_MAX) {
      pa_log_warn(" Unable to change volume of format %s.", pa_sample_format_to_string(spec->format));
      return;
    }

    do_volume = pa_get_volume_func (spec->format);
    pa_assert(do_volume);

    calc_volume_table[spec->format] ((void *)linear, volume);

    ptr = (uint8_t*) pa_memblock_acquire(c->memblock) + c->index;

    do_volume (ptr, (void *)linear, spec->channels, c->length);

    pa_memblock_release(c->memblock);
}

size_t pa_frame_align(size_t l, const pa_sample_spec *ss) {
    size_t fs;

    pa_assert(ss);

    fs = pa_frame_size(ss);

    return (l/fs) * fs;
}

pa_bool_t pa_frame_aligned(size_t l, const pa_sample_spec *ss) {
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
            memcpy(d, s, (int) ss);
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
            memcpy(d, s, (int) ss);
            s = (uint8_t*) s + fs;
            d = (uint8_t*) d + ss;
        }
    }
}

static pa_memblock *silence_memblock_new(pa_mempool *pool, uint8_t c) {
    pa_memblock *b;
    size_t length;
    void *data;

    pa_assert(pool);

    length = PA_MIN(pa_mempool_block_size_max(pool), PA_SILENCE_MAX);

    b = pa_memblock_new(pool, length);

    data = pa_memblock_acquire(b);
    memset(data, c, length);
    pa_memblock_release(b);

    pa_memblock_set_is_silence(b, TRUE);

    return b;
}

void pa_silence_cache_init(pa_silence_cache *cache) {
    pa_assert(cache);

    memset(cache, 0, sizeof(pa_silence_cache));
}

void pa_silence_cache_done(pa_silence_cache *cache) {
    pa_sample_format_t f;
    pa_assert(cache);

    for (f = 0; f < PA_SAMPLE_MAX; f++)
        if (cache->blocks[f])
            pa_memblock_unref(cache->blocks[f]);

    memset(cache, 0, sizeof(pa_silence_cache));
}

pa_memchunk* pa_silence_memchunk_get(pa_silence_cache *cache, pa_mempool *pool, pa_memchunk* ret, const pa_sample_spec *spec, size_t length) {
    pa_memblock *b;
    size_t l;

    pa_assert(cache);
    pa_assert(pa_sample_spec_valid(spec));

    if (!(b = cache->blocks[spec->format]))

        switch (spec->format) {
            case PA_SAMPLE_U8:
                cache->blocks[PA_SAMPLE_U8] = b = silence_memblock_new(pool, 0x80);
                break;
            case PA_SAMPLE_S16LE:
            case PA_SAMPLE_S16BE:
            case PA_SAMPLE_S32LE:
            case PA_SAMPLE_S32BE:
            case PA_SAMPLE_S24LE:
            case PA_SAMPLE_S24BE:
            case PA_SAMPLE_S24_32LE:
            case PA_SAMPLE_S24_32BE:
            case PA_SAMPLE_FLOAT32LE:
            case PA_SAMPLE_FLOAT32BE:
                cache->blocks[PA_SAMPLE_S16LE] = b = silence_memblock_new(pool, 0);
                cache->blocks[PA_SAMPLE_S16BE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S32LE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S32BE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S24LE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S24BE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S24_32LE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S24_32BE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_FLOAT32LE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_FLOAT32BE] = pa_memblock_ref(b);
                break;
            case PA_SAMPLE_ALAW:
                cache->blocks[PA_SAMPLE_ALAW] = b = silence_memblock_new(pool, 0xd5);
                break;
            case PA_SAMPLE_ULAW:
                cache->blocks[PA_SAMPLE_ULAW] = b = silence_memblock_new(pool, 0xff);
                break;
            default:
                pa_assert_not_reached();
    }

    pa_assert(b);

    ret->memblock = pa_memblock_ref(b);

    l = pa_memblock_get_length(b);
    if (length > l || length == 0)
        length = l;

    ret->length = pa_frame_align(length, spec);
    ret->index = 0;

    return ret;
}

void pa_sample_clamp(pa_sample_format_t format, void *dst, size_t dstr, const void *src, size_t sstr, unsigned n) {
    const float *s;
    float *d;

    s = src; d = dst;

    if (format == PA_SAMPLE_FLOAT32NE) {
        for (; n > 0; n--) {
            float f;

            f = *s;
            *d = PA_CLAMP_UNLIKELY(f, -1.0f, 1.0f);

            s = (const float*) ((const uint8_t*) s + sstr);
            d = (float*) ((uint8_t*) d + dstr);
        }
    } else {
        pa_assert(format == PA_SAMPLE_FLOAT32RE);

        for (; n > 0; n--) {
            float f;

            f = PA_FLOAT32_SWAP(*s);
            f = PA_CLAMP_UNLIKELY(f, -1.0f, 1.0f);
            *d = PA_FLOAT32_SWAP(f);

            s = (const float*) ((const uint8_t*) s + sstr);
            d = (float*) ((uint8_t*) d + dstr);
        }
    }
}

/* Similar to pa_bytes_to_usec() but rounds up, not down */

pa_usec_t pa_bytes_to_usec_round_up(uint64_t length, const pa_sample_spec *spec) {
    size_t fs;
    pa_usec_t usec;

    pa_assert(spec);

    fs = pa_frame_size(spec);
    length = (length + fs - 1) / fs;

    usec = (pa_usec_t) length * PA_USEC_PER_SEC;

    return (usec + spec->rate - 1) / spec->rate;
}

/* Similar to pa_usec_to_bytes() but rounds up, not down */

size_t pa_usec_to_bytes_round_up(pa_usec_t t, const pa_sample_spec *spec) {
    uint64_t u;
    pa_assert(spec);

    u = (uint64_t) t * (uint64_t) spec->rate;

    u = (u + PA_USEC_PER_SEC - 1) / PA_USEC_PER_SEC;

    u *= pa_frame_size(spec);

    return (size_t) u;
}

void pa_memchunk_dump_to_file(pa_memchunk *c, const char *fn) {
    FILE *f;
    void *p;

    pa_assert(c);
    pa_assert(fn);

    /* Only for debugging purposes */

    f = fopen(fn, "a");

    if (!f) {
        pa_log_warn("Failed to open '%s': %s", fn, pa_cstrerror(errno));
        return;
    }

    p = pa_memblock_acquire(c->memblock);

    if (fwrite((uint8_t*) p + c->index, 1, c->length, f) != c->length)
        pa_log_warn("Failed to write to '%s': %s", fn, pa_cstrerror(errno));

    pa_memblock_release(c->memblock);

    fclose(f);
}

static void calc_sine(float *f, size_t l, double freq) {
    size_t i;

    l /= sizeof(float);

    for (i = 0; i < l; i++)
        *(f++) = (float) 0.5f * sin((double) i*M_PI*2*freq / (double) l);
}

void pa_memchunk_sine(pa_memchunk *c, pa_mempool *pool, unsigned rate, unsigned freq) {
    size_t l;
    unsigned gcd, n;
    void *p;

    pa_memchunk_reset(c);

    gcd = pa_gcd(rate, freq);
    n = rate / gcd;

    l = pa_mempool_block_size_max(pool) / sizeof(float);

    l /= n;
    if (l <= 0) l = 1;
    l *= n;

    c->length = l * sizeof(float);
    c->memblock = pa_memblock_new(pool, c->length);

    p = pa_memblock_acquire(c->memblock);
    calc_sine(p, c->length, freq * l / rate);
    pa_memblock_release(c->memblock);
}

size_t pa_convert_size(size_t size, const pa_sample_spec *from, const pa_sample_spec *to) {
    pa_usec_t usec;

    pa_assert(from);
    pa_assert(to);

    usec = pa_bytes_to_usec_round_up(size, from);
    return pa_usec_to_bytes_round_up(usec, to);
}
