#ifndef foosampleutilhfoo
#define foosampleutilhfoo

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

#include <pulse/gccmacro.h>
#include <pulse/sample.h>
#include <pulse/volume.h>
#include <pulse/channelmap.h>

#include <pulsecore/memblock.h>
#include <pulsecore/memchunk.h>

typedef struct pa_silence_cache {
    pa_memblock* blocks[PA_SAMPLE_MAX];
} pa_silence_cache;

void pa_silence_cache_init(pa_silence_cache *cache);
void pa_silence_cache_done(pa_silence_cache *cache);

void *pa_silence_memory(void *p, size_t length, const pa_sample_spec *spec);
pa_memchunk* pa_silence_memchunk(pa_memchunk *c, const pa_sample_spec *spec);
pa_memblock* pa_silence_memblock(pa_memblock *b, const pa_sample_spec *spec);

pa_memchunk* pa_silence_memchunk_get(pa_silence_cache *cache, pa_mempool *pool, pa_memchunk* ret, const pa_sample_spec *spec, size_t length);

typedef struct pa_mix_info {
    pa_memchunk chunk;
    pa_cvolume volume;
    void *userdata;

    /* The following fields are used internally by pa_mix(), should
     * not be initialised by the caller of pa_mix(). */
    void *ptr;
    union {
        int32_t i;
        float f;
    } linear[PA_CHANNELS_MAX];
} pa_mix_info;

size_t pa_mix(
    pa_mix_info channels[],
    unsigned nchannels,
    void *data,
    size_t length,
    const pa_sample_spec *spec,
    const pa_cvolume *volume,
    pa_bool_t mute);

void pa_volume_memchunk(
    pa_memchunk*c,
    const pa_sample_spec *spec,
    const pa_cvolume *volume);

size_t pa_frame_align(size_t l, const pa_sample_spec *ss) PA_GCC_PURE;

pa_bool_t pa_frame_aligned(size_t l, const pa_sample_spec *ss) PA_GCC_PURE;

void pa_interleave(const void *src[], unsigned channels, void *dst, size_t ss, unsigned n);
void pa_deinterleave(const void *src, void *dst[], unsigned channels, size_t ss, unsigned n);

void pa_sample_clamp(pa_sample_format_t format, void *dst, size_t dstr, const void *src, size_t sstr, unsigned n);

pa_usec_t pa_bytes_to_usec_round_up(uint64_t length, const pa_sample_spec *spec);
size_t pa_usec_to_bytes_round_up(pa_usec_t t, const pa_sample_spec *spec);

void pa_memchunk_dump_to_file(pa_memchunk *c, const char *fn);

void pa_memchunk_sine(pa_memchunk *c, pa_mempool *pool, unsigned rate, unsigned freq);

typedef void (*pa_do_volume_func_t) (void *samples, void *volumes, unsigned channels, unsigned length);

pa_do_volume_func_t pa_get_volume_func(pa_sample_format_t f);
void pa_set_volume_func(pa_sample_format_t f, pa_do_volume_func_t func);

size_t pa_convert_size(size_t size, const pa_sample_spec *from, const pa_sample_spec *to);

#define PA_CHANNEL_POSITION_MASK_LEFT                                   \
    (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_LEFT)           \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_REAR_LEFT)          \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER) \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_SIDE_LEFT)          \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_LEFT)     \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_LEFT))     \

#define PA_CHANNEL_POSITION_MASK_RIGHT                                  \
    (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_RIGHT)          \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_REAR_RIGHT)         \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER) \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_SIDE_RIGHT)         \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_RIGHT)    \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_RIGHT))

#define PA_CHANNEL_POSITION_MASK_CENTER                                 \
    (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_CENTER)         \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_REAR_CENTER)        \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_CENTER)         \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_CENTER)   \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_CENTER))

#define PA_CHANNEL_POSITION_MASK_FRONT                                  \
    (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_LEFT)           \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_RIGHT)        \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_CENTER)       \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER) \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER) \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_LEFT)     \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_RIGHT)    \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_CENTER))

#define PA_CHANNEL_POSITION_MASK_REAR                                   \
    (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_REAR_LEFT)            \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_REAR_RIGHT)         \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_REAR_CENTER)        \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_LEFT)      \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_RIGHT)     \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_CENTER))

#define PA_CHANNEL_POSITION_MASK_SIDE_OR_TOP_CENTER                     \
    (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_SIDE_LEFT)            \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_SIDE_RIGHT)         \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_CENTER))

#define PA_CHANNEL_POSITION_MASK_TOP                                    \
    (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_CENTER)           \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_LEFT)     \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_RIGHT)    \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_FRONT_CENTER)   \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_LEFT)      \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_RIGHT)     \
     | PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_TOP_REAR_CENTER))

#define PA_CHANNEL_POSITION_MASK_ALL            \
    ((pa_channel_position_mask_t) (PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_MAX)-1))

#endif
