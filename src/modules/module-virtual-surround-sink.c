/***
    This file is part of PulseAudio.

    Copyright 2010 Intel Corporation
    Contributor: Pierre-Louis Bossart <pierre-louis.bossart@intel.com>
    Copyright 2012 Niels Ole Salscheider <niels_ole@salscheider-online.de>
    Contributor: Alexander E. Patrakov <patrakov@gmail.com>
    Copyright 2020 Christopher Snowhill <kode54@gmail.com>

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>

#include <fftw3.h>

#include <pulse/gccmacro.h>
#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/sound-file.h>
#include <pulsecore/resampler.h>


PA_MODULE_AUTHOR("Christopher Snowhill");
PA_MODULE_DESCRIPTION(_("Virtual surround sink"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        _("sink_name=<name for the sink> "
          "sink_properties=<properties for the sink> "
          "master=<name of sink to filter> "
          "sink_master=<name of sink to filter> "
          "format=<sample format> "
          "rate=<sample rate> "
          "channels=<number of channels> "
          "channel_map=<channel map> "
          "use_volume_sharing=<yes or no> "
          "force_flat_volume=<yes or no> "
          "hrir=/path/to/left_hrir.wav "
          "hrir_left=/path/to/left_hrir.wav "
          "hrir_right=/path/to/optional/right_hrir.wav "
          "autoloaded=<set if this module is being loaded automatically> "
        ));

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)
#define DEFAULT_AUTOLOADED false

struct userdata {
    pa_module *module;

    bool autoloaded;

    pa_sink *sink;
    pa_sink_input *sink_input;

    pa_memblockq *memblockq_sink;

    bool auto_desc;

    size_t fftlen;
    size_t hrir_samples;
    size_t inputs;

    fftwf_plan *p_fw, p_bw;
    fftwf_complex *f_in, *f_out, **f_ir;
    float *revspace, *outspace[2], **inspace;
};

#define BLOCK_SIZE (512)

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "master",  /* Will be deprecated. */
    "sink_master",
    "format",
    "rate",
    "channels",
    "channel_map",
    "use_volume_sharing",
    "force_flat_volume",
    "autoloaded",
    "hrir",
    "hrir_left",
    "hrir_right",
    NULL
};

/* Vector size of 4 floats */
#define v_size 4
static void * alloc(size_t x, size_t s) {
    size_t f;
    float *t;

    f = PA_ROUND_UP(x*s, sizeof(float)*v_size);
    pa_assert_se(t = fftwf_malloc(f));
    pa_memzero(t, f);

    return t;
}

static size_t sink_input_samples(size_t nbytes)
{
    return nbytes / 8;
}

static size_t sink_input_bytes(size_t nsamples)
{
    return nsamples * 8;
}

static size_t sink_samples(const struct userdata *u, size_t nbytes)
{
    return nbytes / (u->inputs * 4);
}

static size_t sink_bytes(const struct userdata *u, size_t nsamples)
{
    return nsamples * (u->inputs * 4);
}

/* Mirror channels for symmetrical impulse */
static pa_channel_position_t mirror_channel(pa_channel_position_t channel) {
    switch (channel) {
        case PA_CHANNEL_POSITION_FRONT_LEFT:
            return PA_CHANNEL_POSITION_FRONT_RIGHT;

        case PA_CHANNEL_POSITION_FRONT_RIGHT:
            return PA_CHANNEL_POSITION_FRONT_LEFT;

        case PA_CHANNEL_POSITION_REAR_LEFT:
            return PA_CHANNEL_POSITION_REAR_RIGHT;

        case PA_CHANNEL_POSITION_REAR_RIGHT:
            return PA_CHANNEL_POSITION_REAR_LEFT;

        case PA_CHANNEL_POSITION_SIDE_LEFT:
            return PA_CHANNEL_POSITION_SIDE_RIGHT;

        case PA_CHANNEL_POSITION_SIDE_RIGHT:
            return PA_CHANNEL_POSITION_SIDE_LEFT;

        case PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER:
            return PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;

        case PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER:
            return PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;

        case PA_CHANNEL_POSITION_TOP_FRONT_LEFT:
            return PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;

        case PA_CHANNEL_POSITION_TOP_FRONT_RIGHT:
            return PA_CHANNEL_POSITION_TOP_FRONT_LEFT;

        case PA_CHANNEL_POSITION_TOP_REAR_LEFT:
            return PA_CHANNEL_POSITION_TOP_REAR_RIGHT;

        case PA_CHANNEL_POSITION_TOP_REAR_RIGHT:
            return PA_CHANNEL_POSITION_TOP_REAR_LEFT;

        default:
            return channel;
    }
}

/* Normalize the hrir */
static void normalize_hrir(float * hrir_data, unsigned hrir_samples, unsigned hrir_channels) {
    /* normalize hrir to avoid audible clipping
     *
     * The following heuristic tries to avoid audible clipping. It cannot avoid
     * clipping in the worst case though, because the scaling factor would
     * become too large resulting in a too quiet signal.
     * The idea of the heuristic is to avoid clipping when a single click is
     * played back on all channels. The scaling factor describes the additional
     * factor that is necessary to avoid clipping for "normal" signals.
     *
     * This algorithm doesn't pretend to be perfect, it's just something that
     * appears to work (not too quiet, no audible clipping) on the material that
     * it has been tested on. If you find a real-world example where this
     * algorithm results in audible clipping, please write a patch that adjusts
     * the scaling factor constants or improves the algorithm (or if you can't
     * write a patch, at least report the problem to the PulseAudio mailing list
     * or bug tracker). */

    const float scaling_factor = 2.5;

    float hrir_sum, hrir_max;
    unsigned i, j;

    hrir_max = 0;
    for (i = 0; i < hrir_samples; i++) {
        hrir_sum = 0;
        for (j = 0; j < hrir_channels; j++)
            hrir_sum += fabs(hrir_data[i * hrir_channels + j]);

        if (hrir_sum > hrir_max)
            hrir_max = hrir_sum;
    }

    for (i = 0; i < hrir_samples; i++) {
        for (j = 0; j < hrir_channels; j++)
            hrir_data[i * hrir_channels + j] /= hrir_max * scaling_factor;
    }
}

/* Normalize a stereo hrir */
static void normalize_hrir_stereo(float * hrir_data, float * hrir_right_data, unsigned hrir_samples, unsigned hrir_channels) {
    const float scaling_factor = 2.5;

    float hrir_sum, hrir_max;
    unsigned i, j;

    hrir_max = 0;
    for (i = 0; i < hrir_samples; i++) {
        hrir_sum = 0;
        for (j = 0; j < hrir_channels; j++) {
            hrir_sum += fabs(hrir_data[i * hrir_channels + j]);
            hrir_sum += fabs(hrir_right_data[i * hrir_channels + j]);
        }

        if (hrir_sum > hrir_max)
            hrir_max = hrir_sum;
    }

    for (i = 0; i < hrir_samples; i++) {
        for (j = 0; j < hrir_channels; j++) {
            hrir_data[i * hrir_channels + j] /= hrir_max * scaling_factor;
            hrir_right_data[i * hrir_channels + j] /= hrir_max * scaling_factor;
        }
    }
}

/* Called from I/O thread context */
static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY:

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
                pa_sink_get_latency_within_thread(u->sink_input->sink, true) +

                /* Add the latency internal to our sink input on top */
                pa_bytes_to_usec(pa_memblockq_get_length(u->sink_input->thread_info.render_memblockq), &u->sink_input->sink->sample_spec);

            /* Add resampler latency */
            *((int64_t*) data) += pa_resampler_get_delay_usec(u->sink_input->thread_info.resampler);

            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state_in_main_thread_cb(pa_sink *s, pa_sink_state_t state, pa_suspend_cause_t suspend_cause) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->state))
        return 0;

    pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);
    return 0;
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    /* When set to running or idle for the first time, request a rewind
     * of the master sink to make sure we are heard immediately */
    if (PA_SINK_IS_OPENED(new_state) && s->thread_info.state == PA_SINK_INIT) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(u->sink_input, 0, false, true, true);
    }

    return 0;
}

/* Called from I/O thread context */
static void sink_request_rewind_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes_sink, nbytes_input;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(u->sink->thread_info.state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->thread_info.state))
        return;

    nbytes_sink = s->thread_info.rewind_nbytes + pa_memblockq_get_length(u->memblockq_sink);
    nbytes_input = sink_input_bytes(sink_samples(u, nbytes_sink));

    /* Just hand this one over to the master sink */
    pa_sink_input_request_rewind(u->sink_input, nbytes_input, true, false, false);
}

/* Called from I/O thread context */
static void sink_update_requested_latency_cb(pa_sink *s) {
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

/* Called from main context */
static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(s->state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->state))
        return;

    pa_sink_input_set_volume(u->sink_input, &s->real_volume, s->save_volume, true);
}

/* Called from main context */
static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (!PA_SINK_IS_LINKED(s->state) ||
        !PA_SINK_INPUT_IS_LINKED(u->sink_input->state))
        return;

    pa_sink_input_set_mute(u->sink_input, s->muted, s->save_muted);
}

static size_t memblockq_missing(pa_memblockq *bq) {
    size_t l, tlength;
    pa_assert(bq);

    tlength = pa_memblockq_get_tlength(bq);
    if ((l = pa_memblockq_get_length(bq)) >= tlength)
        return 0;

    l = tlength - l;
    return l >= pa_memblockq_get_minreq(bq) ? l : 0;
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t nbytes_input, pa_memchunk *chunk) {
    struct userdata *u;
    float *src, *dst;
    int c, ear;
    size_t s, bytes_missing, fftlen;
    pa_memchunk tchunk;
    float fftlen_if, *revspace;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert_se(u = i->userdata);

    /* Hmm, process any rewind request that might be queued up */
    pa_sink_process_rewind(u->sink, 0);

    while ((bytes_missing = memblockq_missing(u->memblockq_sink)) != 0) {
        pa_memchunk nchunk;

        pa_sink_render(u->sink, bytes_missing, &nchunk);
        pa_memblockq_push(u->memblockq_sink, &nchunk);
        pa_memblock_unref(nchunk.memblock);
    }

    pa_memblockq_rewind(u->memblockq_sink, sink_bytes(u, u->fftlen - BLOCK_SIZE));
    pa_memblockq_peek_fixed_size(u->memblockq_sink, sink_bytes(u, u->fftlen), &tchunk);

    pa_memblockq_drop(u->memblockq_sink, tchunk.length);

    /* Now tchunk contains enough data to perform the FFT
     * This should be equal to u->fftlen */

    chunk->index = 0;
    chunk->length = sink_input_bytes(BLOCK_SIZE);
    chunk->memblock = pa_memblock_new(i->sink->core->mempool, chunk->length);

    src = pa_memblock_acquire_chunk(&tchunk);

    for (c = 0; c < u->inputs; c++) {
        for (s = 0, fftlen = u->fftlen; s < fftlen; s++) {
            u->inspace[c][s] = src[s * u->inputs + c];
        }
    }

    pa_memblock_release(tchunk.memblock);
    pa_memblock_unref(tchunk.memblock);

    fftlen_if = 1.0f / (float)u->fftlen;
    revspace = u->revspace + u->fftlen - BLOCK_SIZE;

    pa_memzero(u->outspace[0], BLOCK_SIZE * 4);
    pa_memzero(u->outspace[1], BLOCK_SIZE * 4);

    for (c = 0; c < u->inputs; c++) {
        fftwf_complex *f_in = u->f_in;
        fftwf_complex *f_out = u->f_out;

        fftwf_execute(u->p_fw[c]);

        for (ear = 0; ear < 2; ear++) {
            fftwf_complex *f_ir = u->f_ir[c * 2 + ear];
            float *outspace = u->outspace[ear];

            for (s = 0, fftlen = u->fftlen / 2 + 1; s < fftlen; s++) {
                float re = f_ir[s][0] * f_in[s][0] - f_ir[s][1] * f_in[s][1];
                float im = f_ir[s][1] * f_in[s][0] + f_ir[s][0] * f_in[s][1];
                f_out[s][0] = re;
                f_out[s][1] = im;
            }

            fftwf_execute(u->p_bw);

            for (s = 0, fftlen = BLOCK_SIZE; s < fftlen; ++s)
                outspace[s] += revspace[s] * fftlen_if;
        }
    }

    dst = pa_memblock_acquire_chunk(chunk);

    for (s = 0, fftlen = BLOCK_SIZE; s < fftlen; s++) {
        float output;
        float *outspace = u->outspace[0];

        output = outspace[s];
        if (output < -1.0) output = -1.0;
        if (output > 1.0) output = 1.0;
        dst[s * 2 + 0] = output;

        outspace = u->outspace[1];

        output = outspace[s];
        if (output < -1.0) output = -1.0;
        if (output > 1.0) output = 1.0;
        dst[s * 2 + 1] = output;
    }

    pa_memblock_release(chunk->memblock);

    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes_input) {
    struct userdata *u;
    size_t amount = 0;
    size_t nbytes_sink;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    nbytes_sink = sink_bytes(u, sink_input_samples(nbytes_input));

    if (u->sink->thread_info.rewind_nbytes > 0) {
        size_t max_rewrite;

        max_rewrite = nbytes_sink + pa_memblockq_get_length(u->memblockq_sink);
        amount = PA_MIN(u->sink->thread_info.rewind_nbytes, max_rewrite);
        u->sink->thread_info.rewind_nbytes = 0;

        if (amount > 0) {
            pa_memblockq_seek(u->memblockq_sink, - (int64_t) amount, PA_SEEK_RELATIVE, true);
        }
    }

    pa_sink_process_rewind(u->sink, amount);

    pa_memblockq_rewind(u->memblockq_sink, nbytes_sink);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes_input) {
    struct userdata *u;
    size_t nbytes_sink, nbytes_memblockq;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    nbytes_sink = sink_bytes(u, sink_input_samples(nbytes_input));
    nbytes_memblockq = sink_bytes(u, sink_input_samples(nbytes_input) + u->fftlen);

    /* FIXME: Too small max_rewind:
     * https://bugs.freedesktop.org/show_bug.cgi?id=53709 */
    pa_memblockq_set_maxrewind(u->memblockq_sink, nbytes_memblockq);
    pa_sink_set_max_rewind_within_thread(u->sink, nbytes_sink);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes_input) {
    struct userdata *u;

    size_t nbytes_sink;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    nbytes_sink = sink_bytes(u, sink_input_samples(nbytes_input));

    nbytes_sink = PA_ROUND_UP(nbytes_sink, sink_bytes(u, BLOCK_SIZE));
    pa_sink_set_max_request_within_thread(u->sink, nbytes_sink);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

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

    if (PA_SINK_IS_LINKED(u->sink->thread_info.state))
        pa_sink_detach_within_thread(u->sink);

    pa_sink_set_rtpoll(u->sink, NULL);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;
    size_t max_request;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_set_rtpoll(u->sink, i->sink->thread_info.rtpoll);
    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);

    max_request = sink_bytes(u, sink_input_samples(pa_sink_input_get_max_request(i)));
    max_request = PA_ROUND_UP(max_request, sink_bytes(u, BLOCK_SIZE));
    pa_sink_set_max_request_within_thread(u->sink, max_request);

    /* FIXME: Too small max_rewind:
     * https://bugs.freedesktop.org/show_bug.cgi?id=53709 */
    pa_sink_set_max_rewind_within_thread(u->sink, sink_bytes(u, sink_input_samples(pa_sink_input_get_max_rewind(i))));

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
    pa_sink_input_cork(u->sink_input, true);
    pa_sink_input_unlink(u->sink_input);
    pa_sink_unlink(u->sink);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_sink_unref(u->sink);
    u->sink = NULL;

    pa_module_unload_request(u->module, true);
}

/* Called from main context */
static bool sink_input_may_move_to_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (u->autoloaded)
        return false;

    return u->sink != dest;
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (dest) {
        pa_sink_set_asyncmsgq(u->sink, dest->asyncmsgq);
        pa_sink_update_flags(u->sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, dest->flags);
    } else
        pa_sink_set_asyncmsgq(u->sink, NULL);

    if (u->auto_desc && dest) {
        const char *z;
        pa_proplist *pl;

        pl = pa_proplist_new();
        z = pa_proplist_gets(dest->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(pl, PA_PROP_DEVICE_DESCRIPTION, "Virtual Surround Sink %s on %s",
                         pa_proplist_gets(u->sink->proplist, "device.vsurroundsink.name"), z ? z : dest->name);

        pa_sink_update_proplist(u->sink, PA_UPDATE_REPLACE, pl);
        pa_proplist_free(pl);
    }
}

/* Called from main context */
static void sink_input_volume_changed_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_volume_changed(u->sink, &i->volume);
}

/* Called from main context */
static void sink_input_mute_changed_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_mute_changed(u->sink, i->muted);
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_sample_spec ss_input, ss_output;
    pa_channel_map map_output;
    pa_modargs *ma;
    const char *master_name;
    const char *hrir_left_file;
    const char *hrir_right_file;
    pa_sink *master=NULL;
    pa_sink_input_new_data sink_input_data;
    pa_sink_new_data sink_data;
    bool use_volume_sharing = true;
    bool force_flat_volume = false;
    pa_memchunk silence;
    const char* z;
    unsigned i, j, ear, found_channel_left, found_channel_right;

    pa_sample_spec ss;
    pa_channel_map map;

    float *hrir_data=NULL, *hrir_right_data=NULL;
    float *hrir_temp_data;
    size_t hrir_samples;
    size_t hrir_copied_length, hrir_total_length;
    int hrir_channels;
    int fftlen;

    float *impulse_temp=NULL;

    unsigned *mapping_left=NULL;
    unsigned *mapping_right=NULL;

    fftwf_plan p;

    pa_channel_map hrir_map, hrir_right_map;

    pa_sample_spec hrir_left_temp_ss;
    pa_memchunk hrir_left_temp_chunk, hrir_left_temp_chunk_resampled;
    pa_resampler *resampler;


    pa_sample_spec hrir_right_temp_ss;
    pa_memchunk hrir_right_temp_chunk, hrir_right_temp_chunk_resampled;

    pa_assert(m);

    hrir_left_temp_chunk.memblock = NULL;
    hrir_left_temp_chunk_resampled.memblock = NULL;
    hrir_right_temp_chunk.memblock = NULL;
    hrir_right_temp_chunk_resampled.memblock = NULL;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    master_name = pa_modargs_get_value(ma, "sink_master", NULL);
    if (!master_name) {
        master_name = pa_modargs_get_value(ma, "master", NULL);
        if (master_name)
            pa_log_warn("The 'master' module argument is deprecated and may be removed in the future, "
                        "please use the 'sink_master' argument instead.");
    }

    if (!(master = pa_namereg_get(m->core, master_name, PA_NAMEREG_SINK))) {
        pa_log("Master sink not found");
        goto fail;
    }

    hrir_left_file = pa_modargs_get_value(ma, "hrir_left", NULL);
    if (!hrir_left_file) {
        hrir_left_file = pa_modargs_get_value(ma, "hrir", NULL);
        if (!hrir_left_file) {
            pa_log("Either the 'hrir' or 'hrir_left' module arguments are required.");
            goto fail;
        }
    }

    hrir_right_file = pa_modargs_get_value(ma, "hrir_right", NULL);

    pa_assert(master);

    if (pa_sound_file_load(master->core->mempool, hrir_left_file, &hrir_left_temp_ss, &hrir_map, &hrir_left_temp_chunk, NULL) < 0) {
        pa_log("Cannot load hrir file.");
        goto fail;
    }

    if (hrir_right_file) {
        if (pa_sound_file_load(master->core->mempool, hrir_right_file, &hrir_right_temp_ss, &hrir_right_map, &hrir_right_temp_chunk, NULL) < 0) {
            pa_log("Cannot load hrir_right file.");
            goto fail;
        }
        if (!pa_sample_spec_equal(&hrir_left_temp_ss, &hrir_right_temp_ss)) {
            pa_log("Both hrir_left and hrir_right must have the same sample format");
            goto fail;
        }
        if (!pa_channel_map_equal(&hrir_map, &hrir_right_map)) {
            pa_log("Both hrir_left and hrir_right must have the same channel layout");
            goto fail;
        }
    }

    ss_input.format = PA_SAMPLE_FLOAT32NE;
    ss_input.rate = master->sample_spec.rate;
    ss_input.channels = hrir_left_temp_ss.channels;

    ss = ss_input;
    map = hrir_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    ss.format = PA_SAMPLE_FLOAT32NE;
    ss_input.rate = ss.rate;
    ss_input.channels = ss.channels;

    ss_output = ss_input;
    ss_output.channels = 2;

    if (pa_modargs_get_value_boolean(ma, "use_volume_sharing", &use_volume_sharing) < 0) {
        pa_log("use_volume_sharing= expects a boolean argument");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "force_flat_volume", &force_flat_volume) < 0) {
        pa_log("force_flat_volume= expects a boolean argument");
        goto fail;
    }

    if (use_volume_sharing && force_flat_volume) {
        pa_log("Flat volume can't be forced when using volume sharing.");
        goto fail;
    }

    pa_channel_map_init_stereo(&map_output);

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    if (!(sink_data.name = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL))))
        sink_data.name = pa_sprintf_malloc("%s.vsurroundsink", master->name);
    pa_sink_new_data_set_sample_spec(&sink_data, &ss_input);
    pa_sink_new_data_set_channel_map(&sink_data, &map);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "filter");
    pa_proplist_sets(sink_data.proplist, "device.vsurroundsink.name", sink_data.name);

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    u->autoloaded = DEFAULT_AUTOLOADED;
    if (pa_modargs_get_value_boolean(ma, "autoloaded", &u->autoloaded) < 0) {
        pa_log("Failed to parse autoloaded value");
        goto fail;
    }

    if ((u->auto_desc = !pa_proplist_contains(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION))) {
        z = pa_proplist_gets(master->proplist, PA_PROP_DEVICE_DESCRIPTION);
        pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Virtual Surround Sink %s on %s", sink_data.name, z ? z : master->name);
    }

    u->sink = pa_sink_new(m->core, &sink_data, (master->flags & (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY))
                                               | (use_volume_sharing ? PA_SINK_SHARE_VOLUME_WITH_MASTER : 0));
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg_cb;
    u->sink->set_state_in_main_thread = sink_set_state_in_main_thread_cb;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->request_rewind = sink_request_rewind_cb;
    pa_sink_set_set_mute_callback(u->sink, sink_set_mute_cb);
    if (!use_volume_sharing) {
        pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);
        pa_sink_enable_decibel_volume(u->sink, true);
    }
    /* Normally this flag would be enabled automatically but we can force it. */
    if (force_flat_volume)
        u->sink->flags |= PA_SINK_FLAT_VOLUME;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, master->asyncmsgq);

    /* Create sink input */
    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    pa_sink_input_new_data_set_sink(&sink_input_data, master, false, true);
    sink_input_data.origin_sink = u->sink;
    pa_proplist_setf(sink_input_data.proplist, PA_PROP_MEDIA_NAME, "Virtual Surround Sink Stream from %s", pa_proplist_gets(u->sink->proplist, PA_PROP_DEVICE_DESCRIPTION));
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_ROLE, "filter");
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss_output);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map_output);

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
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
    u->sink_input->may_move_to = sink_input_may_move_to_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->volume_changed = use_volume_sharing ? NULL : sink_input_volume_changed_cb;
    u->sink_input->mute_changed = sink_input_mute_changed_cb;
    u->sink_input->userdata = u;

    u->sink->input_to_master = u->sink_input;

    pa_sink_input_get_silence(u->sink_input, &silence);

    resampler = pa_resampler_new(u->sink->core->mempool, &hrir_left_temp_ss, &hrir_map, &ss_input, &hrir_map, u->sink->core->lfe_crossover_freq,
                                 PA_RESAMPLER_SRC_SINC_BEST_QUALITY, PA_RESAMPLER_NO_REMAP);

    hrir_samples = hrir_left_temp_chunk.length / pa_frame_size(&hrir_left_temp_ss) * ss_input.rate / hrir_left_temp_ss.rate;

    hrir_total_length = hrir_samples * pa_frame_size(&ss_input);
    hrir_channels = ss_input.channels;

    hrir_data = (float *) pa_xmalloc(hrir_total_length);
    hrir_copied_length = 0;

    u->hrir_samples = hrir_samples;
    u->inputs = hrir_channels;

    /* add silence to the hrir until we get enough samples out of the resampler */
    while (hrir_copied_length < hrir_total_length) {
        pa_resampler_run(resampler, &hrir_left_temp_chunk, &hrir_left_temp_chunk_resampled);
        if (hrir_left_temp_chunk.memblock != hrir_left_temp_chunk_resampled.memblock) {
            /* Silence input block */
            pa_silence_memblock(hrir_left_temp_chunk.memblock, &hrir_left_temp_ss);
        }

        if (hrir_left_temp_chunk_resampled.memblock) {
            /* Copy hrir data */
            hrir_temp_data = (float *) pa_memblock_acquire(hrir_left_temp_chunk_resampled.memblock);

            if (hrir_total_length - hrir_copied_length >= hrir_left_temp_chunk_resampled.length) {
                memcpy(hrir_data + hrir_copied_length, hrir_temp_data, hrir_left_temp_chunk_resampled.length);
                hrir_copied_length += hrir_left_temp_chunk_resampled.length;
            } else {
                memcpy(hrir_data + hrir_copied_length, hrir_temp_data, hrir_total_length - hrir_copied_length);
                hrir_copied_length = hrir_total_length;
            }

            pa_memblock_release(hrir_left_temp_chunk_resampled.memblock);
            pa_memblock_unref(hrir_left_temp_chunk_resampled.memblock);
            hrir_left_temp_chunk_resampled.memblock = NULL;
        }
    }

    pa_memblock_unref(hrir_left_temp_chunk.memblock);
    hrir_left_temp_chunk.memblock = NULL;

    if (hrir_right_file) {
        pa_resampler_reset(resampler);

        hrir_right_data = (float *) pa_xmalloc(hrir_total_length);
        hrir_copied_length = 0;

        while (hrir_copied_length < hrir_total_length) {
            pa_resampler_run(resampler, &hrir_right_temp_chunk, &hrir_right_temp_chunk_resampled);
            if (hrir_right_temp_chunk.memblock != hrir_right_temp_chunk_resampled.memblock) {
                /* Silence input block */
                pa_silence_memblock(hrir_right_temp_chunk.memblock, &hrir_right_temp_ss);
            }

            if (hrir_right_temp_chunk_resampled.memblock) {
                /* Copy hrir data */
                hrir_temp_data = (float *) pa_memblock_acquire(hrir_right_temp_chunk_resampled.memblock);

                if (hrir_total_length - hrir_copied_length >= hrir_right_temp_chunk_resampled.length) {
                    memcpy(hrir_right_data + hrir_copied_length, hrir_temp_data, hrir_right_temp_chunk_resampled.length);
                    hrir_copied_length += hrir_right_temp_chunk_resampled.length;
                } else {
                    memcpy(hrir_right_data + hrir_copied_length, hrir_temp_data, hrir_total_length - hrir_copied_length);
                    hrir_copied_length = hrir_total_length;
                }

                pa_memblock_release(hrir_right_temp_chunk_resampled.memblock);
                pa_memblock_unref(hrir_right_temp_chunk_resampled.memblock);
                hrir_right_temp_chunk_resampled.memblock = NULL;
            }
        }

        pa_memblock_unref(hrir_right_temp_chunk.memblock);
        hrir_right_temp_chunk.memblock = NULL;
    }

    pa_resampler_free(resampler);

    if (hrir_right_data)
        normalize_hrir_stereo(hrir_data, hrir_right_data, hrir_samples, hrir_channels);
    else
        normalize_hrir(hrir_data, hrir_samples, hrir_channels);

    /* create mapping between hrir and input */
    mapping_left = (unsigned *) pa_xnew0(unsigned, hrir_channels);
    mapping_right = (unsigned *) pa_xnew0(unsigned, hrir_channels);
    for (i = 0; i < map.channels; i++) {
        found_channel_left = 0;
        found_channel_right = 0;

        for (j = 0; j < hrir_map.channels; j++) {
            if (hrir_map.map[j] == map.map[i]) {
                mapping_left[i] = j;
                found_channel_left = 1;
            }

            if (hrir_map.map[j] == mirror_channel(map.map[i])) {
                mapping_right[i] = j;
                found_channel_right = 1;
            }
        }

        if (!found_channel_left) {
            pa_log("Cannot find mapping for channel %s", pa_channel_position_to_string(map.map[i]));
            goto fail;
        }
        if (!found_channel_right) {
            pa_log("Cannot find mapping for channel %s", pa_channel_position_to_string(mirror_channel(map.map[i])));
            goto fail;
        }
    }

    fftlen = (hrir_samples + BLOCK_SIZE + 1); /* Grow a bit for overlap */
    {
        /* Round up to a power of two */
        int pow = 1;
        while (fftlen > 2) { pow++; fftlen /= 2; }
        fftlen = 2 << pow;
    }

    u->fftlen = fftlen;

    u->f_in = (fftwf_complex*) alloc(sizeof(fftwf_complex), (fftlen/2+1));
    u->f_out = (fftwf_complex*) alloc(sizeof(fftwf_complex), (fftlen/2+1));

    u->f_ir = (fftwf_complex**) alloc(sizeof(fftwf_complex*), (hrir_channels*2));
    for (i = 0, j = hrir_channels*2; i < j; i++)
        u->f_ir[i] = (fftwf_complex*) alloc(sizeof(fftwf_complex), (fftlen/2+1));

    u->revspace = (float*) alloc(sizeof(float), fftlen);

    u->outspace[0] = (float*) alloc(sizeof(float), BLOCK_SIZE);
    u->outspace[1] = (float*) alloc(sizeof(float), BLOCK_SIZE);

    u->inspace = (float**) alloc(sizeof(float*), hrir_channels);
    for (i = 0; i < hrir_channels; i++)
        u->inspace[i] = (float*) alloc(sizeof(float), fftlen);

    u->p_fw = (fftwf_plan*) alloc(sizeof(fftwf_plan), hrir_channels);
    for (i = 0; i < hrir_channels; i++)
        pa_assert_se(u->p_fw[i] = fftwf_plan_dft_r2c_1d(fftlen, u->inspace[i], u->f_in, FFTW_ESTIMATE));

    pa_assert_se(u->p_bw = fftwf_plan_dft_c2r_1d(fftlen, u->f_out, u->revspace, FFTW_ESTIMATE));

    impulse_temp = (float*) alloc(sizeof(float), fftlen);

    if (hrir_right_data) {
        for (i = 0; i < hrir_channels; i++) {
            for (ear = 0; ear < 2; ear++) {
                size_t index = i * 2 + ear;
                size_t impulse_index = mapping_left[i];
                float *impulse = (ear == 0) ? hrir_data : hrir_right_data;
                for (j = 0; j < hrir_samples; j++) {
                    impulse_temp[j] = impulse[j * hrir_channels + impulse_index];
                }

                p = fftwf_plan_dft_r2c_1d(fftlen, impulse_temp, u->f_ir[index], FFTW_ESTIMATE);
                if (p) {
                    fftwf_execute(p);
                    fftwf_destroy_plan(p);
                } else {
                    pa_log("fftw plan creation failed for %s ear speaker index %d", (ear == 0) ? "left" : "right", i);
                    goto fail;
                }
            }
        }
    } else {
        for (i = 0; i < hrir_channels; i++) {
            for (ear = 0; ear < 2; ear++) {
                size_t index = i * 2 + ear;
                size_t impulse_index = (ear == 0) ? mapping_left[i] : mapping_right[i];
                for (j = 0; j < hrir_samples; j++) {
                    impulse_temp[j] = hrir_data[j * hrir_channels + impulse_index];
                }

                p = fftwf_plan_dft_r2c_1d(fftlen, impulse_temp, u->f_ir[index], FFTW_ESTIMATE);
                if (p) {
                    fftwf_execute(p);
                    fftwf_destroy_plan(p);
                } else {
                    pa_log("fftw plan creation failed for %s ear speaker index %d", (ear == 0) ? "left" : "right", i);
                    goto fail;
                }
            }
        }
    }

    pa_xfree(impulse_temp);

    pa_xfree(hrir_data);
    if (hrir_right_data)
        pa_xfree(hrir_right_data);

    pa_xfree(mapping_left);
    pa_xfree(mapping_right);

    u->memblockq_sink = pa_memblockq_new("module-virtual-surround-sink memblockq (input)", 0, MEMBLOCKQ_MAXLENGTH, sink_bytes(u, BLOCK_SIZE), &ss_input, 0, 0, sink_bytes(u, u->fftlen), &silence);
    pa_memblock_unref(silence.memblock);

    pa_memblockq_seek(u->memblockq_sink, sink_bytes(u, u->fftlen - BLOCK_SIZE), PA_SEEK_RELATIVE, false);
    pa_memblockq_flush_read(u->memblockq_sink);

    pa_sink_put(u->sink);
    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);

    return 0;

fail:
    if (impulse_temp)
        pa_xfree(impulse_temp);

    if (mapping_left)
        pa_xfree(mapping_left);

    if (mapping_right)
        pa_xfree(mapping_right);

    if (hrir_data)
        pa_xfree(hrir_data);

    if (hrir_right_data)
        pa_xfree(hrir_right_data);

    if (hrir_left_temp_chunk.memblock)
        pa_memblock_unref(hrir_left_temp_chunk.memblock);

    if (hrir_left_temp_chunk_resampled.memblock)
        pa_memblock_unref(hrir_left_temp_chunk_resampled.memblock);

    if (hrir_right_temp_chunk.memblock)
        pa_memblock_unref(hrir_right_temp_chunk.memblock);

    if (hrir_right_temp_chunk_resampled.memblock)
        pa_memblock_unref(hrir_right_temp_chunk_resampled.memblock);

    if (ma)
        pa_modargs_free(ma);

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
    size_t i, j;
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

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

    if (u->memblockq_sink)
        pa_memblockq_free(u->memblockq_sink);

    if (u->p_fw) {
        for (i = 0, j = u->inputs; i < j; i++) {
            if (u->p_fw[i])
                fftwf_destroy_plan(u->p_fw[i]);
        }
        fftwf_free(u->p_fw);
    }

    if (u->p_bw)
        fftwf_destroy_plan(u->p_bw);

    if (u->f_ir) {
        for (i = 0, j = u->inputs * 2; i < j; i++) {
            if (u->f_ir[i])
                fftwf_free(u->f_ir[i]);
        }
        fftwf_free(u->f_ir);
    }

    if (u->f_out)
        fftwf_free(u->f_out);

    if (u->f_in)
        fftwf_free(u->f_in);

    if (u->revspace)
        fftwf_free(u->revspace);

    if (u->outspace[0])
        fftwf_free(u->outspace[0]);
    if (u->outspace[1])
        fftwf_free(u->outspace[1]);

    if (u->inspace) {
        for (i = 0, j = u->inputs; i < j; i++) {
            if (u->inspace[i])
                fftwf_free(u->inspace[i]);
        }
        fftwf_free(u->inspace);
    }

    pa_xfree(u);
}
