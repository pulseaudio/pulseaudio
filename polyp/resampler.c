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

#include <samplerate.h>

#include "resampler.h"
#include "sconv.h"
#include "xmalloc.h"
#include "log.h"

struct pa_resampler {
    struct pa_sample_spec i_ss, o_ss;
    float* i_buf, *o_buf;
    unsigned i_alloc, o_alloc;
    size_t i_sz, o_sz;

    int channels;

    pa_convert_to_float32_func_t to_float32_func;
    pa_convert_from_float32_func_t from_float32_func;
    SRC_STATE *src_state;

    struct pa_memblock_stat *memblock_stat;
};

struct pa_resampler* pa_resampler_new(const struct pa_sample_spec *a, const struct pa_sample_spec *b, struct pa_memblock_stat *s, int resample_method) {
    struct pa_resampler *r = NULL;
    int err;
    assert(a && b && pa_sample_spec_valid(a) && pa_sample_spec_valid(b));

    if (a->channels != b->channels && a->channels != 1 && b->channels != 1)
        goto fail;

    if (a->format == PA_SAMPLE_ALAW || a->format == PA_SAMPLE_ULAW || b->format == PA_SAMPLE_ALAW || b->format == PA_SAMPLE_ULAW)
        goto fail;

    r = pa_xmalloc(sizeof(struct pa_resampler));

    r->channels = a->channels;
    if (b->channels < r->channels)
        r->channels = b->channels;
    
    r->i_buf = r->o_buf = NULL;
    r->i_alloc = r->o_alloc = 0;

    r->src_state = src_new(resample_method, r->channels, &err);
    if (err != 0 || !r->src_state)
        goto fail;

    r->i_ss = *a;
    r->o_ss = *b;

    r->i_sz = pa_frame_size(a);
    r->o_sz = pa_frame_size(b);

    r->to_float32_func = pa_get_convert_to_float32_function(a->format);
    r->from_float32_func = pa_get_convert_from_float32_function(b->format);

    assert(r->to_float32_func && r->from_float32_func);

    r->memblock_stat = s;
    
    return r;
    
fail:
    if (r)
        pa_xfree(r);
    
    return NULL;
}

void pa_resampler_free(struct pa_resampler *r) {
    assert(r);
    if (r->src_state)
        src_delete(r->src_state);
    pa_xfree(r->i_buf);
    pa_xfree(r->o_buf);
    pa_xfree(r);
}

size_t pa_resampler_request(struct pa_resampler *r, size_t out_length) {
    assert(r && (out_length % r->o_sz) == 0);
    
    return (((out_length / r->o_sz)*r->i_ss.rate)/r->o_ss.rate) * r->i_sz;
}


void pa_resampler_run(struct pa_resampler *r, const struct pa_memchunk *in, struct pa_memchunk *out) {
    unsigned i_nchannels, o_nchannels, ins, ons, eff_ins, eff_ons;
    float *cbuf;
    assert(r && in && out && in->length && in->memblock && (in->length % r->i_sz) == 0);

    /* How many input samples? */
    ins = in->length/r->i_sz;

/*     pa_log("%u / %u = %u\n", in->length, r->i_sz, ins); */

    /* How much space for output samples? */
    if (r->src_state)
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
    
    
    out->memblock = pa_memblock_new(out->length = (ons*r->o_sz), r->memblock_stat);
    out->index = 0;
    assert(out->memblock);

    if (r->i_alloc < eff_ins)
        r->i_buf = pa_xrealloc(r->i_buf, sizeof(float) * (r->i_alloc = eff_ins));
    assert(r->i_buf);

/*     pa_log("eff_ins = %u \n", eff_ins); */

    r->to_float32_func(eff_ins, (uint8_t*) in->memblock->data+in->index, i_nchannels, r->i_buf);

    if (r->src_state) {
        int ret;
        SRC_DATA data;

        if (r->o_alloc < eff_ons)
            r->o_buf = pa_xrealloc(r->o_buf, sizeof(float) * (r->o_alloc = eff_ons));
        assert(r->o_buf);

        data.data_in = r->i_buf;
        data.input_frames = ins;

        data.data_out = r->o_buf;
        data.output_frames = ons;
        
        data.src_ratio = (double) r->o_ss.rate / r->i_ss.rate;
        data.end_of_input = 0;
        
        ret = src_process(r->src_state, &data);
        assert(ret == 0);
        assert((unsigned) data.input_frames_used == ins);
        
        cbuf = r->o_buf;
        ons = data.output_frames_gen;

        if (r->i_ss.channels == r->o_ss.channels) 
            eff_ons = ons*r->o_ss.channels;
        else
            eff_ons = ons;
    } else
        cbuf = r->i_buf;

    if (eff_ons)
        r->from_float32_func(eff_ons, cbuf, (uint8_t*)out->memblock->data+out->index, o_nchannels);
    out->length = ons*r->o_sz;


    if (!out->length) {
        pa_memblock_unref(out->memblock);
        out->memblock = NULL;
    }
}

void pa_resampler_set_input_rate(struct pa_resampler *r, uint32_t rate) {
    int ret;
    assert(r);

    r->i_ss.rate = rate;
    ret = src_set_ratio(r->src_state, (double) r->o_ss.rate / r->i_ss.rate);
    assert(ret == 0);
}
