#include <stdlib.h>
#include <assert.h>

#include <samplerate.h>

#include "resampler.h"
#include "sconv.h"

struct resampler {
    struct pa_sample_spec i_ss, o_ss;
    float* i_buf, *o_buf;
    unsigned i_alloc, o_alloc;
    size_t i_sz, o_sz;

    int channels;

    convert_to_float32_func_t to_float32_func;
    convert_from_float32_func_t from_float32_func;
    SRC_STATE *src_state;
};

struct resampler* resampler_new(const struct pa_sample_spec *a, const struct pa_sample_spec *b) {
    struct resampler *r;
    int err;
    assert(a && b && pa_sample_spec_valid(a) && pa_sample_spec_valid(b));

    if (a->channels != b->channels && a->channels != 1 && b->channels != 1)
        goto fail;

    if (a->format == PA_SAMPLE_ALAW || a->format == PA_SAMPLE_ULAW || b->format == PA_SAMPLE_ALAW || b->format == PA_SAMPLE_ULAW)
        goto fail;

    r->channels = a->channels;
    if (b->channels < r->channels)
        r->channels = b->channels;
    
    r = malloc(sizeof(struct resampler));
    assert(r);
    r->i_buf = r->o_buf = NULL;
    r->i_alloc = r->o_alloc = 0;

    if (a->rate != b->rate) {
        r->src_state = src_new(SRC_SINC_FASTEST, r->channels, &err);
        if (err != 0 || !r->src_state)
            goto fail;
    } else
        r->src_state = NULL;

    r->i_ss = *a;
    r->o_ss = *b;

    r->i_sz = pa_sample_size(a);
    r->o_sz = pa_sample_size(b);

    r->to_float32_func = get_convert_to_float32_function(a->format);
    r->from_float32_func = get_convert_from_float32_function(b->format);

    assert(r->to_float32_func && r->from_float32_func);
    
    return r;
    
fail:
    if (r)
        free(r);
    
    return NULL;
}

void resampler_free(struct resampler *r) {
    assert(r);
    if (r->src_state)
        src_delete(r->src_state);
    free(r->i_buf);
    free(r->o_buf);
    free(r);
}

size_t resampler_request(struct resampler *r, size_t out_length) {
    assert(r && (out_length % r->o_sz) == 0);
    
    return (((out_length / r->o_sz)*r->i_ss.rate)/r->o_ss.rate) * r->i_sz;
}


int resampler_run(struct resampler *r, struct memchunk *in, struct memchunk *out) {
    unsigned i_nchannels, o_nchannels, ins, ons, eff_ins, eff_ons;
    float *cbuf;
    size_t in_bytes_used = 0;
    assert(r && in && out && in->length && in->memblock);

    /* How many input samples? */
    ins = in->length/r->i_sz;

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
    
    out->memblock = memblock_new(out->length = (ons*r->o_sz));
    out->index = 0;
    assert(out->memblock);

    if (r->i_alloc < eff_ins)
        r->i_buf = realloc(r->i_buf, sizeof(float) * (r->i_alloc = eff_ins));
    assert(r->i_buf);
    
    r->to_float32_func(eff_ins, in->memblock->data+in->index, i_nchannels, r->i_buf);

    if (r->src_state) {
        int ret;
        SRC_DATA data;

        if (r->o_alloc < eff_ons)
            r->o_buf = realloc(r->o_buf, sizeof(float) * (r->o_alloc = eff_ons));
        assert(r->o_buf);

        data.data_in = r->i_buf;
        data.input_frames = ins;

        data.data_out = r->o_buf;
        data.output_frames = ons;
        
        data.src_ratio = (double) r->o_ss.rate / r->i_ss.rate;
        data.end_of_input = 0;
        
        ret = src_process(r->src_state, &data);
        assert(ret == 0);

        in_bytes_used = data.input_frames_used*r->i_sz;
        cbuf = r->o_buf;
        ons = data.output_frames_gen;

        if (r->i_ss.channels == r->o_ss.channels) 
            eff_ons = ons*r->o_ss.channels;
        else
            eff_ons = ons;
    } else {
        in_bytes_used = ins*r->i_sz;
        cbuf = r->i_buf;
    }

    assert(in_bytes_used < in->length);
    in->index += in_bytes_used;
    in->length -= in_bytes_used;
    
    r->from_float32_func(eff_ons, cbuf, out->memblock->data+out->index, o_nchannels);
    out->length = ons*r->o_sz;
    return 0;
}
