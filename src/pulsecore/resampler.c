/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <string.h>

#if HAVE_LIBSAMPLERATE
#include <samplerate.h>
#endif

#include <liboil/liboilfuncs.h>
#include <liboil/liboil.h>

#include <pulse/xmalloc.h>
#include <pulsecore/sconv.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "speexwrap.h"

#include "ffmpeg/avcodec.h"

#include "resampler.h"

/* Number of samples of extra space we allow the resamplers to return */
#define EXTRA_SAMPLES 128

struct pa_resampler {
    pa_resample_method_t resample_method;
    pa_sample_spec i_ss, o_ss;
    pa_channel_map i_cm, o_cm;
    size_t i_fz, o_fz, w_sz;
    pa_mempool *mempool;

    pa_memchunk buf1, buf2, buf3, buf4;
    unsigned buf1_samples, buf2_samples, buf3_samples, buf4_samples;

    pa_sample_format_t work_format;

    pa_convert_func_t to_work_format_func;
    pa_convert_func_t from_work_format_func;

    int map_table[PA_CHANNELS_MAX][PA_CHANNELS_MAX];
    int map_required;

    void (*impl_free)(pa_resampler *r);
    void (*impl_update_rates)(pa_resampler *r);
    void (*impl_resample)(pa_resampler *r, const pa_memchunk *in, unsigned in_samples, pa_memchunk *out, unsigned *out_samples);

    struct { /* data specific to the trivial resampler */
        unsigned o_counter;
        unsigned i_counter;
    } trivial;

#ifdef HAVE_LIBSAMPLERATE
    struct { /* data specific to libsamplerate */
        SRC_STATE *state;
    } src;
#endif

    struct { /* data specific to speex */
        SpeexResamplerState* state;
    } speex;

    struct { /* data specific to ffmpeg */
        struct AVResampleContext *state;
        pa_memchunk buf[PA_CHANNELS_MAX];
    } ffmpeg;
};

static int copy_init(pa_resampler *r);
static int trivial_init(pa_resampler*r);
static int speex_init(pa_resampler*r);
static int ffmpeg_init(pa_resampler*r);
#ifdef HAVE_LIBSAMPLERATE
static int libsamplerate_init(pa_resampler*r);
#endif

static void calc_map_table(pa_resampler *r);

static int (* const init_table[])(pa_resampler*r) = {
#ifdef HAVE_LIBSAMPLERATE
    [PA_RESAMPLER_SRC_SINC_BEST_QUALITY]   = libsamplerate_init,
    [PA_RESAMPLER_SRC_SINC_MEDIUM_QUALITY] = libsamplerate_init,
    [PA_RESAMPLER_SRC_SINC_FASTEST]        = libsamplerate_init,
    [PA_RESAMPLER_SRC_ZERO_ORDER_HOLD]     = libsamplerate_init,
    [PA_RESAMPLER_SRC_LINEAR]              = libsamplerate_init,
#else
    [PA_RESAMPLER_SRC_SINC_BEST_QUALITY]   = NULL,
    [PA_RESAMPLER_SRC_SINC_MEDIUM_QUALITY] = NULL,
    [PA_RESAMPLER_SRC_SINC_FASTEST]        = NULL,
    [PA_RESAMPLER_SRC_ZERO_ORDER_HOLD]     = NULL,
    [PA_RESAMPLER_SRC_LINEAR]              = NULL,
#endif
    [PA_RESAMPLER_TRIVIAL]                 = trivial_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+0]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+1]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+2]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+3]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+4]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+5]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+6]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+7]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+8]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+9]      = speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+10]     = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+0]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+1]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+2]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+3]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+4]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+5]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+6]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+7]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+8]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+9]      = speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+10]     = speex_init,
    [PA_RESAMPLER_FFMPEG]                  = ffmpeg_init,
    [PA_RESAMPLER_AUTO]                    = NULL,
    [PA_RESAMPLER_COPY]                    = copy_init
};

static inline size_t sample_size(pa_sample_format_t f) {
    pa_sample_spec ss = {
        .format = f,
        .rate = 0,
        .channels = 1
    };

    return pa_sample_size(&ss);
}

pa_resampler* pa_resampler_new(
        pa_mempool *pool,
        const pa_sample_spec *a,
        const pa_channel_map *am,
        const pa_sample_spec *b,
        const pa_channel_map *bm,
        pa_resample_method_t resample_method,
        int variable_rate) {

    pa_resampler *r = NULL;

    pa_assert(pool);
    pa_assert(a);
    pa_assert(b);
    pa_assert(pa_sample_spec_valid(a));
    pa_assert(pa_sample_spec_valid(b));
    pa_assert(resample_method >= 0);
    pa_assert(resample_method < PA_RESAMPLER_MAX);

    /* Fix method */

    if (!variable_rate && a->rate == b->rate) {
        pa_log_info("Forcing resampler 'copy', because of fixed, identical sample rates.");
        resample_method = PA_RESAMPLER_COPY;
    }

    if (!pa_resample_method_supported(resample_method)) {
        pa_log_warn("Support for resampler '%s' not compiled in, reverting to 'auto'.", pa_resample_method_to_string(resample_method));
        resample_method = PA_RESAMPLER_AUTO;
    }

    if (resample_method == PA_RESAMPLER_FFMPEG && variable_rate) {
        pa_log_info("Resampler 'ffmpeg' cannot do variable rate, reverting to resampler 'auto'.");
        resample_method = PA_RESAMPLER_AUTO;
    }

    if (resample_method == PA_RESAMPLER_COPY && (variable_rate || a->rate != b->rate)) {
        pa_log_info("Resampler 'copy' cannot change sampling rate, reverting to resampler 'auto'.");
        resample_method = PA_RESAMPLER_AUTO;
    }

    if (resample_method == PA_RESAMPLER_AUTO)
        resample_method = PA_RESAMPLER_SPEEX_FLOAT_BASE + 3;

    r = pa_xnew(pa_resampler, 1);
    r->mempool = pool;
    r->resample_method = resample_method;

    r->impl_free = NULL;
    r->impl_update_rates = NULL;
    r->impl_resample = NULL;

    /* Fill sample specs */
    r->i_ss = *a;
    r->o_ss = *b;

    if (am)
        r->i_cm = *am;
    else
        pa_channel_map_init_auto(&r->i_cm, r->i_ss.channels, PA_CHANNEL_MAP_DEFAULT);

    if (bm)
        r->o_cm = *bm;
    else
        pa_channel_map_init_auto(&r->o_cm, r->o_ss.channels, PA_CHANNEL_MAP_DEFAULT);

    r->i_fz = pa_frame_size(a);
    r->o_fz = pa_frame_size(b);

    pa_memchunk_reset(&r->buf1);
    pa_memchunk_reset(&r->buf2);
    pa_memchunk_reset(&r->buf3);
    pa_memchunk_reset(&r->buf4);

    r->buf1_samples = r->buf2_samples = r->buf3_samples = r->buf4_samples = 0;

    calc_map_table(r);

    pa_log_info("Using resampler '%s'", pa_resample_method_to_string(resample_method));

    if ((resample_method >= PA_RESAMPLER_SPEEX_FIXED_BASE && resample_method <= PA_RESAMPLER_SPEEX_FIXED_MAX) ||
        (resample_method == PA_RESAMPLER_FFMPEG))
        r->work_format = PA_SAMPLE_S16NE;
    else if (resample_method == PA_RESAMPLER_TRIVIAL || resample_method == PA_RESAMPLER_COPY) {

        if (r->map_required || a->format != b->format) {

            if (a->format == PA_SAMPLE_FLOAT32NE || a->format == PA_SAMPLE_FLOAT32RE ||
                b->format == PA_SAMPLE_FLOAT32NE || b->format == PA_SAMPLE_FLOAT32RE)
                r->work_format = PA_SAMPLE_FLOAT32NE;
            else
                r->work_format = PA_SAMPLE_S16NE;

        } else
            r->work_format = a->format;

    } else
        r->work_format = PA_SAMPLE_FLOAT32NE;

    pa_log_info("Using %s as working format.", pa_sample_format_to_string(r->work_format));

    r->w_sz = sample_size(r->work_format);

    if (r->i_ss.format == r->work_format)
        r->to_work_format_func = NULL;
    else if (r->work_format == PA_SAMPLE_FLOAT32NE) {
        if (!(r->to_work_format_func = pa_get_convert_to_float32ne_function(r->i_ss.format)))
            goto fail;
    } else {
        pa_assert(r->work_format == PA_SAMPLE_S16NE);
        if (!(r->to_work_format_func = pa_get_convert_to_s16ne_function(r->i_ss.format)))
            goto fail;
    }

    if (r->o_ss.format == r->work_format)
        r->from_work_format_func = NULL;
    else if (r->work_format == PA_SAMPLE_FLOAT32NE) {
        if (!(r->from_work_format_func = pa_get_convert_from_float32ne_function(r->o_ss.format)))
            goto fail;
    } else {
        pa_assert(r->work_format == PA_SAMPLE_S16NE);
        if (!(r->from_work_format_func = pa_get_convert_from_s16ne_function(r->o_ss.format)))
            goto fail;
    }

    /* initialize implementation */
    if (init_table[resample_method](r) < 0)
        goto fail;

    return r;

fail:
    if (r)
        pa_xfree(r);

    return NULL;
}

void pa_resampler_free(pa_resampler *r) {
    pa_assert(r);

    if (r->impl_free)
        r->impl_free(r);

    if (r->buf1.memblock)
        pa_memblock_unref(r->buf1.memblock);
    if (r->buf2.memblock)
        pa_memblock_unref(r->buf2.memblock);
    if (r->buf3.memblock)
        pa_memblock_unref(r->buf3.memblock);
    if (r->buf4.memblock)
        pa_memblock_unref(r->buf4.memblock);

    pa_xfree(r);
}

void pa_resampler_set_input_rate(pa_resampler *r, uint32_t rate) {
    pa_assert(r);
    pa_assert(rate > 0);

    if (r->i_ss.rate == rate)
        return;

    r->i_ss.rate = rate;

    r->impl_update_rates(r);
}

void pa_resampler_set_output_rate(pa_resampler *r, uint32_t rate) {
    pa_assert(r);
    pa_assert(rate > 0);

    if (r->o_ss.rate == rate)
        return;

    r->o_ss.rate = rate;

    r->impl_update_rates(r);
}

size_t pa_resampler_request(pa_resampler *r, size_t out_length) {
    pa_assert(r);

    return (((out_length / r->o_fz)*r->i_ss.rate)/r->o_ss.rate) * r->i_fz;
}

size_t pa_resampler_max_block_size(pa_resampler *r) {
    size_t block_size_max;
    pa_sample_spec ss;
    size_t fs;

    pa_assert(r);

    block_size_max = pa_mempool_block_size_max(r->mempool);

    /* We deduce the "largest" sample spec we're using during the
     * conversion */
    ss = r->i_ss;
    if (r->o_ss.channels > ss.channels)
        ss.channels = r->o_ss.channels;

    /* We silently assume that the format enum is ordered by size */
    if (r->o_ss.format > ss.format)
        ss.format = r->o_ss.format;
    if (r->work_format > ss.format)
        ss.format = r->work_format;

    if (r->o_ss.rate > ss.rate)
        ss.rate = r->o_ss.rate;

    fs = pa_frame_size(&ss);

    return (((block_size_max/fs + EXTRA_SAMPLES)*r->i_ss.rate)/ss.rate)*r->i_fz;
}

pa_resample_method_t pa_resampler_get_method(pa_resampler *r) {
    pa_assert(r);

    return r->resample_method;
}

static const char * const resample_methods[] = {
    "src-sinc-best-quality",
    "src-sinc-medium-quality",
    "src-sinc-fastest",
    "src-zero-order-hold",
    "src-linear",
    "trivial",
    "speex-float-0",
    "speex-float-1",
    "speex-float-2",
    "speex-float-3",
    "speex-float-4",
    "speex-float-5",
    "speex-float-6",
    "speex-float-7",
    "speex-float-8",
    "speex-float-9",
    "speex-float-10",
    "speex-fixed-0",
    "speex-fixed-1",
    "speex-fixed-2",
    "speex-fixed-3",
    "speex-fixed-4",
    "speex-fixed-5",
    "speex-fixed-6",
    "speex-fixed-7",
    "speex-fixed-8",
    "speex-fixed-9",
    "speex-fixed-10",
    "ffmpeg",
    "auto",
    "copy"
};

const char *pa_resample_method_to_string(pa_resample_method_t m) {

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return NULL;

    return resample_methods[m];
}

int pa_resample_method_supported(pa_resample_method_t m) {

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return 0;

#ifndef HAVE_LIBSAMPLERATE
    if (m <= PA_RESAMPLER_SRC_LINEAR)
        return 0;
#endif

    return 1;
}

pa_resample_method_t pa_parse_resample_method(const char *string) {
    pa_resample_method_t m;

    pa_assert(string);

    for (m = 0; m < PA_RESAMPLER_MAX; m++)
        if (!strcmp(string, resample_methods[m]))
            return m;

    if (!strcmp(string, "speex-fixed"))
        return PA_RESAMPLER_SPEEX_FIXED_BASE + 3;

    if (!strcmp(string, "speex-float"))
        return PA_RESAMPLER_SPEEX_FLOAT_BASE + 3;

    return PA_RESAMPLER_INVALID;
}

static void calc_map_table(pa_resampler *r) {
    unsigned oc;

    pa_assert(r);

    if (!(r->map_required = (r->i_ss.channels != r->o_ss.channels || !pa_channel_map_equal(&r->i_cm, &r->o_cm))))
        return;

    for (oc = 0; oc < r->o_ss.channels; oc++) {
        unsigned ic, i = 0;

        for (ic = 0; ic < r->i_ss.channels; ic++) {
            pa_channel_position_t a, b;

            a = r->i_cm.map[ic];
            b = r->o_cm.map[oc];

            if (a == b ||
                (a == PA_CHANNEL_POSITION_MONO && b == PA_CHANNEL_POSITION_LEFT) ||
                (a == PA_CHANNEL_POSITION_MONO && b == PA_CHANNEL_POSITION_RIGHT) ||
                (a == PA_CHANNEL_POSITION_LEFT && b == PA_CHANNEL_POSITION_MONO) ||
                (a == PA_CHANNEL_POSITION_RIGHT && b == PA_CHANNEL_POSITION_MONO))

                r->map_table[oc][i++] = ic;
        }

        /* Add an end marker */
        if (i < PA_CHANNELS_MAX)
            r->map_table[oc][i] = -1;
    }
}

static pa_memchunk* convert_to_work_format(pa_resampler *r, pa_memchunk *input) {
    unsigned n_samples;
    void *src, *dst;

    pa_assert(r);
    pa_assert(input);
    pa_assert(input->memblock);

    /* Convert the incoming sample into the work sample format and place them in buf1 */

    if (!r->to_work_format_func || !input->length)
        return input;

    n_samples = (input->length / r->i_fz) * r->i_ss.channels;

    r->buf1.index = 0;
    r->buf1.length = r->w_sz * n_samples;

    if (!r->buf1.memblock || r->buf1_samples < n_samples) {
        if (r->buf1.memblock)
            pa_memblock_unref(r->buf1.memblock);

        r->buf1_samples = n_samples;
        r->buf1.memblock = pa_memblock_new(r->mempool, r->buf1.length);
    }

    src = (uint8_t*) pa_memblock_acquire(input->memblock) + input->index;
    dst = (uint8_t*) pa_memblock_acquire(r->buf1.memblock);

    r->to_work_format_func(n_samples, src, dst);

    pa_memblock_release(input->memblock);
    pa_memblock_release(r->buf1.memblock);

    return &r->buf1;
}

static pa_memchunk *remap_channels(pa_resampler *r, pa_memchunk *input) {
    unsigned in_n_samples, out_n_samples, n_frames;
    int i_skip, o_skip;
    unsigned oc;
    void *src, *dst;

    pa_assert(r);
    pa_assert(input);
    pa_assert(input->memblock);

    /* Remap channels and place the result int buf2 */

    if (!r->map_required || !input->length)
        return input;

    in_n_samples = input->length / r->w_sz;
    n_frames = in_n_samples / r->i_ss.channels;
    out_n_samples = n_frames * r->o_ss.channels;

    r->buf2.index = 0;
    r->buf2.length = r->w_sz * out_n_samples;

    if (!r->buf2.memblock || r->buf2_samples < out_n_samples) {
        if (r->buf2.memblock)
            pa_memblock_unref(r->buf2.memblock);

        r->buf2_samples = out_n_samples;
        r->buf2.memblock = pa_memblock_new(r->mempool, r->buf2.length);
    }

    src = ((uint8_t*) pa_memblock_acquire(input->memblock) + input->index);
    dst = pa_memblock_acquire(r->buf2.memblock);

    memset(dst, 0, r->buf2.length);

    o_skip = r->w_sz * r->o_ss.channels;
    i_skip = r->w_sz * r->i_ss.channels;

    switch (r->work_format) {
        case PA_SAMPLE_FLOAT32NE:

            for (oc = 0; oc < r->o_ss.channels; oc++) {
                unsigned i;
                static const float one = 1.0;

                for (i = 0; i < PA_CHANNELS_MAX && r->map_table[oc][i] >= 0; i++)
                    oil_vectoradd_f32(
                            (float*) dst + oc, o_skip,
                            (float*) dst + oc, o_skip,
                            (float*) src + r->map_table[oc][i], i_skip,
                            n_frames,
                            &one, &one);
            }

            break;

        case PA_SAMPLE_S16NE:

            for (oc = 0; oc < r->o_ss.channels; oc++) {
                unsigned i;
                static const int16_t one = 1;

                for (i = 0; i < PA_CHANNELS_MAX && r->map_table[oc][i] >= 0; i++)
                    oil_vectoradd_s16(
                            (int16_t*) dst + oc, o_skip,
                            (int16_t*) dst + oc, o_skip,
                            (int16_t*) src + r->map_table[oc][i], i_skip,
                            n_frames,
                            &one, &one);
            }

            break;

        default:
            pa_assert_not_reached();
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(r->buf2.memblock);

    r->buf2.length = out_n_samples * r->w_sz;

    return &r->buf2;
}

static pa_memchunk *resample(pa_resampler *r, pa_memchunk *input) {
    unsigned in_n_frames, in_n_samples;
    unsigned out_n_frames, out_n_samples;

    pa_assert(r);
    pa_assert(input);

    /* Resample the data and place the result in buf3 */

    if (!r->impl_resample || !input->length)
        return input;

    in_n_samples = input->length / r->w_sz;
    in_n_frames = in_n_samples / r->o_ss.channels;

    out_n_frames = ((in_n_frames*r->o_ss.rate)/r->i_ss.rate)+EXTRA_SAMPLES;
    out_n_samples = out_n_frames * r->o_ss.channels;

    r->buf3.index = 0;
    r->buf3.length = r->w_sz * out_n_samples;

    if (!r->buf3.memblock || r->buf3_samples < out_n_samples) {
        if (r->buf3.memblock)
            pa_memblock_unref(r->buf3.memblock);

        r->buf3_samples = out_n_samples;
        r->buf3.memblock = pa_memblock_new(r->mempool, r->buf3.length);
    }

    r->impl_resample(r, input, in_n_frames, &r->buf3, &out_n_frames);
    r->buf3.length = out_n_frames * r->w_sz * r->o_ss.channels;

    return &r->buf3;
}

static pa_memchunk *convert_from_work_format(pa_resampler *r, pa_memchunk *input) {
    unsigned n_samples, n_frames;
    void *src, *dst;

    pa_assert(r);
    pa_assert(input);

    /* Convert the data into the correct sample type and place the result in buf4 */

    if (!r->from_work_format_func || !input->length)
        return input;

    n_samples = input->length / r->w_sz;
    n_frames =  n_samples / r->o_ss.channels;

    r->buf4.index = 0;
    r->buf4.length = r->o_fz * n_frames;

    if (!r->buf4.memblock || r->buf4_samples < n_samples) {
        if (r->buf4.memblock)
            pa_memblock_unref(r->buf4.memblock);

        r->buf4_samples = n_samples;
        r->buf4.memblock = pa_memblock_new(r->mempool, r->buf4.length);
    }

    src = (uint8_t*) pa_memblock_acquire(input->memblock) + input->index;
    dst = pa_memblock_acquire(r->buf4.memblock);
    r->from_work_format_func(n_samples, src, dst);
    pa_memblock_release(input->memblock);
    pa_memblock_release(r->buf4.memblock);

    r->buf4.length = r->o_fz * n_frames;

    return &r->buf4;
}

void pa_resampler_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    pa_memchunk *buf;

    pa_assert(r);
    pa_assert(in);
    pa_assert(out);
    pa_assert(in->length);
    pa_assert(in->memblock);
    pa_assert(in->length % r->i_fz == 0);

    buf = (pa_memchunk*) in;
    buf = convert_to_work_format(r, buf);
    buf = remap_channels(r, buf);
    buf = resample(r, buf);

    if (buf->length) {
        buf = convert_from_work_format(r, buf);
        *out = *buf;

        if (buf == in)
            pa_memblock_ref(buf->memblock);
        else
            pa_memchunk_reset(buf);
    } else
        pa_memchunk_reset(out);
}

/*** libsamplerate based implementation ***/

#ifdef HAVE_LIBSAMPLERATE
static void libsamplerate_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    SRC_DATA data;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    memset(&data, 0, sizeof(data));

    data.data_in = (float*) ((uint8_t*) pa_memblock_acquire(input->memblock) + input->index);
    data.input_frames = in_n_frames;

    data.data_out = (float*) ((uint8_t*) pa_memblock_acquire(output->memblock) + output->index);
    data.output_frames = *out_n_frames;

    data.src_ratio = (double) r->o_ss.rate / r->i_ss.rate;
    data.end_of_input = 0;

    pa_assert_se(src_process(r->src.state, &data) == 0);
    pa_assert((unsigned) data.input_frames_used == in_n_frames);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = data.output_frames_gen;
}

static void libsamplerate_update_rates(pa_resampler *r) {
    pa_assert(r);

    pa_assert_se(src_set_ratio(r->src.state, (double) r->o_ss.rate / r->i_ss.rate) == 0);
}

static void libsamplerate_free(pa_resampler *r) {
    pa_assert(r);

    if (r->src.state)
        src_delete(r->src.state);
}

static int libsamplerate_init(pa_resampler *r) {
    int err;

    pa_assert(r);

    if (!(r->src.state = src_new(r->resample_method, r->o_ss.channels, &err)))
        return -1;

    r->impl_free = libsamplerate_free;
    r->impl_update_rates = libsamplerate_update_rates;
    r->impl_resample = libsamplerate_resample;

    return 0;
}
#endif

/*** speex based implementation ***/

static void speex_resample_float(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    float *in, *out;
    uint32_t inf = in_n_frames, outf = *out_n_frames;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    in = (float*) ((uint8_t*) pa_memblock_acquire(input->memblock) + input->index);
    out = (float*) ((uint8_t*) pa_memblock_acquire(output->memblock) + output->index);

    pa_assert_se(paspfl_resampler_process_interleaved_float(r->speex.state, in, &inf, out, &outf) == 0);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    pa_assert(inf == in_n_frames);
    *out_n_frames = outf;
}

static void speex_resample_int(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    int16_t *in, *out;
    uint32_t inf = in_n_frames, outf = *out_n_frames;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    in = (int16_t*) ((uint8_t*) pa_memblock_acquire(input->memblock) + input->index);
    out = (int16_t*) ((uint8_t*) pa_memblock_acquire(output->memblock) + output->index);

    pa_assert_se(paspfx_resampler_process_interleaved_int(r->speex.state, in, &inf, out, &outf) == 0);

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    pa_assert(inf == in_n_frames);
    *out_n_frames = outf;
}

static void speex_update_rates(pa_resampler *r) {
    pa_assert(r);

    if (r->resample_method >= PA_RESAMPLER_SPEEX_FIXED_BASE && r->resample_method <= PA_RESAMPLER_SPEEX_FIXED_MAX)
        pa_assert_se(paspfx_resampler_set_rate(r->speex.state, r->i_ss.rate, r->o_ss.rate) == 0);
    else {
        pa_assert(r->resample_method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && r->resample_method <= PA_RESAMPLER_SPEEX_FLOAT_MAX);
        pa_assert_se(paspfl_resampler_set_rate(r->speex.state, r->i_ss.rate, r->o_ss.rate) == 0);
    }
}

static void speex_free(pa_resampler *r) {
    pa_assert(r);

    if (!r->speex.state)
        return;

    if (r->resample_method >= PA_RESAMPLER_SPEEX_FIXED_BASE && r->resample_method <= PA_RESAMPLER_SPEEX_FIXED_MAX)
        paspfx_resampler_destroy(r->speex.state);
    else {
        pa_assert(r->resample_method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && r->resample_method <= PA_RESAMPLER_SPEEX_FLOAT_MAX);
        paspfl_resampler_destroy(r->speex.state);
    }
}

static int speex_init(pa_resampler *r) {
    int q, err;

    pa_assert(r);

    r->impl_free = speex_free;
    r->impl_update_rates = speex_update_rates;

    if (r->resample_method >= PA_RESAMPLER_SPEEX_FIXED_BASE && r->resample_method <= PA_RESAMPLER_SPEEX_FIXED_MAX) {
        q = r->resample_method - PA_RESAMPLER_SPEEX_FIXED_BASE;

        pa_log_info("Choosing speex quality setting %i.", q);

        if (!(r->speex.state = paspfx_resampler_init(r->o_ss.channels, r->i_ss.rate, r->o_ss.rate, q, &err)))
            return -1;

        r->impl_resample = speex_resample_int;
    } else {
        pa_assert(r->resample_method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && r->resample_method <= PA_RESAMPLER_SPEEX_FLOAT_MAX);
        q = r->resample_method - PA_RESAMPLER_SPEEX_FLOAT_BASE;

        pa_log_info("Choosing speex quality setting %i.", q);

        if (!(r->speex.state = paspfl_resampler_init(r->o_ss.channels, r->i_ss.rate, r->o_ss.rate, q, &err)))
            return -1;

        r->impl_resample = speex_resample_float;
    }

    return 0;
}

/* Trivial implementation */

static void trivial_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    size_t fz;
    unsigned o_index;
    void *src, *dst;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    fz = r->w_sz * r->o_ss.channels;

    src = (uint8_t*) pa_memblock_acquire(input->memblock) + input->index;
    dst = (uint8_t*) pa_memblock_acquire(output->memblock) + output->index;

    for (o_index = 0;; o_index++, r->trivial.o_counter++) {
        unsigned j;

        j = ((r->trivial.o_counter * r->i_ss.rate) / r->o_ss.rate);
        j = j > r->trivial.i_counter ? j - r->trivial.i_counter : 0;

        if (j >= in_n_frames)
            break;

        pa_assert(o_index * fz < pa_memblock_get_length(output->memblock));

        oil_memcpy((uint8_t*) dst + fz * o_index,
                   (uint8_t*) src + fz * j, fz);
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

    *out_n_frames = o_index;

    r->trivial.i_counter += in_n_frames;

    /* Normalize counters */
    while (r->trivial.i_counter >= r->i_ss.rate) {
        pa_assert(r->trivial.o_counter >= r->o_ss.rate);

        r->trivial.i_counter -= r->i_ss.rate;
        r->trivial.o_counter -= r->o_ss.rate;
    }
}

static void trivial_update_rates(pa_resampler *r) {
    pa_assert(r);

    r->trivial.i_counter = 0;
    r->trivial.o_counter = 0;
}

static int trivial_init(pa_resampler*r) {
    pa_assert(r);

    r->trivial.o_counter = r->trivial.i_counter = 0;

    r->impl_resample = trivial_resample;
    r->impl_update_rates = trivial_update_rates;
    r->impl_free = NULL;

    return 0;
}

/*** ffmpeg based implementation ***/

static void ffmpeg_resample(pa_resampler *r, const pa_memchunk *input, unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    unsigned used_frames = 0, c;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    for (c = 0; c < r->o_ss.channels; c++) {
        unsigned u;
        pa_memblock *b, *w;
        int16_t *p, *t, *k, *q, *s;
        int consumed_frames;
        unsigned in, l;

        /* Allocate a new block */
        b = pa_memblock_new(r->mempool, r->ffmpeg.buf[c].length + in_n_frames * sizeof(int16_t));
        p = pa_memblock_acquire(b);

        /* Copy the remaining data into it */
        l = r->ffmpeg.buf[c].length;
        if (r->ffmpeg.buf[c].memblock) {
            t = (int16_t*) ((uint8_t*) pa_memblock_acquire(r->ffmpeg.buf[c].memblock) + r->ffmpeg.buf[c].index);
            memcpy(p, t, l);
            pa_memblock_release(r->ffmpeg.buf[c].memblock);
            pa_memblock_unref(r->ffmpeg.buf[c].memblock);
            pa_memchunk_reset(&r->ffmpeg.buf[c]);
        }

        /* Now append the new data, splitting up channels */
        t = ((int16_t*) ((uint8_t*) pa_memblock_acquire(input->memblock) + input->index)) + c;
        k = (int16_t*) ((uint8_t*) p + l);
        for (u = 0; u < in_n_frames; u++) {
            *k = *t;
            t += r->o_ss.channels;
            k ++;
        }
        pa_memblock_release(input->memblock);

        /* Calculate the resulting number of frames */
        in = in_n_frames + l / sizeof(int16_t);

        /* Allocate buffer for the result */
        w = pa_memblock_new(r->mempool, *out_n_frames * sizeof(int16_t));
        q = pa_memblock_acquire(w);

        /* Now, resample */
        used_frames = av_resample(r->ffmpeg.state,
                                  q, p,
                                  &consumed_frames,
                                  in, *out_n_frames,
                                  c >= (unsigned) r->o_ss.channels-1);

        pa_memblock_release(b);

        /* Now store the remaining samples away */
        pa_assert(consumed_frames <= (int) in);
        if (consumed_frames < (int) in) {
            r->ffmpeg.buf[c].memblock = b;
            r->ffmpeg.buf[c].index = consumed_frames * sizeof(int16_t);
            r->ffmpeg.buf[c].length = (in - consumed_frames) * sizeof(int16_t);
        } else
            pa_memblock_unref(b);

        /* And place the results in the output buffer */
        s = (short*) ((uint8_t*) pa_memblock_acquire(output->memblock) + output->index) + c;
        for (u = 0; u < used_frames; u++) {
            *s = *q;
            q++;
            s += r->o_ss.channels;
        }
        pa_memblock_release(output->memblock);
        pa_memblock_release(w);
        pa_memblock_unref(w);
    }

    *out_n_frames = used_frames;
}

static void ffmpeg_free(pa_resampler *r) {
    unsigned c;

    pa_assert(r);

    if (r->ffmpeg.state)
        av_resample_close(r->ffmpeg.state);

    for (c = 0; c < PA_ELEMENTSOF(r->ffmpeg.buf); c++)
        if (r->ffmpeg.buf[c].memblock)
            pa_memblock_unref(r->ffmpeg.buf[c].memblock);
}

static int ffmpeg_init(pa_resampler *r) {
    unsigned c;

    pa_assert(r);

    /* We could probably implement different quality levels by
     * adjusting the filter parameters here. However, ffmpeg
     * internally only uses these hardcoded values, so let's use them
     * here for now as well until ffmpeg makes this configurable. */

    if (!(r->ffmpeg.state = av_resample_init(r->o_ss.rate, r->i_ss.rate, 16, 10, 0, 0.8)))
        return -1;

    r->impl_free = ffmpeg_free;
    r->impl_resample = ffmpeg_resample;

    for (c = 0; c < PA_ELEMENTSOF(r->ffmpeg.buf); c++)
        pa_memchunk_reset(&r->ffmpeg.buf[c]);

    return 0;
}

/*** copy (noop) implementation ***/

static int copy_init(pa_resampler *r) {
    pa_assert(r);

    pa_assert(r->o_ss.rate == r->i_ss.rate);

    r->impl_free = NULL;
    r->impl_resample = NULL;
    r->impl_update_rates = NULL;

    return 0;
}
