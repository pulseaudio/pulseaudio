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
    size_t samples_gathered;
    size_t n_buffered_output;
    size_t max_output;
    float *H;//frequency response filter (magnitude based)
    float *W;//windowing function (time domain)
    float *work_buffer,**input,**overlap_accum,**output_buffer;
    fftwf_complex *output_window;
    fftwf_plan forward_plan,inverse_plan;
    //size_t samplings;

    pa_memblockq *memblockq;
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

            /* Get the latency of the master sink */
            if (PA_MSGOBJECT(u->master)->process_msg(PA_MSGOBJECT(u->master), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
                usec = 0;

            usec+=pa_bytes_to_usec(u->n_buffered_output*pa_frame_size(ss),ss);
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
    pa_sink_input_request_rewind(u->sink_input, s->thread_info.rewind_nbytes + pa_memblockq_get_length(u->memblockq), TRUE, FALSE, FALSE);
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

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes, pa_memchunk *chunk) {
    struct userdata *u;
    float *src, *dst;
    pa_memchunk tchunk;
    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = i->userdata);
    size_t fs = pa_frame_size(&u->sink->sample_spec);
    size_t ss=pa_sample_size(&u->sink->sample_spec);
    size_t fe = fs/ss;

    if (!u->sink || !PA_SINK_IS_OPENED(u->sink->thread_info.state))
        return -1;

    //output any buffered outputs first
    if(u->n_buffered_output>0){
        //pa_log("outputing %ld buffered samples",u->n_buffered_output);
        chunk->index = 0;
        size_t n_outputable=PA_MIN(u->n_buffered_output,u->max_output);
        chunk->length = n_outputable*fs;
        chunk->memblock = pa_memblock_new(i->sink->core->mempool, chunk->length);
        pa_memblockq_drop(u->memblockq, chunk->length);
        dst = (float*) pa_memblock_acquire(chunk->memblock);
        for(size_t j=0;j<u->channels;++j){
            pa_sample_clamp(PA_SAMPLE_FLOAT32NE, dst+j, fs, u->output_buffer[j], sizeof(float),n_outputable);
            memmove(u->output_buffer[j],u->output_buffer[j]+n_outputable,(u->n_buffered_output-n_outputable)*sizeof(float));
        }
        u->n_buffered_output-=n_outputable;
        pa_memblock_release(chunk->memblock);
        return 0;
    }
    pa_assert_se(u->n_buffered_output==0);

    //collect the minimum number of samples
    //TODO figure out a better way of buffering the needed
    //number of samples, this doesn't seem to work correctly
    while(u->samples_gathered < u->R){
        //render some new fragments to our memblockq
        size_t desired_samples=PA_MIN(u->R-u->samples_gathered,u->max_output);
        while (pa_memblockq_peek(u->memblockq, &tchunk) < 0) {
            pa_memchunk nchunk;

            pa_sink_render(u->sink, desired_samples*fs, &nchunk);
            pa_memblockq_push(u->memblockq, &nchunk);
            pa_memblock_unref(nchunk.memblock);
        }
        if(tchunk.length/fs!=desired_samples){
            pa_log("got %ld samples, asked for %ld",tchunk.length/fs,desired_samples);
        }
        size_t n_samples=PA_MIN(tchunk.length/fs,u->R-u->samples_gathered);
        //TODO: figure out what to do with rest of the samples when there's too many (rare?)
        src = (float*) ((uint8_t*) pa_memblock_acquire(tchunk.memblock) + tchunk.index);
        for (size_t c=0;c<u->channels;c++) {
            pa_sample_clamp(PA_SAMPLE_FLOAT32NE,u->input[c]+(u->window_size-u->R)+u->samples_gathered,sizeof(float), src+c, fs, n_samples);
        }
        u->samples_gathered+=n_samples;
        pa_memblock_release(tchunk.memblock);
        pa_memblock_unref(tchunk.memblock);
    }
    //IT should be this guy if we're buffering like how its supposed to
    //size_t n_outputable=PA_MIN(u->window_size-u->R,u->max_output);
    //This one takes into account the actual data gathered but then the dsp
    //stuff is wrong when the buffer "underruns"
    size_t n_outputable=PA_MIN(u->R,u->max_output);


    chunk->index=0;
    chunk->length=n_outputable*fs;
    chunk->memblock = pa_memblock_new(i->sink->core->mempool, chunk->length);
    pa_memblockq_drop(u->memblockq, chunk->length);
    dst = (float*) pa_memblock_acquire(chunk->memblock);

    pa_assert_se(u->fft_size>=u->window_size);
    pa_assert_se(u->R<u->window_size);
    pa_assert_se(u->samples_gathered>=u->R);
    size_t sample_rem=u->R-n_outputable;
    //use a linear-phase sliding STFT and overlap-add method (for each channel)
    for (size_t c=0;c<u->channels;c++) {
        ////zero padd the data
        //memset(u->work_buffer,0,u->fft_size*sizeof(float));
        memset(u->work_buffer+u->window_size,0,(u->fft_size-u->window_size)*sizeof(float));
        ////window the data
        for(size_t j=0;j<u->window_size;++j){
            u->work_buffer[j]=u->W[j]*u->input[c][j];
        }
        //Processing is done here!
        //do fft
        //char fname[1024];
        //if(u->samplings==200){
        //    pa_assert_se(0);
        //}

        //this iterations input
        //sprintf(fname,"/home/jason/input%ld-%ld.txt",u->samplings+1,c);
        //array_out(fname,u->input[c]+(u->window_size-u->R),u->R);

        fftwf_execute_dft_r2c(u->forward_plan,u->work_buffer,u->output_window);
        //perform filtering
        for(size_t j=0;j<u->fft_size/2+1;++j){
            u->output_window[j][0]*=u->H[j];
            u->output_window[j][1]*=u->H[j];
        }
        ////inverse fft
        fftwf_execute_dft_c2r(u->inverse_plan,u->output_window,u->work_buffer);
        //the output for the previous iteration's input
        //sprintf(fname,"/home/jason/output%ld-%ld.txt",u->samplings,c);
        //array_out(fname,u->work_buffer,u->window_size);


        ////debug: tests overlaping add
        ////and negates ALL PREVIOUS processing
        ////yields a perfect reconstruction if COLA is held
        //for(size_t j=0;j<u->window_size;++j){
        //    u->work_buffer[j]=u->W[j]*u->input[c][j];
        //}

        //overlap add and preserve overlap component from this window (linear phase)
        for(size_t j=0;j<u->R;++j){
            u->work_buffer[j]+=u->overlap_accum[c][j];
            u->overlap_accum[c][j]=u->work_buffer[u->window_size-u->R+j];
        }


        /*
        //debug: tests if basic buffering works
        //shouldn't modify the signal AT ALL
        for(size_t j=0;j<u->window_size;++j){
            u->work_buffer[j]=u->input[c][j];
        }
        */

        //preseve the needed input for the next windows overlap
        memmove(u->input[c],u->input[c]+u->R,
            (u->window_size-u->R)*sizeof(float)
        );
        //output the samples that are outputable now
        pa_sample_clamp(PA_SAMPLE_FLOAT32NE, dst+c, fs, u->work_buffer, sizeof(float),n_outputable);
        //buffer the rest of them
        memcpy(u->output_buffer[c]+u->n_buffered_output,u->work_buffer+n_outputable,sample_rem*sizeof(float));

    }
    //u->samplings++;
    u->n_buffered_output+=sample_rem;
    u->samples_gathered=0;
    pa_memblock_release(chunk->memblock);
    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    size_t amount = 0;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_OPENED(u->sink->thread_info.state))
        return;

    if (u->sink->thread_info.rewind_nbytes > 0) {
        size_t max_rewrite;

        max_rewrite = nbytes + pa_memblockq_get_length(u->memblockq);
        amount = PA_MIN(u->sink->thread_info.rewind_nbytes, max_rewrite);
        u->sink->thread_info.rewind_nbytes = 0;

        if (amount > 0) {
            pa_memblockq_seek(u->memblockq, - (int64_t) amount, PA_SEEK_RELATIVE, TRUE);
            pa_log_debug("Resetting equalizer");
            u->n_buffered_output=0;
            u->samples_gathered=0;
        }
    }

    pa_sink_process_rewind(u->sink, amount);
    pa_memblockq_rewind(u->memblockq, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_memblockq_set_maxrewind(u->memblockq, nbytes);
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_set_max_request_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
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

    pa_sink_set_latency_range_within_thread(u->sink, u->master->thread_info.min_latency, u->master->thread_info.max_latency);
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
    u->memblockq = pa_memblockq_new(0, MEMBLOCKQ_MAXLENGTH, 0, fs, 1, 1, 0, NULL);

    //u->samplings=0;
    u->channels=ss.channels;
    u->fft_size=pow(2,ceil(log(ss.rate)/log(2)));
    pa_log("fft size: %ld",u->fft_size);
    u->window_size=7999;
    u->R=(u->window_size+1)/2;
    u->samples_gathered=0;
    u->n_buffered_output=0;
    u->max_output=pa_frame_align(pa_mempool_block_size_max(m->core->mempool), &ss)/pa_frame_size(&ss);
    u->H=(float*) fftwf_malloc((u->fft_size/2+1)*sizeof(float));
    u->W=(float*) fftwf_malloc((u->window_size)*sizeof(float));
    u->work_buffer=(float*) fftwf_malloc(u->fft_size*sizeof(float));
    u->input=(float **)malloc(sizeof(float *)*u->channels);
    u->overlap_accum=(float **)malloc(sizeof(float *)*u->channels);
    u->output_buffer=(float **)malloc(sizeof(float *)*u->channels);
    for(size_t c=0;c<u->channels;++c){
        u->input[c]=(float*) fftwf_malloc(u->window_size*sizeof(float));
        memset(u->input[c],0,u->window_size*sizeof(float));
        u->overlap_accum[c]=(float*) fftwf_malloc(u->R*sizeof(float));
        memset(u->overlap_accum[c],0,u->R*sizeof(float));
        u->output_buffer[c]=(float*) fftwf_malloc(u->window_size*sizeof(float));
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
    const float coefficients[]={.1,.1,.1,.1,.1,.1,.1,.1,.1,.1,
        .1,.1,.1,.1,.1,.1,.1,.1,
        .1,.1,.1,.1,.1,.1,.1,.1,.1,.1,.1,.1,.1,.1,.1};
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

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

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
