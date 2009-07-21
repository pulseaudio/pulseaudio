/***
This file is part of PulseAudio.

This module is based off Lennart Poettering's LADSPA sink and swaps out
LADSPA functionality for a STFT OLA based digital equalizer.  All new work
is published under Pulseaudio's original license.
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

#include <stdio.h>
#include <math.h>
#include <fftw3.h>
#include <float.h>


#include <pulse/xmalloc.h>
#include <pulse/i18n.h>

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
#include <pulsecore/ltdl-helper.h>

#include <stdint.h>
#include <time.h>


#include "module-equalizer-sink-symdef.h"

PA_MODULE_AUTHOR("Jason Newton");
PA_MODULE_DESCRIPTION(_("General Purpose Equalizer"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(_("sink=<sink to connect to> "));

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)


struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink, *master;
    pa_sink_input *sink_input;

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
    size_t overlap_size;//window_size-R
    size_t samples_gathered;
    size_t max_output;
    size_t target_samples;
    float *H;//frequency response filter (magnitude based)
    float *W;//windowing function (time domain)
    float *work_buffer,**input,**overlap_accum,**output_buffer;
    fftwf_complex *output_window;
    fftwf_plan forward_plan,inverse_plan;
    //size_t samplings;

    pa_memchunk conv_buffer;
    pa_memblockq *rendered_q;
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

uint64_t time_diff(struct timespec *timeA_p, struct timespec *timeB_p);
void hanning_normalized_window(float *W,size_t window_size);
void hanning_window(float *W,size_t window_size);
void hamming_window(float *W,size_t window_size);
void blackman_window(float *W,size_t window_size);
void sin_window(float *W,size_t window_size);
void array_out(const char *name,float *a,size_t length);

static void dsp_logic(float *dst,struct userdata *u);
static void process_samples(struct userdata *u);
void input_buffer(struct userdata *u,pa_memchunk *in);

#define gettime(x) clock_gettime(CLOCK_MONOTONIC,&x)
#define tdiff(x,y) time_diff(&x,&y)

uint64_t time_diff(struct timespec *timeA_p, struct timespec *timeB_p)
{
    return ((timeA_p->tv_sec * 1000000000) + timeA_p->tv_nsec) -
    ((timeB_p->tv_sec * 1000000000) + timeB_p->tv_nsec);
}

void hanning_normalized_window(float *W,size_t window_size){
    //h = sqrt(2)/2 * (1+cos(t*pi)) ./ sqrt( 1+cos(t*pi).^2 )
    float c;
    for(size_t i=0;i<window_size;++i){
        c=cos(M_PI*i/(window_size-1));
        W[i]=sqrt(2.0)/2.0*(1.0+c) / sqrt(1.0+c*c);
    }
}
void hanning_window(float *W,size_t window_size){
    //h=.5*(1-cos(2*pi*j/(window_size+1)), COLA for R=(M+1)/2
    for(size_t i=0;i<window_size;++i){
        W[i]=.5*(1-cos(2*M_PI*i/(window_size+1)));
    }
}
void hamming_window(float *W,size_t window_size){
    //h=.54-.46*cos(2*pi*j/(window_size-1))
    //COLA for R=(M-1)/2,(M-1)/4 etc when endpoints are divided by 2
    //or one endpoint is zeroed
    float m;
    for(size_t i=0;i<window_size;++i){
        m=i;
        m/=(window_size-1);
        W[i]=.54-.46*cos(2*M_PI*m);
    }
    W[window_size-1]=0;
    //W[0]/=2;
    //W[window_size-1]/=2;
}
void blackman_window(float *W,size_t window_size){
    //h=.42-.5*cos(2*pi*m)+.08*cos(4*pi*m), m=(0:W-1)/(W-1)
    //COLA for R=(M-1)/3 when M is odd and R is an integer
    //R=M/3 when M is even and R is an integer
    float m;
    for(size_t i=0;i<window_size;++i){
        m=i;
        m/=(window_size-1);
        W[i]=.42-.5*cos(2*M_PI*m)+.08*cos(4*M_PI*m);
    }
}


void sin_window(float *W,size_t window_size){
    //h = (cos(t*pi)+1)/2 .* float(abs(t)<1);
    for(size_t i=0;i<window_size;++i){
        W[i]=sin(M_PI*i/(window_size-1));
    }
}


void array_out(const char *name,float *a,size_t length){
    FILE *p=fopen(name,"w");
    if(!p){
        pa_log("opening %s failed!",name);
        return;
    }
    for(size_t i=0;i<length;++i){
        fprintf(p,"%e,",a[i]);
        //if(i%1000==0){
        //    fprintf(p,"\n");
        //}
    }
    fprintf(p,"\n");
    fclose(p);
}


/* Called from I/O thread context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t usec = 0;
            pa_sample_spec *ss=&u->sink->sample_spec;
            size_t fs=pa_frame_size(&(u->sink->sample_spec));

            /* Get the latency of the master sink */
            if (PA_MSGOBJECT(u->master)->process_msg(PA_MSGOBJECT(u->master), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
                usec = 0;

            usec+=pa_bytes_to_usec(u->R*fs,ss);
            //usec+=pa_bytes_to_usec(u->samples_gathered*fs,ss);
            usec += pa_bytes_to_usec(pa_memblockq_get_length(u->rendered_q), ss);
            /* Add the latency internal to our sink input on top */
            usec += pa_bytes_to_usec(pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq), &u->master->sample_spec);
            *((pa_usec_t*) data) = usec;
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

    if (PA_SINK_IS_LINKED(state) &&
        u->sink_input &&
        PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))

        pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);

    return 0;
}

/* Called from I/O thread context */
static void sink_request_rewind(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* Just hand this one over to the master sink */
    pa_sink_input_request_rewind(u->sink_input, s->thread_info.rewind_nbytes + pa_memblockq_get_length(u->rendered_q), TRUE, FALSE, FALSE);
}

/* Called from I/O thread context */
static void sink_update_requested_latency(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
            u->sink_input,
            pa_sink_get_requested_latency_within_thread(s));
}

static void process_samples(struct userdata *u){
    pa_memchunk tchunk;
    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    while(u->samples_gathered>=u->R){
        float *dst;
        //pa_log("iter gathered: %ld",u->samples_gathered);
        //pa_memblockq_drop(u->rendered_q, tchunk.length);
        tchunk.index=0;
        tchunk.length=u->R*fs;
        tchunk.memblock=pa_memblock_new(u->core->mempool,tchunk.length);
        dst=((float*)pa_memblock_acquire(tchunk.memblock));
        dsp_logic(dst,u);
        pa_memblock_release(tchunk.memblock);
        pa_memblockq_push(u->rendered_q, &tchunk);
        pa_memblock_unref(tchunk.memblock);
        u->samples_gathered-=u->R;
    }
}

static void dsp_logic(float *dst,struct userdata *u){
    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    //use a linear-phase sliding STFT and overlap-add method (for each channel)
    for (size_t c=0;c<u->channels;c++) {
        //zero padd the data
        memset(u->work_buffer+u->window_size,0,(u->fft_size-u->window_size)*sizeof(float));
        //window the data
        for(size_t j=0;j<u->window_size;++j){
            u->work_buffer[j]=u->W[j]*u->input[c][j];
        }
        //Processing is done here!
        //do fft
        fftwf_execute_dft_r2c(u->forward_plan,u->work_buffer,u->output_window);
        //perform filtering
        for(size_t j=0;j<u->fft_size/2+1;++j){
            u->output_window[j][0]*=u->H[j];
            u->output_window[j][1]*=u->H[j];
        }
        //inverse fft
        fftwf_execute_dft_c2r(u->inverse_plan,u->output_window,u->work_buffer);
        ////debug: tests overlaping add
        ////and negates ALL PREVIOUS processing
        ////yields a perfect reconstruction if COLA is held
        //for(size_t j=0;j<u->window_size;++j){
        //    u->work_buffer[j]=u->W[j]*u->input[c][j];
        //}

        //overlap add and preserve overlap component from this window (linear phase)
        for(size_t j=0;j<u->R;++j){
            u->work_buffer[j]+=u->overlap_accum[c][j];
            u->overlap_accum[c][j]=u->work_buffer[u->overlap_size+j];
        }

        //debug: tests if basic buffering works
        //shouldn't modify the signal AT ALL (beyond roundoff)
        for(size_t j=0;j<u->window_size;++j){
            u->work_buffer[j]=u->input[c][j];
        }

        //preseve the needed input for the next window's overlap
        memmove(u->input[c],u->input[c]+u->R,
            (u->samples_gathered+u->overlap_size-u->R)*sizeof(float)
        );
        //output the samples that are outputable now
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE,dst+c,fs,u->work_buffer,sizeof(float),u->R);
    }
}

void input_buffer(struct userdata *u,pa_memchunk *in){
    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    size_t samples=in->length/fs;
    pa_assert_se(samples<=u->target_samples-u->samples_gathered);
    float *src = (float*) ((uint8_t*) pa_memblock_acquire(in->memblock) + in->index);
    for (size_t c=0;c<u->channels;c++) {
        //buffer with an offset after the overlap from previous
        //iterations
        pa_assert_se(
            u->input[c]+u->overlap_size+u->samples_gathered+samples<=u->input[c]+u->target_samples+u->overlap_size
        );
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE,u->input[c]+u->overlap_size+u->samples_gathered,sizeof(float),src+c,fs,samples);
    }
    u->samples_gathered+=samples;
    pa_memblock_release(in->memblock);
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;
    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = i->userdata);
    pa_assert_se(u->sink);
    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    size_t samples_requested=nbytes/fs;
    size_t buffered_samples=pa_memblockq_get_length(u->rendered_q)/fs;
    pa_memchunk tchunk;
    chunk->memblock=NULL;
    if (!u->sink || !PA_SINK_IS_OPENED(u->sink->thread_info.state))
        return -1;

    pa_log("start output-buffered %ld, input-buffered %ld, requested %ld",buffered_samples,u->samples_gathered,samples_requested);
    struct timespec start,end;

    if(pa_memblockq_peek(u->rendered_q,&tchunk)==0){
        *chunk=tchunk;
        pa_memblockq_drop(u->rendered_q, chunk->length);
        return 0;
    }
    do{
        pa_memchunk *buffer;
        size_t input_remaining=u->target_samples-u->samples_gathered;
        pa_assert(input_remaining>0);
        //collect samples

        //buffer=&u->conv_buffer;
        //buffer->length=input_remaining*fs;
        //buffer->index=0;
        //pa_memblock_ref(buffer);
        //pa_sink_render_into(u->sink,buffer);

        if(u->sink->thread_info.rewind_requested)
            sink_request_rewind(u->sink);

        pa_memchunk p;
        buffer=&p;
        pa_sink_render(u->sink,u->R*fs,buffer);
        buffer->length=PA_MIN(input_remaining*fs,buffer->length);

        //debug block
        //pa_memblockq_push(u->rendered_q,buffer);
        //pa_memblock_unref(buffer->memblock);
        //goto END;

        pa_log("asked for %ld input samples, got %ld samples",input_remaining,buffer->length/fs);
        //copy new input
        gettime(start);
        input_buffer(u,buffer);
        gettime(end);
        pa_log("Took %0.5f seconds to setup",tdiff(end,start)*1e-9);

        pa_memblock_unref(buffer->memblock);

        pa_assert_se(u->fft_size>=u->window_size);
        pa_assert_se(u->R<u->window_size);
        //process every complete block on hand

        gettime(start);
        process_samples(u);
        gettime(end);
        pa_log("Took %0.5f seconds to process",tdiff(end,start)*1e-9);

        buffered_samples=pa_memblockq_get_length(u->rendered_q)/fs;
    }while(buffered_samples<u->R);

    //deque from rendered_q and output
    pa_assert_se(pa_memblockq_peek(u->rendered_q,&tchunk)==0);
    *chunk=tchunk;
    pa_memblockq_drop(u->rendered_q, chunk->length);
    //if(tchunk.length>=nbytes){
        //chunk->length=PA_MIN(tchunk.length,nbytes);
    //}else{
    //    size_t copied=0;
    //    chunk->index=0;
    //    chunk->length=PA_MIN(nbytes,pa_memblockq_get_length(u->rendered_q));
    //    chunk->memblock=pa_memblock_new(u->core->mempool,chunk->length);
    //    uint8_t *dst=(uint8_t*)pa_memblock_acquire(chunk->memblock);
    //    for(;;){
    //        size_t l=PA_MIN(tchunk.length,nbytes-copied);
    //        pa_assert_se(l>0);
    //        uint8_t *src=(((uint8_t*)pa_memblock_acquire(tchunk.memblock))+tchunk.index);
    //        memmove(dst+copied,src,l);
    //        copied+=l;
    //        pa_memblock_release(tchunk.memblock);
    //        pa_memblock_unref(tchunk.memblock);
    //        pa_memblockq_drop(u->rendered_q,l);
    //        if(copied<chunk->length){
    //            pa_assert_se(pa_memblockq_peek(u->rendered_q,&tchunk)==0);
    //        }else{
    //            break;
    //        }
    //    }
    //    pa_memblock_release(chunk->memblock);
    //}
    pa_assert_se(chunk->memblock);
    pa_log("gave %ld",chunk->length/fs);
    //pa_log("end pop");
    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    size_t amount = 0;

    pa_log_debug("Rewind callback!");
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_OPENED(u->sink->thread_info.state))
        return;

    if (u->sink->thread_info.rewind_nbytes > 0) {
        size_t max_rewrite;

        max_rewrite = nbytes + pa_memblockq_get_length(u->rendered_q);
        amount = PA_MIN(u->sink->thread_info.rewind_nbytes, max_rewrite);
        u->sink->thread_info.rewind_nbytes = 0;

        if (amount > 0) {
            //pa_sample_spec *ss=&u->sink->sample_spec;
            pa_memblockq_seek(u->rendered_q, - (int64_t) amount, PA_SEEK_RELATIVE, TRUE);
            pa_log_debug("Resetting equalizer");
            u->samples_gathered=0;
        }
    }

    pa_sink_process_rewind(u->sink, amount);
    pa_memblockq_rewind(u->rendered_q, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_memblockq_set_maxrewind(u->rendered_q, nbytes);
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    pa_sink_set_max_request_within_thread(u->sink, u->R*fs);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    pa_sink_set_latency_range_within_thread(u->sink,u->R*fs ,u->R*fs );
    //pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_detach_within_thread(u->sink);
    pa_sink_set_asyncmsgq(u->sink, NULL);
    pa_sink_set_rtpoll(u->sink, NULL);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_set_asyncmsgq(u->sink, i->sink->asyncmsgq);
    pa_sink_set_rtpoll(u->sink, i->sink->rtpoll);
    pa_sink_attach_within_thread(u->sink);

    size_t fs=pa_frame_size(&(u->sink->sample_spec));
    pa_sink_set_latency_range_within_thread(u->sink, u->R*fs, u->R*fs);
    //pa_sink_set_latency_range_within_thread(u->sink, u->master->thread_info.min_latency, u->master->thread_info.max_latency);
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_unlink(u->sink);
    pa_sink_input_unlink(u->sink_input);

    pa_sink_unref(u->sink);
    u->sink = NULL;
    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

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

/* Called from main context */
static pa_bool_t sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    return u->sink != dest;
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
    fs=pa_frame_size(&ss);

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->master = master;
    u->sink = NULL;
    u->sink_input = NULL;

    u->channels=ss.channels;
    u->fft_size=pow(2,ceil(log(ss.rate)/log(2)));
    pa_log("fft size: %ld",u->fft_size);
    u->window_size=7999;
    u->R=(u->window_size+1)/2;
    u->overlap_size=u->window_size-u->R;
    u->target_samples=1*u->R;
    u->samples_gathered=0;
    u->max_output=pa_frame_align(pa_mempool_block_size_max(m->core->mempool), &ss)/pa_frame_size(&ss);
    u->rendered_q = pa_memblockq_new(0, MEMBLOCKQ_MAXLENGTH,u->target_samples*fs, fs, fs, 0, 0, NULL);
    u->conv_buffer.memblock=pa_memblock_new(u->core->mempool,u->target_samples*fs);


    u->H=(float*) fftwf_malloc((u->fft_size/2+1)*sizeof(float));
    u->W=(float*) fftwf_malloc((u->window_size)*sizeof(float));
    u->work_buffer=(float*) fftwf_malloc(u->fft_size*sizeof(float));
    u->input=(float **)malloc(sizeof(float *)*u->channels);
    u->overlap_accum=(float **)malloc(sizeof(float *)*u->channels);
    u->output_buffer=(float **)malloc(sizeof(float *)*u->channels);
    for(size_t c=0;c<u->channels;++c){
        u->input[c]=(float*) fftwf_malloc((u->target_samples+u->overlap_size)*sizeof(float));
        pa_assert_se(u->input[c]);
        memset(u->input[c],0,(u->target_samples+u->overlap_size)*sizeof(float));
        pa_assert_se(u->input[c]);
        u->overlap_accum[c]=(float*) fftwf_malloc(u->R*sizeof(float));
        pa_assert_se(u->overlap_accum[c]);
        memset(u->overlap_accum[c],0,u->R*sizeof(float));
        u->output_buffer[c]=(float*) fftwf_malloc(u->window_size*sizeof(float));
        pa_assert_se(u->output_buffer[c]);
    }
    u->output_window = (fftwf_complex *) fftwf_malloc(sizeof(fftwf_complex) * (u->fft_size/2+1));
    u->forward_plan=fftwf_plan_dft_r2c_1d(u->fft_size, u->work_buffer, u->output_window, FFTW_ESTIMATE);
    u->inverse_plan=fftwf_plan_dft_c2r_1d(u->fft_size, u->output_window, u->work_buffer, FFTW_ESTIMATE);
    /*
    for(size_t j=0;j<u->window_size;++j){
        u->W[j]=.5;
    }
    */
    hanning_window(u->W,u->window_size);

    const int freqs[]={0,25,50,100,200,300,400,800,1500,
        2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,
        13000,14000,15000,16000,17000,18000,19000,20000,21000,22000,23000,24000,INT_MAX};
    const float coefficients[]={1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    const size_t ncoefficients=sizeof(coefficients)/sizeof(float);
    pa_assert_se(sizeof(freqs)/sizeof(int)==sizeof(coefficients)/sizeof(float));
    float *freq_translated=(float *) malloc(sizeof(float)*(ncoefficients));
    freq_translated[0]=1;
    //Translate the frequencies in their natural sampling rate to the new sampling rate frequencies
    for(size_t i=1;i<ncoefficients-1;++i){
        freq_translated[i]=((float)freqs[i]*u->fft_size)/ss.rate;
        //pa_log("i: %ld: %d , %g",i,freqs[i],freq_translated[i]);
        pa_assert_se(freq_translated[i]>=freq_translated[i-1]);
    }
    freq_translated[ncoefficients-1]=FLT_MAX;
    //Interpolate the specified frequency band values
    u->H[0]=1;
    for(size_t i=1,j=0;i<(u->fft_size/2+1);++i){
        pa_assert_se(j<ncoefficients);
        //max frequency range passed, consider the rest as one band
        if(freq_translated[j+1]>=FLT_MAX){
            for(;i<(u->fft_size/2+1);++i){
                u->H[i]=coefficients[j];
            }
            break;
        }
        //pa_log("i: %d, j: %d, freq: %f",i,j,freq_translated[j]);
        //pa_log("interp: %0.4f %0.4f",freq_translated[j],freq_translated[j+1]);
        pa_assert_se(freq_translated[j]<freq_translated[j+1]);
        pa_assert_se(i>=freq_translated[j]);
        pa_assert_se(i<=freq_translated[j+1]);
        //bilinear-inerpolation of coefficients specified
        float c0=(i-freq_translated[j])/(freq_translated[j+1]-freq_translated[j]);
        pa_assert_se(c0>=0&&c0<=1.0);
        u->H[i]=((1.0f-c0)*coefficients[j]+c0*coefficients[j+1]);
        pa_assert_se(u->H[i]>0);
        while(i>=floor(freq_translated[j+1])){
            j++;
        }
    }
    //divide out the fft gain
    for(size_t i=0;i<(u->fft_size/2+1);++i){
        u->H[i]/=u->fft_size;
    }
    free(freq_translated);

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    if (!(sink_data.name = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL))))
        sink_data.name = pa_sprintf_malloc("%s.equalizer", master->name);
    sink_data.namereg_fail = FALSE;
    pa_sink_new_data_set_sample_spec(&sink_data, &ss);
    pa_sink_new_data_set_channel_map(&sink_data, &map);
    z = pa_proplist_gets(master->proplist, PA_PROP_DEVICE_DESCRIPTION);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "FFT based equalizer");
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    u->sink = pa_sink_new(m->core, &sink_data, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY);
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state = sink_set_state;
    u->sink->update_requested_latency = sink_update_requested_latency;
    u->sink->request_rewind = sink_request_rewind;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);
    pa_sink_set_rtpoll(u->sink, master->rtpoll);
    pa_sink_set_max_request(u->sink,u->R*fs);
    //pa_sink_set_fixed_latency(u->sink,pa_bytes_to_usec(u->R*fs,&ss));

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    sink_input_data.sink = u->master;
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Equalized Stream");
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map);

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data, PA_SINK_INPUT_DONT_MOVE);
    pa_sink_input_new_data_done(&sink_input_data);

    if (!u->sink_input)
        goto fail;

    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->state_change = sink_input_state_change_cb;
    u->sink_input->may_move_to = sink_input_may_move_to_cb;
    u->sink_input->userdata = u;

    pa_sink_put(u->sink);
    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);

    pa_xfree(use_default);

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

    if (u->sink) {
        pa_sink_unlink(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->sink_input) {
        pa_sink_input_unlink(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }

    if(u->conv_buffer.memblock)
        pa_memblock_unref(u->conv_buffer.memblock);

    if (u->rendered_q)
        pa_memblockq_free(u->rendered_q);

    fftwf_destroy_plan(u->inverse_plan);
    fftwf_destroy_plan(u->forward_plan);
    fftwf_free(u->output_window);
    for(size_t c=0;c<u->channels;++c){
        fftwf_free(u->output_buffer[c]);
        fftwf_free(u->overlap_accum[c]);
        fftwf_free(u->input[c]);
    }
    free(u->output_buffer);
    free(u->overlap_accum);
    free(u->input);
    fftwf_free(u->work_buffer);
    fftwf_free(u->W);
    fftwf_free(u->H);

    pa_xfree(u);
}
