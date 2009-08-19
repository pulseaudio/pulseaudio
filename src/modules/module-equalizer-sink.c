/***
This file is part of PulseAudio.

This module is based off Lennart Poettering's LADSPA sink and swaps out
LADSPA functionality for a dbus-aware STFT OLA based digital equalizer.
All new work is published under Pulseaudio's original license.
Copyright 2009 Jason Newton <nevion@gmail.com>

Original Author:
Copyright 2004-2008 Lennart Poettering

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

#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#include <math.h>
#include <fftw3.h>
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulse/i18n.h>
#include <pulse/timeval.h>

#include <pulsecore/core-rtclock.h>
#include <pulsecore/aupdate.h>
#include <pulsecore/core-error.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/shared.h>
#include <pulsecore/idxset.h>
#include <pulsecore/strlist.h>
#include <pulsecore/database.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/dbus-util.h>

#include <stdint.h>
#include <time.h>


//#undef __SSE2__
#ifdef __SSE2__
#include <xmmintrin.h>
#include <emmintrin.h>
#endif



#include "module-equalizer-sink-symdef.h"

PA_MODULE_AUTHOR("Jason Newton");
PA_MODULE_DESCRIPTION(_("General Purpose Equalizer"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(_("sink=<sink to connect to> "));

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)


struct userdata {
    pa_module *module;
    pa_sink *sink;
    pa_sink_input *sink_input;
    char *name;

    size_t channels;
    size_t fft_size;//length (res) of fft
    size_t window_size;/*
                        *sliding window size
                        *effectively chooses R
                        */
    size_t R;/* the hop size between overlapping windows
              * the latency of the filter, calculated from window_size
              * based on constraints of COLA and window function
              */
    size_t latency;//Really just R but made into it's own variable
    //for twiddling with pulseaudio
    size_t overlap_size;//window_size-R
    size_t samples_gathered;
    //message
    float X;
    float *H;//frequency response filter (magnitude based)
    float *W;//windowing function (time domain)
    float *work_buffer, **input, **overlap_accum;
    fftwf_complex *output_window;
    fftwf_plan forward_plan, inverse_plan;
    //size_t samplings;

    float Xs[2];
    float *Hs[2];//thread updatable copies
    pa_aupdate *a_H;
    pa_memchunk conv_buffer;
    pa_memblockq *input_q;
    pa_bool_t first_iteration;

    pa_dbus_protocol *dbus_protocol;
    char *dbus_path;

    pa_database *database;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "master",
    "format",
    "rate",
    "channels",
    "channel_map",
    NULL
};


#define v_size 4
#define SINKLIST "equalized_sinklist"
#define EQDB "equalizer_db"
#define FILTER_SIZE (u->fft_size / 2 + 1)
#define PROFILE_SIZE (FILTER_SIZE + 1)
static void dbus_init(struct userdata *u);
static void dbus_done(struct userdata *u);

static void hanning_window(float *W, size_t window_size){
    //h=.5*(1-cos(2*pi*j/(window_size+1)), COLA for R=(M+1)/2
    for(size_t i=0; i < window_size;++i){
        W[i] = (float).5*(1-cos(2*M_PI*i/(window_size+1)));
    }
}

static void fix_filter(float *H, size_t fft_size){
    //divide out the fft gain
    for(size_t i = 0; i < fft_size / 2 + 1; ++i){
        H[i] /= fft_size;
    }
}

static void interpolate(float *signal, size_t length, uint32_t *xs, float *ys, size_t n_points){
    //Note that xs must be monotonically increasing!
    float x_range_lower, x_range_upper, c0;
    pa_assert_se(n_points>=2);
    pa_assert_se(xs[0] == 0);
    pa_assert_se(xs[n_points - 1] == length - 1);
    for(size_t x = 0, x_range_lower_i = 0; x < length-1; ++x){
        pa_assert(x_range_lower_i < n_points-1);
        x_range_lower = (float) (xs[x_range_lower_i]);
        x_range_upper = (float) (xs[x_range_lower_i+1]);
        pa_assert_se(x_range_lower < x_range_upper);
        pa_assert_se(x >= x_range_lower);
        pa_assert_se(x <= x_range_upper);
        //bilinear-interpolation of coefficients specified
        c0 = (x-x_range_lower)/(x_range_upper-x_range_lower);
        pa_assert_se(c0 >= 0&&c0 <= 1.0);
        signal[x] = ((1.0f - c0) * ys[x_range_lower_i] + c0 * ys[x_range_lower_i + 1]);
        while(x >= xs[x_range_lower_i + 1]){
            x_range_lower_i++;
        }
    }
    signal[length-1]=ys[n_points-1];
}

static int is_monotonic(const uint32_t *xs,size_t length){
    if(length<2){
        return 1;
    }
    for(size_t i = 1; i < length; ++i){
        if(xs[i]<=xs[i-1]){
            return 0;
        }
    }
    return 1;
}


/* Called from I/O thread context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            //size_t fs=pa_frame_size(&u->sink->sample_spec);

            /* The sink is _put() before the sink input is, so let's
             * make sure we don't access it in that time. Also, the
             * sink input is first shut down, the sink second. */
            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
                !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =
                /* Get the latency of the master sink */
                pa_sink_get_latency_within_thread(u->sink_input->sink) +

                /* Add the latency internal to our sink input on top */
                pa_bytes_to_usec(pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq), &u->sink_input->sink->sample_spec);
            //    pa_bytes_to_usec(u->samples_gathered * fs, &u->sink->sample_spec);
            //+ pa_bytes_to_usec(u->latency * fs, ss)
            //+ pa_bytes_to_usec(pa_memblockq_get_length(u->input_q), ss);
            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}


/* Called from main context */
static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(state) ||
        !PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        return 0;

    pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);
    return 0;
}

/* Called from I/O thread context */
static void sink_request_rewind(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    /* Just hand this one over to the master sink */
    pa_sink_input_request_rewind(u->sink_input, s->thread_info.rewind_nbytes+pa_memblockq_get_length(u->input_q), TRUE, FALSE, FALSE);
}

/* Called from I/O thread context */
static void sink_update_requested_latency(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
            u->sink_input,
            pa_sink_get_requested_latency_within_thread(s));
}

//reference implementation
static void dsp_logic(
    float * restrict dst,//used as a temp array too, needs to be fft_length!
    float * restrict src,/*input data w/ overlap at start,
                               *automatically cycled in routine
                               */
    float * restrict overlap,//The size of the overlap
    const float * restrict H,//The freq. magnitude scalers filter
    const float * restrict W,//The windowing function
    fftwf_complex * restrict output_window,//The transformed window'd src
    struct userdata *u){
    //use a linear-phase sliding STFT and overlap-add method (for each channel)
    //zero padd the data
    memset(dst + u->window_size, 0, (u->fft_size - u->window_size) * sizeof(float));
    //window the data
    for(size_t j = 0;j < u->window_size; ++j){
        dst[j] = u->X * W[j] * src[j];
    }
    //Processing is done here!
    //do fft
    fftwf_execute_dft_r2c(u->forward_plan, dst, output_window);
    //perform filtering
    for(size_t j = 0; j < FILTER_SIZE; ++j){
        u->output_window[j][0] *= u->H[j];
        u->output_window[j][1] *= u->H[j];
    }
    //inverse fft
    fftwf_execute_dft_c2r(u->inverse_plan, output_window, dst);
    ////debug: tests overlaping add
    ////and negates ALL PREVIOUS processing
    ////yields a perfect reconstruction if COLA is held
    //for(size_t j = 0; j < u->window_size; ++j){
    //    u->work_buffer[j] = u->W[j] * u->input[c][j];
    //}

    //overlap add and preserve overlap component from this window (linear phase)
    for(size_t j = 0;j < u->overlap_size; ++j){
        u->work_buffer[j] += overlap[j];
        overlap[j] = dst[u->R+j];
    }
    ////debug: tests if basic buffering works
    ////shouldn't modify the signal AT ALL (beyond roundoff)
    //for(size_t j = 0; j < u->window_size;++j){
    //    u->work_buffer[j] = u->input[c][j];
    //}

    //preseve the needed input for the next window's overlap
    memmove(src, src+u->R,
        ((u->overlap_size + u->samples_gathered) - u->R)*sizeof(float)
    );
}

typedef float v4sf __attribute__ ((__aligned__(v_size * sizeof(float))));
typedef union float_vector {
    float f[v_size];
    v4sf v;
#ifdef __SSE2__
    __m128 m;
#endif
} float_vector_t;

////regardless of sse enabled, the loops in here assume
////16 byte aligned addresses and memory allocations divisible by v_size
//void dsp_logic(
//    float * restrict dst,//used as a temp array too, needs to be fft_length!
//    float * restrict src,/*input data w/ overlap at start,
//                               *automatically cycled in routine
//                               */
//    float * restrict overlap,//The size of the overlap
//    const float * restrict H,//The freq. magnitude scalers filter
//    const float * restrict W,//The windowing function
//    fftwf_complex * restrict output_window,//The transformed window'd src
//    struct userdata *u){//Collection of constants
      //float_vector_t x = {u->X, u->X, u->X, u->X};
//    const size_t window_size = PA_ROUND_UP(u->window_size,v_size);
//    const size_t fft_h = PA_ROUND_UP(FILTER_SIZE, v_size / 2);
//    //const size_t R = PA_ROUND_UP(u->R, v_size);
//    const size_t overlap_size = PA_ROUND_UP(u->overlap_size, v_size);
//     overlap_size = PA_ROUND_UP(u->overlap_size, v_size);
//
//    //assert(u->samples_gathered >= u->R);
//    //zero out the bit beyond the real overlap so we don't add garbage
//    for(size_t j = overlap_size; j > u->overlap_size; --j){
//       overlap[j-1] = 0;
//    }
//    //use a linear-phase sliding STFT and overlap-add method
//    //zero padd the data
//    memset(dst + u->window_size, 0, (u->fft_size - u->window_size)*sizeof(float));
//    //window the data
//    for(size_t j = 0; j < window_size; j += v_size){
//        //dst[j] = W[j]*src[j];
//        float_vector_t *d = (float_vector_t*) (dst+j);
//        float_vector_t *w = (float_vector_t*) (W+j);
//        float_vector_t *s = (float_vector_t*) (src+j);
//#if __SSE2__
//        d->m = _mm_mul_ps(x->m, _mm_mul_ps(w->m, s->m));
//#else
//        d->v = x->v * w->v * s->v;
//#endif
//    }
//    //Processing is done here!
//    //do fft
//    fftwf_execute_dft_r2c(u->forward_plan, dst, output_window);
//
//
//    //perform filtering - purely magnitude based
//    for(size_t j = 0;j < fft_h; j+=v_size/2){
//        //output_window[j][0]*=H[j];
//        //output_window[j][1]*=H[j];
//        float_vector_t *d = (float_vector_t*)(output_window+j);
//        float_vector_t h;
//        h.f[0] = h.f[1] = H[j];
//        h.f[2] = h.f[3] = H[j+1];
//#if __SSE2__
//        d->m = _mm_mul_ps(d->m, h.m);
//#else
//        d->v = d->v*h->v;
//#endif
//    }
//    //inverse fft
//    fftwf_execute_dft_c2r(u->inverse_plan, output_window, dst);
//
//    ////debug: tests overlaping add
//    ////and negates ALL PREVIOUS processing
//    ////yields a perfect reconstruction if COLA is held
//    //for(size_t j = 0; j < u->window_size; ++j){
//    //    dst[j] = W[j]*src[j];
//    //}
//
//    //overlap add and preserve overlap component from this window (linear phase)
//    for(size_t j = 0; j < overlap_size; j+=v_size){
//        //dst[j]+=overlap[j];
//        //overlap[j]+=dst[j+R];
//        float_vector_t *d = (float_vector_t*)(dst+j);
//        float_vector_t *o = (float_vector_t*)(overlap+j);
//#if __SSE2__
//        d->m = _mm_add_ps(d->m, o->m);
//        o->m = ((float_vector_t*)(dst+u->R+j))->m;
//#else
//        d->v = d->v+o->v;
//        o->v = ((float_vector_t*)(dst+u->R+j))->v;
//#endif
//    }
//    //memcpy(overlap, dst+u->R, u->overlap_size*sizeof(float));
//
//    //////debug: tests if basic buffering works
//    //////shouldn't modify the signal AT ALL (beyond roundoff)
//    //for(size_t j = 0; j < u->window_size; ++j){
//    //    dst[j] = src[j];
//    //}
//
//    //preseve the needed input for the next window's overlap
//    memmove(src, src+u->R,
//        ((u->overlap_size+u->samples_gathered)+-u->R)*sizeof(float)
//    );
//}

static void process_samples(struct userdata *u, pa_memchunk *tchunk){
    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    float *dst;
    pa_assert(u->samples_gathered >= u->R);
    tchunk->index = 0;
    tchunk->length = u->R * fs;
    tchunk->memblock = pa_memblock_new(u->sink->core->mempool, tchunk->length);
    dst = ((float*)pa_memblock_acquire(tchunk->memblock));
    for(size_t c=0;c < u->channels; c++) {
        dsp_logic(
            u->work_buffer,
            u->input[c],
            u->overlap_accum[c],
            u->H,
            u->W,
            u->output_window,
            u
        );
        if(u->first_iteration){
            /* The windowing function will make the audio ramped in, as a cheap fix we can
             * undo the windowing (for non-zero window values)
             */
            for(size_t i = 0;i < u->overlap_size; ++i){
                u->work_buffer[i] = u->W[i] <= FLT_EPSILON ? u->work_buffer[i] : u->work_buffer[i] / u->W[i];
            }
        }
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE, dst + c, fs, u->work_buffer, sizeof(float), u->R);
    }
    pa_memblock_release(tchunk->memblock);
    u->samples_gathered -= u->R;
}

static void initialize_buffer(struct userdata *u, pa_memchunk *in){
    size_t fs = pa_frame_size(&u->sink->sample_spec);
    size_t samples = in->length / fs;
    float *src = (float*) ((uint8_t*) pa_memblock_acquire(in->memblock) + in->index);
    pa_assert_se(u->samples_gathered + samples <= u->window_size);
    for(size_t c = 0; c < u->channels; c++) {
        //buffer with an offset after the overlap from previous
        //iterations
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE, u->input[c] + u->samples_gathered, sizeof(float), src + c, fs, samples);
    }
    u->samples_gathered += samples;
    pa_memblock_release(in->memblock);
}

static void input_buffer(struct userdata *u, pa_memchunk *in){
    size_t fs = pa_frame_size(&(u->sink->sample_spec));
    size_t samples = in->length/fs;
    float *src = (float*) ((uint8_t*) pa_memblock_acquire(in->memblock) + in->index);
    pa_assert_se(samples <= u->window_size - u->samples_gathered);
    for(size_t c = 0; c < u->channels; c++) {
        //buffer with an offset after the overlap from previous
        //iterations
        pa_assert_se(
            u->input[c]+u->samples_gathered+samples <= u->input[c]+u->window_size
        );
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE, u->input[c]+u->overlap_size+u->samples_gathered, sizeof(float), src + c, fs, samples);
    }
    u->samples_gathered += samples;
    pa_memblock_release(in->memblock);
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;
    size_t fs;
    struct timeval start, end;
    unsigned a_i;
    pa_memchunk tchunk;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_assert(chunk);
    pa_assert(u->sink);
    fs = pa_frame_size(&(u->sink->sample_spec));
    chunk->memblock = NULL;

    /* Hmm, process any rewind request that might be queued up */
    pa_sink_process_rewind(u->sink, 0);

    //pa_log_debug("start output-buffered %ld, input-buffered %ld, requested %ld",buffered_samples,u->samples_gathered,samples_requested);
    pa_rtclock_get(&start);
    do{
        size_t input_remaining = u->window_size - u->samples_gathered;
        pa_assert(input_remaining > 0);
        //collect samples

        //buffer = &u->conv_buffer;
        //buffer->length = input_remaining*fs;
        //buffer->index = 0;
        //pa_memblock_ref(buffer->memblock);
        //pa_sink_render_into(u->sink, buffer);
        while(pa_memblockq_peek(u->input_q, &tchunk) < 0){
            pa_sink_render(u->sink, input_remaining*fs, &tchunk);
            pa_assert(tchunk.memblock);
            pa_memblockq_push(u->input_q, &tchunk);
            pa_memblock_unref(tchunk.memblock);
        }
        pa_assert(tchunk.memblock);
        tchunk.length = PA_MIN(input_remaining * fs, tchunk.length);
        pa_memblockq_drop(u->input_q, tchunk.length);
        //pa_log_debug("asked for %ld input samples, got %ld samples",input_remaining,buffer->length/fs);
        /* copy new input */
        //pa_rtclock_get(start);
        if(u->first_iteration){
            initialize_buffer(u, &tchunk);
        }else{
            input_buffer(u, &tchunk);
        }
        //pa_rtclock_get(&end);
        //pa_log_debug("Took %0.5f seconds to setup", pa_timeval_diff(end, start) / (double) PA_USEC_PER_SEC);
        pa_memblock_unref(tchunk.memblock);
    }while(u->samples_gathered < u->window_size);
    pa_rtclock_get(&end);
    pa_log_debug("Took %0.6f seconds to get data", (double) pa_timeval_diff(&end, &start) / PA_USEC_PER_SEC);

    pa_assert(u->fft_size >= u->window_size);
    pa_assert(u->R < u->window_size);
    /* set the H filter */
    a_i = pa_aupdate_read_begin(u->a_H);
    u->X = u->Xs[a_i];
    u->H = u->Hs[a_i];
    pa_rtclock_get(&start);
    /* process a block */
    process_samples(u, chunk);
    pa_rtclock_get(&end);
    pa_log_debug("Took %0.6f seconds to process", (double) pa_timeval_diff(&end, &start) / PA_USEC_PER_SEC);
    pa_aupdate_read_end(u->a_H);

    pa_assert(chunk->memblock);
    //pa_log_debug("gave %ld", chunk->length/fs);
    //pa_log_debug("end pop");
    if(u->first_iteration){
        u->first_iteration = FALSE;
    }
    return 0;
}

static void reset_filter(struct userdata *u){
    u->samples_gathered = 0;
    for(size_t i = 0;i < u->channels; ++i){
        memset(u->overlap_accum[i], 0, u->overlap_size * sizeof(float));
    }
    u->first_iteration = TRUE;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    size_t amount = 0;

    pa_log_debug("Rewind callback!");
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (u->sink->thread_info.rewind_nbytes > 0) {
        size_t max_rewrite;

        //max_rewrite = nbytes;
        max_rewrite = nbytes + pa_memblockq_get_length(u->input_q);
        //PA_MIN(pa_memblockq_get_length(u->input_q), nbytes);
        amount = PA_MIN(u->sink->thread_info.rewind_nbytes, max_rewrite);
        u->sink->thread_info.rewind_nbytes = 0;

        if (amount > 0) {
            //pa_sample_spec *ss = &u->sink->sample_spec;
            //invalidate the output q
            pa_memblockq_seek(u->input_q, - (int64_t) amount, PA_SEEK_RELATIVE, TRUE);
            //pa_memblockq_drop(u->input_q, pa_memblockq_get_length(u->input_q));
            //pa_memblockq_seek(u->input_q, - (int64_t) amount, PA_SEEK_RELATIVE, TRUE);
            pa_log("Resetting filter");
            reset_filter(u);
        }
    }

    pa_sink_process_rewind(u->sink, amount);
    pa_memblockq_rewind(u->input_q, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_memblockq_set_maxrewind(u->input_q, nbytes);
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    size_t fs;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    fs = pa_frame_size(&(u->sink->sample_spec));
    //pa_sink_set_max_request_within_thread(u->sink, nbytes);
    //pa_sink_set_max_request_within_thread(u->sink, u->R*fs);
    pa_sink_set_max_request_within_thread(u->sink, ((nbytes+u->R*fs-1)/(u->R*fs))*(u->R*fs));
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    //pa_sink_set_latency_range_within_thread(u->sink, u->master->thread_info.min_latency, u->latency*fs);
    //pa_sink_set_latency_range_within_thread(u->sink, u->latency*fs, u->latency*fs );
    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_detach_within_thread(u->sink);

    pa_sink_set_rtpoll(u->sink, NULL);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;
    size_t fs;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_rtpoll(u->sink, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
    fs = pa_frame_size(&(u->sink->sample_spec));
    pa_sink_set_max_request_within_thread(u->sink, PA_ROUND_UP(pa_sink_input_get_max_request(i), u->R*fs));

    //pa_sink_set_latency_range_within_thread(u->sink, u->latency*fs, u->latency*fs);
    //pa_sink_set_latency_range_within_thread(u->sink, u->latency*fs, u->master->thread_info.max_latency);
    //TODO: setting this guy minimizes drop outs but doesn't get rid
    //of them completely, figure out why
    //pa_sink_set_latency_range_within_thread(u->sink, u->master->thread_info.min_latency, u->latency*fs);
    //TODO: this guy causes dropouts constantly+rewinds, it's unusable
    //pa_sink_set_latency_range_within_thread(u->sink, u->master->thread_info.min_latency, u->master->thread_info.max_latency);
    pa_sink_attach_within_thread(u->sink);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* The order here matters! We first kill the sink input, followed
     * by the sink. That means the sink callbacks must be protected
     * against an unconnected sink input! */
    pa_sink_input_unlink(u->sink_input);
    pa_sink_unlink(u->sink);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_sink_unref(u->sink);
    u->sink = NULL;

    pa_module_unload_request(u->module, TRUE);
}

/* Called from IO thread context */
static void sink_input_state_change_cb(pa_sink_input *i, pa_sink_input_state_t state) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* If we are added for the first time, ask for a rewinding so that
     * we are heard right-away. */
    if (PA_SINK_INPUT_IS_LINKED(state) &&
        i->thread_info.state == PA_SINK_INPUT_INIT) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(i, 0, FALSE, TRUE, TRUE);
    }
}

static void save_profile(struct userdata *u, char *name){
    unsigned a_i;
    const size_t profile_size = PROFILE_SIZE * sizeof(float);
    float *H_n, *profile;
    const float *H;
    pa_datum key, data;
    profile = pa_xnew0(float, profile_size);
    a_i = pa_aupdate_read_begin(u->a_H);
    H_n = profile + 1;
    H = u->Hs[a_i];
    profile[0] = u->Xs[a_i];
    for(size_t i = 0 ; i <= FILTER_SIZE; ++i){
        //H_n[i] = H[i] * u->fft_size;
        H_n[i] = H[i];
    }
    pa_aupdate_read_end(u->a_H);
    key.data=name;
    key.size = strlen(key.data);
    data.data = profile;
    data.size = profile_size;
    pa_database_set(u->database, &key, &data, TRUE);
    pa_database_sync(u->database);
}

static void save_state(struct userdata *u){
    char *state_name = pa_sprintf_malloc("%s-previous-state", u->name);
    save_profile(u, state_name);
    pa_xfree(state_name);
}

static void remove_profile(pa_core *c, char *name){
    pa_datum key;
    pa_database *database;
    key.data = name;
    key.size = strlen(key.data);
    pa_assert_se(database = pa_shared_get(c, EQDB));
    pa_database_unset(database, &key);
    pa_database_sync(database);
}

static const char* load_profile(struct userdata *u, char *name){
    unsigned a_i;
    pa_datum key, value;
    const size_t profile_size = PROFILE_SIZE * sizeof(float);
    key.data = name;
    key.size = strlen(key.data);
    if(pa_database_get(u->database, &key, &value) != NULL){
        if(value.size == profile_size){
            float *H = (float *) value.data;
            a_i = pa_aupdate_write_begin(u->a_H);
            u->Xs[a_i] = H[0];
            memcpy(u->Hs[a_i], H + 1, (FILTER_SIZE) * sizeof(float));
            pa_aupdate_write_end(u->a_H);
        }else{
            return "incompatible size";
        }
        pa_datum_free(&value);
    }else{
        return "profile doesn't exist";
    }
    return NULL;
    //fix_filter(u->H, u->fft_size);
}

static void load_state(struct userdata *u){
    char *state_name=pa_sprintf_malloc("%s-previous-state", u->name);
    load_profile(u,state_name);
    pa_xfree(state_name);
}

/* Called from main context */
static pa_bool_t sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    return u->sink != dest;
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_asyncmsgq(u->sink, dest->asyncmsgq);
    pa_sink_update_flags(u->sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, dest->flags);
}

//ensure's memory allocated is a multiple of v_size
//and aligned
static void * alloc(size_t x,size_t s){
    size_t f = PA_ROUND_UP(x*s, sizeof(float)*v_size);
    float *t;
    pa_assert(f >= x*s);
    //printf("requested %ld floats=%ld bytes, rem=%ld\n", x, x*sizeof(float), x*sizeof(float)%16);
    //printf("giving %ld floats=%ld bytes, rem=%ld\n", f, f*sizeof(float), f*sizeof(float)%16);
    t = fftwf_malloc(f);
    memset(t, 0, f);
    return t;
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    const char *z;
    pa_sink *master;
    pa_sink_input_new_data sink_input_data;
    pa_sink_new_data sink_data;
    pa_bool_t *use_default = NULL;
    size_t fs;
    float *H;
    unsigned a_i;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (!(master = pa_namereg_get(m->core, pa_modargs_get_value(ma, "master", NULL), PA_NAMEREG_SINK))) {
        pa_log("Master sink not found");
        goto fail;
    }

    ss = master->sample_spec;
    ss.format = PA_SAMPLE_FLOAT32;
    map = master->channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }
    fs = pa_frame_size(&ss);

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;

    u->channels = ss.channels;
    u->fft_size = pow(2, ceil(log(ss.rate)/log(2)));
    pa_log_debug("fft size: %ld", u->fft_size);
    u->window_size = 15999;
    u->R = (u->window_size + 1) / 2;
    u->overlap_size = u->window_size - u->R;
    u->samples_gathered = 0;
    u->a_H = pa_aupdate_new();
    u->latency = u->window_size - u->R;
    for(size_t i = 0; i < 2; ++i){
        u->Hs[i] = alloc((FILTER_SIZE), sizeof(float));
    }
    u->W = alloc(u->window_size, sizeof(float));
    u->work_buffer = alloc(u->fft_size, sizeof(float));
    memset(u->work_buffer, 0, u->fft_size*sizeof(float));
    u->input = pa_xnew0(float *, u->channels);
    u->overlap_accum = pa_xnew0(float *, u->channels);
    for(size_t c = 0; c < u->channels; ++c){
        u->input[c] = alloc(u->window_size, sizeof(float));
        memset(u->input[c], 0, (u->window_size)*sizeof(float));
        u->overlap_accum[c] = alloc(u->overlap_size, sizeof(float));
        memset(u->overlap_accum[c], 0, u->overlap_size*sizeof(float));
    }
    u->output_window = alloc((FILTER_SIZE), sizeof(fftwf_complex));
    u->forward_plan = fftwf_plan_dft_r2c_1d(u->fft_size, u->work_buffer, u->output_window, FFTW_ESTIMATE);
    u->inverse_plan = fftwf_plan_dft_c2r_1d(u->fft_size, u->output_window, u->work_buffer, FFTW_ESTIMATE);

    hanning_window(u->W, u->window_size);
    u->first_iteration = TRUE;

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    if (!(sink_data.name = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL))))
        sink_data.name = pa_sprintf_malloc("%s.equalizer", master->name);
    pa_sink_new_data_set_sample_spec(&sink_data, &ss);
    pa_sink_new_data_set_channel_map(&sink_data, &map);
    z = pa_proplist_gets(master->proplist, PA_PROP_DEVICE_DESCRIPTION);
    pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "FFT based equalizer on %s",z? z: master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    u->sink = pa_sink_new(m->core, &sink_data, master->flags & (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY));
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }
    u->name=pa_xstrdup(u->sink->name);
    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state = sink_set_state;
    u->sink->update_requested_latency = sink_update_requested_latency;
    u->sink->request_rewind = sink_request_rewind;
    u->sink->userdata = u;
    u->input_q = pa_memblockq_new(0,  MEMBLOCKQ_MAXLENGTH, 0, fs, 1, 1, 0, &u->sink->silence);

    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);
    //pa_sink_set_fixed_latency(u->sink, pa_bytes_to_usec(u->R*fs, &ss));

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    sink_input_data.sink = master;
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Equalized Stream");
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map);

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data, 0);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input)
        goto fail;

    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    u->sink_input->update_sink_fixed_latency = sink_input_update_sink_fixed_latency_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->state_change = sink_input_state_change_cb;
    u->sink_input->may_move_to = sink_input_may_move_to_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->userdata = u;

    pa_sink_put(u->sink);
    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);

    pa_xfree(use_default);

    dbus_init(u);

    //default filter to these
    a_i = pa_aupdate_write_begin(u->a_H);
    H = u->Hs[a_i];
    u->Xs[a_i] = 1.0f;
    for(size_t i = 0; i < FILTER_SIZE; ++i){
        H[i] = 1.0 / sqrtf(2.0f);
    }
    fix_filter(H, u->fft_size);
    pa_aupdate_write_end(u->a_H);
    //load old parameters
    load_state(u);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(use_default);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    save_state(u);

    dbus_done(u);

    /* See comments in sink_input_kill_cb() above regarding
     * destruction order! */

    if (u->sink_input)
        pa_sink_input_unlink(u->sink_input);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->sink_input)
        pa_sink_input_unref(u->sink_input);

    if (u->sink)
        pa_sink_unref(u->sink);

    pa_aupdate_free(u->a_H);
    pa_memblockq_free(u->input_q);

    fftwf_destroy_plan(u->inverse_plan);
    fftwf_destroy_plan(u->forward_plan);
    pa_xfree(u->output_window);
    for(size_t c=0; c < u->channels; ++c){
        pa_xfree(u->overlap_accum[c]);
        pa_xfree(u->input[c]);
    }
    pa_xfree(u->overlap_accum);
    pa_xfree(u->input);
    pa_xfree(u->work_buffer);
    pa_xfree(u->W);
    for(size_t i = 0; i < 2; ++i){
        pa_xfree(u->Hs[i]);
    }

    pa_xfree(u->name);

    pa_xfree(u);
}

/*
 * DBus Routines and Callbacks
 */
#define EXTNAME "org.PulseAudio.Ext.Equalizing1"
#define MANAGER_PATH "/org/pulseaudio/equalizing1"
#define MANAGER_IFACE EXTNAME ".Manager"
#define EQUALIZER_IFACE EXTNAME ".Equalizer"
static void manager_get_revision(DBusConnection *conn, DBusMessage *msg, void *_u);
static void manager_get_sinks(DBusConnection *conn, DBusMessage *msg, void *_u);
static void manager_get_profiles(DBusConnection *conn, DBusMessage *msg, void *_u);
static void manager_get_all(DBusConnection *conn, DBusMessage *msg, void *_u);
static void manager_handle_remove_profile(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_get_revision(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_get_sample_rate(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_get_filter_rate(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_get_n_coefs(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_get_filter(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_set_filter(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_get_all(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_handle_seed_filter(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_handle_get_filter_points(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_handle_save_profile(DBusConnection *conn, DBusMessage *msg, void *_u);
static void equalizer_handle_load_profile(DBusConnection *conn, DBusMessage *msg, void *_u);
enum manager_method_index {
    MANAGER_METHOD_REMOVE_PROFILE,
    MANAGER_METHOD_MAX
};

pa_dbus_arg_info remove_profile_args[]={
    {"name", "s","in"},
};

static pa_dbus_method_handler manager_methods[MANAGER_METHOD_MAX]={
    [MANAGER_METHOD_REMOVE_PROFILE]{
        .method_name="RemoveProfile",
        .arguments=remove_profile_args,
        .n_arguments=sizeof(remove_profile_args)/sizeof(pa_dbus_arg_info),
        .receive_cb=manager_handle_remove_profile}
};

enum manager_handler_index {
    MANAGER_HANDLER_REVISION,
    MANAGER_HANDLER_EQUALIZED_SINKS,
    MANAGER_HANDLER_PROFILES,
    MANAGER_HANDLER_MAX
};

static pa_dbus_property_handler manager_handlers[MANAGER_HANDLER_MAX]={
    [MANAGER_HANDLER_REVISION]={.property_name="InterfaceRevision",.type="u",.get_cb=manager_get_revision,.set_cb=NULL},
    [MANAGER_HANDLER_EQUALIZED_SINKS]={.property_name="EqualizedSinks",.type="ao",.get_cb=manager_get_sinks,.set_cb=NULL},
    [MANAGER_HANDLER_PROFILES]={.property_name="Profiles",.type="as",.get_cb=manager_get_profiles,.set_cb=NULL}
};

pa_dbus_arg_info sink_args[]={
    {"sink", "o", NULL}
};

enum manager_signal_index{
    MANAGER_SIGNAL_SINK_ADDED,
    MANAGER_SIGNAL_SINK_REMOVED,
    MANAGER_SIGNAL_PROFILES_CHANGED,
    MANAGER_SIGNAL_MAX
};

static pa_dbus_signal_info manager_signals[MANAGER_SIGNAL_MAX]={
    [MANAGER_SIGNAL_SINK_ADDED]={.name="SinkAdded", .arguments=sink_args, .n_arguments=sizeof(sink_args)/sizeof(pa_dbus_arg_info)},
    [MANAGER_SIGNAL_SINK_REMOVED]={.name="SinkRemoved", .arguments=sink_args, .n_arguments=sizeof(sink_args)/sizeof(pa_dbus_arg_info)},
    [MANAGER_SIGNAL_PROFILES_CHANGED]={.name="ProfilesChanged", .arguments=NULL, .n_arguments=0}
};

static pa_dbus_interface_info manager_info={
    .name=MANAGER_IFACE,
    .method_handlers=manager_methods,
    .n_method_handlers=MANAGER_METHOD_MAX,
    .property_handlers=manager_handlers,
    .n_property_handlers=MANAGER_HANDLER_MAX,
    .get_all_properties_cb=manager_get_all,
    .signals=manager_signals,
    .n_signals=MANAGER_SIGNAL_MAX
};

enum equalizer_method_index {
    EQUALIZER_METHOD_FILTER_POINTS,
    EQUALIZER_METHOD_SEED_FILTER,
    EQUALIZER_METHOD_SAVE_PROFILE,
    EQUALIZER_METHOD_LOAD_PROFILE,
    EQUALIZER_METHOD_MAX
};

enum equalizer_handler_index {
    EQUALIZER_HANDLER_REVISION,
    EQUALIZER_HANDLER_SAMPLERATE,
    EQUALIZER_HANDLER_FILTERSAMPLERATE,
    EQUALIZER_HANDLER_N_COEFS,
    EQUALIZER_HANDLER_FILTER,
    EQUALIZER_HANDLER_PREAMP,
    EQUALIZER_HANDLER_MAX
};

pa_dbus_arg_info filter_points_args[]={
    {"xs", "au","in"},
    {"ys", "ad","out"},
    {"preamp", "d","out"},
};
pa_dbus_arg_info seed_filter_args[]={
    {"xs", "au","in"},
    {"ys", "ad","in"},
    {"preamp", "d","in"},
};
pa_dbus_arg_info save_profile_args[]={
    {"name", "s","in"},
};
pa_dbus_arg_info load_profile_args[]={
    {"name", "s","in"},
};

static pa_dbus_method_handler equalizer_methods[EQUALIZER_METHOD_MAX]={
    [EQUALIZER_METHOD_SEED_FILTER]{
        .method_name="SeedFilter",
        .arguments=seed_filter_args,
        .n_arguments=sizeof(seed_filter_args)/sizeof(pa_dbus_arg_info),
        .receive_cb=equalizer_handle_seed_filter},
    [EQUALIZER_METHOD_FILTER_POINTS]{
        .method_name="FilterAtPoints",
        .arguments=filter_points_args,
        .n_arguments=sizeof(filter_points_args)/sizeof(pa_dbus_arg_info),
        .receive_cb=equalizer_handle_get_filter_points},
    [EQUALIZER_METHOD_SAVE_PROFILE]{
        .method_name="SaveProfile",
        .arguments=save_profile_args,
        .n_arguments=sizeof(save_profile_args)/sizeof(pa_dbus_arg_info),
        .receive_cb=equalizer_handle_save_profile},
    [EQUALIZER_METHOD_LOAD_PROFILE]{
        .method_name="LoadProfile",
        .arguments=load_profile_args,
        .n_arguments=sizeof(load_profile_args)/sizeof(pa_dbus_arg_info),
        .receive_cb=equalizer_handle_load_profile},
};

static pa_dbus_property_handler equalizer_handlers[EQUALIZER_HANDLER_MAX]={
    [EQUALIZER_HANDLER_REVISION]={.property_name="InterfaceRevision",.type="u",.get_cb=equalizer_get_revision,.set_cb=NULL},
    [EQUALIZER_HANDLER_SAMPLERATE]{.property_name="SampleRate",.type="u",.get_cb=equalizer_get_sample_rate,.set_cb=NULL},
    [EQUALIZER_HANDLER_FILTERSAMPLERATE]{.property_name="FilterSampleRate",.type="u",.get_cb=equalizer_get_filter_rate,.set_cb=NULL},
    [EQUALIZER_HANDLER_N_COEFS]{.property_name="NFilterCoefficients",.type="u",.get_cb=equalizer_get_n_coefs,.set_cb=NULL},
    [EQUALIZER_HANDLER_FILTER]{.property_name="Filter",.type="ad",.get_cb=equalizer_get_filter,.set_cb=equalizer_set_filter}
};

enum equalizer_signal_index{
    EQUALIZER_SIGNAL_FILTER_CHANGED,
    EQUALIZER_SIGNAL_SINK_RECONFIGURED,
    EQUALIZER_SIGNAL_MAX
};

static pa_dbus_signal_info equalizer_signals[EQUALIZER_SIGNAL_MAX]={
    [EQUALIZER_SIGNAL_FILTER_CHANGED]={.name="FilterChanged", .arguments=NULL, .n_arguments=0},
    [EQUALIZER_SIGNAL_SINK_RECONFIGURED]={.name="SinkReconfigured", .arguments=NULL, .n_arguments=0},
};

static pa_dbus_interface_info equalizer_info={
    .name=EQUALIZER_IFACE,
    .method_handlers=equalizer_methods,
    .n_method_handlers=EQUALIZER_METHOD_MAX,
    .property_handlers=equalizer_handlers,
    .n_property_handlers=EQUALIZER_HANDLER_MAX,
    .get_all_properties_cb=equalizer_get_all,
    .signals=equalizer_signals,
    .n_signals=EQUALIZER_SIGNAL_MAX
};

void dbus_init(struct userdata *u){
    uint32_t dummy;
    DBusMessage *signal = NULL;
    pa_idxset *sink_list = NULL;
    u->dbus_protocol=pa_dbus_protocol_get(u->sink->core);
    u->dbus_path=pa_sprintf_malloc("/org/pulseaudio/core1/sink%d", u->sink->index);

    pa_dbus_protocol_add_interface(u->dbus_protocol, u->dbus_path, &equalizer_info, u);
    sink_list = pa_shared_get(u->sink->core, SINKLIST);
    u->database=pa_shared_get(u->sink->core, EQDB);
    if(sink_list==NULL){
        char *dbname;
        sink_list=pa_idxset_new(&pa_idxset_trivial_hash_func, &pa_idxset_trivial_compare_func);
        pa_shared_set(u->sink->core, SINKLIST, sink_list);
        pa_assert_se(dbname = pa_state_path("equalizers", TRUE));
        pa_assert_se(u->database = pa_database_open(dbname, TRUE));
        pa_xfree(dbname);
        pa_shared_set(u->sink->core,EQDB,u->database);
        pa_dbus_protocol_add_interface(u->dbus_protocol, MANAGER_PATH, &manager_info, u->sink->core);
        pa_dbus_protocol_register_extension(u->dbus_protocol, EXTNAME);
    }
    pa_idxset_put(sink_list, u, &dummy);

    pa_assert_se((signal = dbus_message_new_signal(MANAGER_PATH, MANAGER_IFACE, manager_signals[MANAGER_SIGNAL_SINK_ADDED].name)));
    dbus_message_append_args(signal, DBUS_TYPE_OBJECT_PATH, &u->dbus_path, DBUS_TYPE_INVALID);
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);
}

void dbus_done(struct userdata *u){
    pa_idxset *sink_list;
    uint32_t dummy;

    DBusMessage *signal = NULL;
    pa_assert_se((signal = dbus_message_new_signal(MANAGER_PATH, MANAGER_IFACE, manager_signals[MANAGER_SIGNAL_SINK_REMOVED].name)));
    dbus_message_append_args(signal, DBUS_TYPE_OBJECT_PATH, &u->dbus_path, DBUS_TYPE_INVALID);
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);

    pa_assert_se(sink_list=pa_shared_get(u->sink->core,SINKLIST));
    pa_idxset_remove_by_data(sink_list,u,&dummy);
    if(pa_idxset_size(sink_list)==0){
        pa_dbus_protocol_unregister_extension(u->dbus_protocol, EXTNAME);
        pa_dbus_protocol_remove_interface(u->dbus_protocol, MANAGER_PATH, manager_info.name);
        pa_shared_remove(u->sink->core, EQDB);
        pa_database_close(u->database);
        pa_shared_remove(u->sink->core, SINKLIST);
        pa_xfree(sink_list);
    }
    pa_dbus_protocol_remove_interface(u->dbus_protocol, u->dbus_path, equalizer_info.name);
    pa_xfree(u->dbus_path);
    pa_dbus_protocol_unref(u->dbus_protocol);
}

void manager_handle_remove_profile(DBusConnection *conn, DBusMessage *msg, void *_u) {
    DBusError error;
    pa_core *c = (pa_core *)_u;
    DBusMessage *signal = NULL;
    pa_dbus_protocol *dbus_protocol;
    char *name;
    pa_assert(conn);
    pa_assert(msg);
    pa_assert(c);
    dbus_error_init(&error);
    if(!dbus_message_get_args(msg, &error,
                 DBUS_TYPE_STRING, &name,
                DBUS_TYPE_INVALID)){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }
    remove_profile(c,name);
    pa_dbus_send_empty_reply(conn, msg);

    pa_assert_se((signal = dbus_message_new_signal(MANAGER_PATH, MANAGER_IFACE, manager_signals[MANAGER_SIGNAL_PROFILES_CHANGED].name)));
    dbus_protocol = pa_dbus_protocol_get(c);
    pa_dbus_protocol_send_signal(dbus_protocol, signal);
    pa_dbus_protocol_unref(dbus_protocol);
    dbus_message_unref(signal);
}

void manager_get_revision(DBusConnection *conn, DBusMessage *msg, void *_u){
    uint32_t rev=1;
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_UINT32, &rev);
}

static void get_sinks(pa_core *u, char ***names, unsigned *n_sinks){
    void *iter = NULL;
    struct userdata *sink_u = NULL;
    uint32_t dummy;
    pa_idxset *sink_list;
    pa_assert(u);
    pa_assert(names);
    pa_assert(n_sinks);

    pa_assert_se(sink_list = pa_shared_get(u, SINKLIST));
    *n_sinks = (unsigned) pa_idxset_size(sink_list);
    *names = *n_sinks > 0 ? pa_xnew0(char *,*n_sinks) : NULL;
    for(uint32_t i = 0; i < *n_sinks; ++i){
        sink_u = (struct userdata *) pa_idxset_iterate(sink_list, &iter, &dummy);
        (*names)[i] = pa_xstrdup(sink_u->dbus_path);
    }
}

void manager_get_sinks(DBusConnection *conn, DBusMessage *msg, void *_u){
    unsigned n;
    char **names = NULL;
    pa_assert(conn);
    pa_assert(msg);
    pa_assert(_u);

    get_sinks((pa_core *) _u, &names, &n);
    pa_dbus_send_basic_array_variant_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, names, n);
    for(unsigned i = 0; i < n; ++i){
        pa_xfree(names[i]);
    }
    pa_xfree(names);
}

static void get_profiles(pa_core *c, char ***names, unsigned *n){
    char *name;
    pa_database *database;
    pa_datum key, next_key;
    pa_strlist *head=NULL, *iter;
    pa_bool_t done;
    pa_assert_se(database = pa_shared_get(c, EQDB));

    pa_assert(c);
    pa_assert(names);
    pa_assert(n);
    done = !pa_database_first(database, &key, NULL);
    *n = 0;
    while(!done){
        done = !pa_database_next(database, &key, &next_key, NULL);
        name=pa_xmalloc(key.size + 1);
        memcpy(name, key.data, key.size);
        name[key.size] = '\0';
        pa_datum_free(&key);
        head = pa_strlist_prepend(head, name);
        pa_xfree(name);
        key = next_key;
        (*n)++;
    }
    (*names) = *n > 0 ? pa_xnew0(char *, *n) : NULL;
    iter=head;
    for(unsigned i = 0; i < *n; ++i){
        (*names)[*n - 1 - i] = pa_xstrdup(pa_strlist_data(iter));
        iter = pa_strlist_next(iter);
    }
    pa_strlist_free(head);
}

void manager_get_profiles(DBusConnection *conn, DBusMessage *msg, void *_u){
    char **names;
    unsigned n;
    pa_assert(conn);
    pa_assert(msg);
    pa_assert(_u);

    get_profiles((pa_core *)_u, &names, &n);
    pa_dbus_send_basic_array_variant_reply(conn, msg, DBUS_TYPE_STRING, names, n);
    for(unsigned i = 0; i < n; ++i){
        pa_xfree(names[i]);
    }
    pa_xfree(names);
}

void manager_get_all(DBusConnection *conn, DBusMessage *msg, void *_u){
    pa_core *c;
    char **names = NULL;
    unsigned n;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter, dict_iter;
    uint32_t rev;
    pa_assert(conn);
    pa_assert(msg);
    pa_assert_se(c = _u);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    rev = 1;
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, manager_handlers[MANAGER_HANDLER_REVISION].property_name, DBUS_TYPE_UINT32, &rev);

    get_sinks(c, &names, &n);
    pa_dbus_append_basic_array_variant_dict_entry(&dict_iter,manager_handlers[MANAGER_HANDLER_EQUALIZED_SINKS].property_name, DBUS_TYPE_OBJECT_PATH, names, n);
    for(unsigned i = 0; i < n; ++i){
        pa_xfree(names[i]);
    }
    pa_xfree(names);

    get_profiles(c, &names, &n);
    pa_dbus_append_basic_array_variant_dict_entry(&dict_iter, manager_handlers[MANAGER_HANDLER_PROFILES].property_name, DBUS_TYPE_STRING, names, n);
    for(unsigned i = 0; i < n; ++i){
        pa_xfree(names[i]);
    }
    pa_xfree(names);
    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));
    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}

void equalizer_handle_seed_filter(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct userdata *u=(struct userdata *) _u;
    DBusError error;
    DBusMessage *signal = NULL;
    float *ys;
    uint32_t *xs;
    double *_ys, preamp;
    unsigned x_npoints, y_npoints, a_i;
    float *H;
    pa_bool_t points_good = TRUE;
    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    dbus_error_init(&error);

    if(!dbus_message_get_args(msg, &error,
                DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &xs, &x_npoints,
                DBUS_TYPE_ARRAY, DBUS_TYPE_DOUBLE, &_ys, &y_npoints,
                DBUS_TYPE_DOUBLE, &preamp,
                DBUS_TYPE_INVALID)){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }
    for(size_t i = 0; i < x_npoints; ++i){
        if(xs[i] >= FILTER_SIZE){
            points_good = FALSE;
            break;
        }
    }
    if(!is_monotonic(xs, x_npoints) || !points_good){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "xs must be monotonic and 0<=x<=%ld", u->fft_size / 2);
        dbus_error_free(&error);
        return;

    }else if(x_npoints != y_npoints || x_npoints < 2 || x_npoints > FILTER_SIZE  ){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "xs and ys must be the same length and 2<=l<=%ld!", FILTER_SIZE);
        dbus_error_free(&error);
        return;
    }else if(xs[0] != 0 || xs[x_npoints - 1] != u->fft_size / 2){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "xs[0] must be 0 and xs[-1]=fft_size/2");
        dbus_error_free(&error);
        return;
    }

    ys = pa_xmalloc(x_npoints * sizeof(float));
    for(uint32_t i = 0; i < x_npoints; ++i){
        ys[i] = (float) _ys[i];
    }
    a_i = pa_aupdate_write_begin(u->a_H);
    H = u->Hs[a_i];
    u->Xs[a_i] = preamp;
    interpolate(H, FILTER_SIZE, xs, ys, x_npoints);
    fix_filter(H, u->fft_size);
    pa_aupdate_write_end(u->a_H);
    pa_xfree(ys);

    //Stupid for IO reasons?  Add a save signal to dbus instead
    //save_state(u);

    pa_dbus_send_empty_reply(conn, msg);

    pa_assert_se((signal = dbus_message_new_signal(u->dbus_path, EQUALIZER_IFACE, equalizer_signals[EQUALIZER_SIGNAL_FILTER_CHANGED].name)));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);
}

void equalizer_handle_get_filter_points(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct userdata *u = (struct userdata *) _u;
    DBusError error;
    uint32_t *xs;
    double *ys, preamp;
    unsigned x_npoints, a_i;
    float *H;
    pa_bool_t points_good=TRUE;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);

    dbus_error_init(&error);

    if(!dbus_message_get_args(msg, &error,
                DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &xs, &x_npoints,
                DBUS_TYPE_INVALID)){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }
    for(size_t i = 0; i < x_npoints; ++i){
        if(xs[i] >= FILTER_SIZE){
            points_good=FALSE;
            break;
        }
    }

    if(x_npoints > FILTER_SIZE || !points_good){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "xs indices/length must be <= %ld!", FILTER_SIZE);
        dbus_error_free(&error);
        return;
    }

    ys = pa_xmalloc(x_npoints * sizeof(double));
    a_i = pa_aupdate_read_begin(u->a_H);
    H = u->Hs[a_i];
    preamp = u->Xs[a_i];
    for(uint32_t i = 0; i < x_npoints; ++i){
        ys[i] = H[xs[i]] * u->fft_size;
    }
    pa_aupdate_read_end(u->a_H);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &msg_iter);

    pa_dbus_append_basic_array(&msg_iter, DBUS_TYPE_DOUBLE, ys, x_npoints);
    pa_dbus_append_basic_variant(&msg_iter, DBUS_TYPE_DOUBLE, &preamp);

    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
    pa_xfree(ys);
}

void equalizer_handle_save_profile(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct userdata *u = (struct userdata *) _u;
    char *name;
    DBusMessage *signal = NULL;
    DBusError error;
    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);
    dbus_error_init(&error);

    if(!dbus_message_get_args(msg, &error,
                 DBUS_TYPE_STRING, &name,
                DBUS_TYPE_INVALID)){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }
    save_profile(u,name);
    pa_dbus_send_empty_reply(conn, msg);

    pa_assert_se((signal = dbus_message_new_signal(MANAGER_PATH, MANAGER_IFACE, manager_signals[MANAGER_SIGNAL_PROFILES_CHANGED].name)));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);
}

void equalizer_handle_load_profile(DBusConnection *conn, DBusMessage *msg, void *_u) {
    struct userdata *u=(struct userdata *) _u;
    char *name;
    DBusError error;
    const char *err_msg = NULL;
    DBusMessage *signal = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(u);
    dbus_error_init(&error);

    if(!dbus_message_get_args(msg, &error,
                 DBUS_TYPE_STRING, &name,
                DBUS_TYPE_INVALID)){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }
    err_msg = load_profile(u, name);
    if(err_msg != NULL){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "error loading profile %s: %s", name, err_msg);
        dbus_error_free(&error);
        return;
    }
    pa_dbus_send_empty_reply(conn, msg);

    pa_assert_se((signal = dbus_message_new_signal(u->dbus_path, EQUALIZER_IFACE, equalizer_signals[EQUALIZER_SIGNAL_FILTER_CHANGED].name)));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);
}

void equalizer_get_revision(DBusConnection *conn, DBusMessage *msg, void *_u){
    uint32_t rev=1;
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_UINT32, &rev);
}

void equalizer_get_n_coefs(DBusConnection *conn, DBusMessage *msg, void *_u){
    struct userdata *u;
    uint32_t n_coefs;
    pa_assert_se(u = (struct userdata *) _u);
    pa_assert(conn);
    pa_assert(msg);

    n_coefs = (uint32_t) PROFILE_SIZE;
    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &n_coefs);
}

void equalizer_get_sample_rate(DBusConnection *conn, DBusMessage *msg, void *_u){
    struct userdata *u;
    uint32_t rate;
    pa_assert_se(u = (struct userdata *) _u);
    pa_assert(conn);
    pa_assert(msg);

    rate = (uint32_t) u->sink->sample_spec.rate;
    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &rate);
}

void equalizer_get_filter_rate(DBusConnection *conn, DBusMessage *msg, void *_u){
    struct userdata *u;
    uint32_t fft_size;
    pa_assert_se(u = (struct userdata *) _u);
    pa_assert(conn);
    pa_assert(msg);

    fft_size = (uint32_t) u->fft_size;
    pa_dbus_send_basic_variant_reply(conn, msg, DBUS_TYPE_UINT32, &fft_size);
}

static double * get_filter(struct userdata *u){
    float *H;
    double *H_;
    unsigned a_i;
    H_ = pa_xnew0(double, PROFILE_SIZE);
    a_i = pa_aupdate_read_begin(u->a_H);
    H = u->Hs[a_i];
    H_[0] = u->Xs[a_i];
    for(size_t i = 0;i < FILTER_SIZE; ++i){
        H_[i + 1] = H[i] * u->fft_size;
    }
    pa_aupdate_read_end(u->a_H);
    return H_;
}

void equalizer_get_filter(DBusConnection *conn, DBusMessage *msg, void *_u){
    struct userdata *u;
    unsigned n_coefs;
    double *H_;
    pa_assert_se(u = (struct userdata *) _u);

    n_coefs = PROFILE_SIZE;
    pa_assert(conn);
    pa_assert(msg);
    H_ = get_filter(u);
    pa_dbus_send_basic_array_variant_reply(conn, msg, DBUS_TYPE_DOUBLE, H_, n_coefs);
    pa_xfree(H_);
}

static void set_filter(struct userdata *u, double *H_){
    unsigned a_i= pa_aupdate_write_begin(u->a_H);
    float *H = u->Hs[a_i];
    u->Xs[a_i] = H_[0];
    for(size_t i = 0; i < FILTER_SIZE; ++i){
        H[i] = (float) H_[i + 1];
    }
    fix_filter(H + 1, u->fft_size);
    pa_aupdate_write_end(u->a_H);
}

void equalizer_set_filter(DBusConnection *conn, DBusMessage *msg, void *_u){
    struct userdata *u;
    double *H;
    unsigned _n_coefs;
    DBusMessage *signal = NULL;
    pa_assert_se(u = (struct userdata *) _u);
    pa_assert(conn);
    pa_assert(msg);

    if(pa_dbus_get_fixed_array_set_property_arg(conn, msg, DBUS_TYPE_DOUBLE, &H, &_n_coefs)){
        return;
    }
    if(_n_coefs != PROFILE_SIZE){
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "This filter takes exactly %ld coefficients, you gave %d", PROFILE_SIZE, _n_coefs);
        return;
    }
    set_filter(u, H);

    pa_dbus_send_empty_reply(conn, msg);

    pa_assert_se((signal = dbus_message_new_signal(u->dbus_path, EQUALIZER_IFACE, equalizer_signals[EQUALIZER_SIGNAL_FILTER_CHANGED].name)));
    pa_dbus_protocol_send_signal(u->dbus_protocol, signal);
    dbus_message_unref(signal);
}

void equalizer_get_all(DBusConnection *conn, DBusMessage *msg, void *_u){
    struct userdata *u;
    DBusMessage *reply = NULL;
    DBusMessageIter msg_iter, dict_iter;
    uint32_t rev, n_coefs, rate, fft_size;
    double *H;
    pa_assert_se(u = (struct userdata *) _u);
    pa_assert(msg);

    rev = 1;
    n_coefs = (uint32_t) PROFILE_SIZE;
    rate = (uint32_t) u->sink->sample_spec.rate;
    fft_size = (uint32_t) u->fft_size;

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &msg_iter);
    pa_assert_se(dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter));

    pa_dbus_append_basic_variant_dict_entry(&dict_iter, equalizer_handlers[EQUALIZER_HANDLER_REVISION].property_name, DBUS_TYPE_UINT32, &rev);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, equalizer_handlers[EQUALIZER_HANDLER_SAMPLERATE].property_name, DBUS_TYPE_UINT32, &rate);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, equalizer_handlers[EQUALIZER_HANDLER_FILTERSAMPLERATE].property_name, DBUS_TYPE_UINT32, &fft_size);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, equalizer_handlers[EQUALIZER_HANDLER_N_COEFS].property_name, DBUS_TYPE_UINT32, &n_coefs);
    H = get_filter(u);
    pa_dbus_append_basic_variant_dict_entry(&dict_iter, equalizer_handlers[EQUALIZER_HANDLER_FILTER].property_name, DBUS_TYPE_UINT32, &H);
    pa_xfree(H);

    pa_assert_se(dbus_message_iter_close_container(&msg_iter, &dict_iter));
    pa_assert_se(dbus_connection_send(conn, reply, NULL));
    dbus_message_unref(reply);
}
