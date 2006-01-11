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

#include <assert.h>
#include <string.h>

#include <samplerate.h>

#include "resampler.h"
#include "sconv.h"
#include "xmalloc.h"
#include "log.h"

struct pa_resampler {
    pa_sample_spec i_ss, o_ss;
    size_t i_fz, o_fz;
    pa_memblock_stat *memblock_stat;
    void *impl_data;
    int channels;
    pa_resample_method resample_method;

    void (*impl_free)(pa_resampler *r);
    void (*impl_set_input_rate)(pa_resampler *r, uint32_t rate);
    void (*impl_run)(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out);
};

struct impl_libsamplerate {
    float* i_buf, *o_buf;
    unsigned i_alloc, o_alloc;
    pa_convert_to_float32ne_func_t to_float32ne_func;
    pa_convert_from_float32ne_func_t from_float32ne_func;
    SRC_STATE *src_state;
};

struct impl_trivial {
    unsigned o_counter;
    unsigned i_counter;
};

static int libsamplerate_init(pa_resampler*r);
static int trivial_init(pa_resampler*r);

pa_resampler* pa_resampler_new(const pa_sample_spec *a, const pa_sample_spec *b, pa_memblock_stat *s, pa_resample_method resample_method) {
    pa_resampler *r = NULL;
    assert(a && b && pa_sample_spec_valid(a) && pa_sample_spec_valid(b) && resample_method != PA_RESAMPLER_INVALID);

    if (a->channels != b->channels && a->channels != 1 && b->channels != 1)
        goto fail;

    r = pa_xmalloc(sizeof(pa_resampler));
    r->impl_data = NULL;
    r->memblock_stat = s;
    r->resample_method = resample_method;

    r->impl_free = NULL;
    r->impl_set_input_rate = NULL;
    r->impl_run = NULL;

    /* Fill sample specs */
    r->i_ss = *a;
    r->o_ss = *b;

    r->i_fz = pa_frame_size(a);
    r->o_fz = pa_frame_size(b);

    r->channels = a->channels;
    if (b->channels < r->channels)
        r->channels = b->channels;
    
    /* Choose implementation */
    if (a->channels != b->channels || a->format != b->format || resample_method != PA_RESAMPLER_TRIVIAL) {
        /* Use the libsamplerate based resampler for the complicated cases */
        if (resample_method == PA_RESAMPLER_TRIVIAL)
            r->resample_method = PA_RESAMPLER_SRC_ZERO_ORDER_HOLD;

        if (libsamplerate_init(r) < 0)
            goto fail;
        
    } else {
        /* Use our own simple non-fp resampler for the trivial cases and when the user selects it */
        if (trivial_init(r) < 0)
            goto fail;
    }
    
    return r;
    
fail:
    if (r)
        pa_xfree(r);
    
    return NULL;
}

void pa_resampler_free(pa_resampler *r) {
    assert(r);

    if (r->impl_free)
        r->impl_free(r);
    
    pa_xfree(r);
}

void pa_resampler_set_input_rate(pa_resampler *r, uint32_t rate) {
    assert(r && rate);

    r->i_ss.rate = rate;
    if (r->impl_set_input_rate)
        r->impl_set_input_rate(r, rate);
}

void pa_resampler_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    assert(r && in && out && r->impl_run);

    r->impl_run(r, in, out);
}

size_t pa_resampler_request(pa_resampler *r, size_t out_length) {
    assert(r && (out_length % r->o_fz) == 0);
    return (((out_length / r->o_fz)*r->i_ss.rate)/r->o_ss.rate) * r->i_fz;
}

pa_resample_method pa_resampler_get_method(pa_resampler *r) {
    assert(r);
    return r->resample_method;
}

/* Parse a libsamplrate compatible resampling implementation */
pa_resample_method pa_parse_resample_method(const char *string) {
    assert(string);

    if (!strcmp(string, "src-sinc-best-quality"))
        return PA_RESAMPLER_SRC_SINC_BEST_QUALITY;
    else if (!strcmp(string, "src-sinc-medium-quality"))
        return PA_RESAMPLER_SRC_SINC_MEDIUM_QUALITY;
    else if (!strcmp(string, "src-sinc-fastest"))
        return PA_RESAMPLER_SRC_SINC_FASTEST;
    else if (!strcmp(string, "src-zero-order-hold"))
        return PA_RESAMPLER_SRC_ZERO_ORDER_HOLD;
    else if (!strcmp(string, "src-linear"))
        return PA_RESAMPLER_SRC_LINEAR;
    else if (!strcmp(string, "trivial"))
        return PA_RESAMPLER_TRIVIAL;
    else
        return PA_RESAMPLER_INVALID;
}

/*** libsamplerate based implementation ***/

static void libsamplerate_free(pa_resampler *r) {
    struct impl_libsamplerate *i;
    assert(r && r->impl_data);
    i = r->impl_data;
    
    if (i->src_state)
        src_delete(i->src_state);

    pa_xfree(i->i_buf);
    pa_xfree(i->o_buf);
    pa_xfree(i);
}

static void libsamplerate_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    unsigned i_nchannels, o_nchannels, ins, ons, eff_ins, eff_ons;
    float *cbuf;
    struct impl_libsamplerate *i;
    assert(r && in && out && in->length && in->memblock && (in->length % r->i_fz) == 0 && r->impl_data);
    i = r->impl_data;

    /* How many input samples? */
    ins = in->length/r->i_fz;

/*     pa_log("%u / %u = %u\n", in->length, r->i_fz, ins); */

    /* How much space for output samples? */
    if (i->src_state)
        ons = (ins*r->o_ss.rate/r->i_ss.rate)+1024;
    else
        ons = ins;
    
    /* How many channels? */
    if (r->i_ss.channels == r->o_ss.channels) {
        i_nchannels = o_nchannels = 1;
        eff_ins = ins*r->i_ss.channels; /* effective samples */
        eff_ons = ons*r->o_ss.channels;
    } else {
        i_nchannels = r->i_ss.channels;
        o_nchannels = r->o_ss.channels;
        eff_ins = ins;
        eff_ons = ons;
    }

/*     pa_log("eff_ins = %u \n", eff_ins); */
    
    
    out->memblock = pa_memblock_new(out->length = (ons*r->o_fz), r->memblock_stat);
    out->index = 0;
    assert(out->memblock);

    if (i->i_alloc < eff_ins)
        i->i_buf = pa_xrealloc(i->i_buf, sizeof(float) * (i->i_alloc = eff_ins));
    assert(i->i_buf);

/*     pa_log("eff_ins = %u \n", eff_ins); */

    i->to_float32ne_func(eff_ins, (uint8_t*) in->memblock->data+in->index, i_nchannels, i->i_buf);

    if (i->src_state) {
        int ret;
        SRC_DATA data;

        if (i->o_alloc < eff_ons)
            i->o_buf = pa_xrealloc(i->o_buf, sizeof(float) * (i->o_alloc = eff_ons));
        assert(i->o_buf);

        data.data_in = i->i_buf;
        data.input_frames = ins;

        data.data_out = i->o_buf;
        data.output_frames = ons;
        
        data.src_ratio = (double) r->o_ss.rate / r->i_ss.rate;
        data.end_of_input = 0;
        
        ret = src_process(i->src_state, &data);
        assert(ret == 0);
        assert((unsigned) data.input_frames_used == ins);
        
        cbuf = i->o_buf;
        ons = data.output_frames_gen;

        if (r->i_ss.channels == r->o_ss.channels) 
            eff_ons = ons*r->o_ss.channels;
        else
            eff_ons = ons;
    } else
        cbuf = i->i_buf;

    if (eff_ons)
        i->from_float32ne_func(eff_ons, cbuf, (uint8_t*)out->memblock->data+out->index, o_nchannels);
    out->length = ons*r->o_fz;

    if (!out->length) {
        pa_memblock_unref(out->memblock);
        out->memblock = NULL;
    }
}

static void libsamplerate_set_input_rate(pa_resampler *r, uint32_t rate) {
    int ret;
    struct impl_libsamplerate *i;
    assert(r && rate > 0 && r->impl_data);
    i = r->impl_data;

    ret = src_set_ratio(i->src_state, (double) r->o_ss.rate / r->i_ss.rate);
    assert(ret == 0);
}

static int libsamplerate_init(pa_resampler *r) {
    struct impl_libsamplerate *i = NULL;
    int err;

    r->impl_data = i = pa_xmalloc(sizeof(struct impl_libsamplerate));
    
    i->to_float32ne_func = pa_get_convert_to_float32ne_function(r->i_ss.format);
    i->from_float32ne_func = pa_get_convert_from_float32ne_function(r->o_ss.format);

    if (!i->to_float32ne_func || !i->from_float32ne_func)
        goto fail;
    
    if (!(i->src_state = src_new(r->resample_method, r->channels, &err)) || !i->src_state)
        goto fail;

    i->i_buf = i->o_buf = NULL;
    i->i_alloc = i->o_alloc = 0;

    r->impl_free = libsamplerate_free;
    r->impl_set_input_rate = libsamplerate_set_input_rate;
    r->impl_run = libsamplerate_run;
    
    return 0;

fail:
    pa_xfree(i);
    return -1;
}

/* Trivial implementation */

static void trivial_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    size_t fz;
    unsigned  nframes;
    struct impl_trivial *i;
    assert(r && in && out && r->impl_data);
    i = r->impl_data;

    fz = r->i_fz;
    assert(fz == r->o_fz);

    nframes = in->length/fz;

    if (r->i_ss.rate == r->o_ss.rate) {

        /* In case there's no diefference in sample types, do nothing */
        *out = *in;
        pa_memblock_ref(out->memblock);

        i->o_counter += nframes;
    } else {
        /* Do real resampling */
        size_t l;
        unsigned o_index;
        
        /* The length of the new memory block rounded up */
        l = ((((nframes+1) * r->o_ss.rate) / r->i_ss.rate) + 1) * fz;
        
        out->index = 0;
        out->memblock = pa_memblock_new(l, r->memblock_stat);
        
        for (o_index = 0;; o_index++, i->o_counter++) {
            unsigned j;
            
            j = (i->o_counter * r->i_ss.rate / r->o_ss.rate);
            j = j > i->i_counter ? j - i->i_counter : 0;
            
            if (j >= nframes)
                break;

            assert(o_index*fz < out->memblock->length);
            
            memcpy((uint8_t*) out->memblock->data + fz*o_index,
                   (uint8_t*) in->memblock->data + in->index + fz*j, fz);
            
        }
            
        out->length = o_index*fz;
    }

    i->i_counter += nframes;
    
    /* Normalize counters */
    while (i->i_counter >= r->i_ss.rate) {
        i->i_counter -= r->i_ss.rate;
        assert(i->o_counter >= r->o_ss.rate);
        i->o_counter -= r->o_ss.rate;
    }
}

static void trivial_free(pa_resampler *r) {
    assert(r);
    pa_xfree(r->impl_data);
}

static void trivial_set_input_rate(pa_resampler *r, uint32_t rate) {
    struct impl_trivial *i;
    assert(r && rate > 0 && r->impl_data);
    i = r->impl_data;

    i->i_counter = 0;
    i->o_counter = 0;
}

static int trivial_init(pa_resampler*r) {
    struct impl_trivial *i;
    assert(r && r->i_ss.format == r->o_ss.format && r->i_ss.channels == r->o_ss.channels);

    r->impl_data = i = pa_xmalloc(sizeof(struct impl_trivial));
    i->o_counter = i->i_counter = 0;

    r->impl_run = trivial_run;
    r->impl_free = trivial_free;
    r->impl_set_input_rate = trivial_set_input_rate;
                                  
    return 0;
}

const char *pa_resample_method_to_string(pa_resample_method m) {
    static const char * const resample_methods[] = {
        "src-sinc-best-quality",
        "src-sinc-medium-quality",
        "src-sinc-fastest",
        "src-zero-order-hold",
        "src-linear",
        "trivial"
    };

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return NULL;

    return resample_methods[m];
}
