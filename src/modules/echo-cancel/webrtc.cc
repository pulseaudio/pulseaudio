/***
    This file is part of PulseAudio.

    Copyright 2011 Collabora Ltd.
              2015 Aldebaran SoftBank Group
              2020 Arun Raghavan <arun@asymptotic.io>
              2020 Eero Nurkkala <eero.nurkkala@offcode.fi>

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

#include <pulse/cdecl.h>

PA_C_DECL_BEGIN
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>

#include <pulse/timeval.h>
#include "echo-cancel.h"
PA_C_DECL_END

#define WEBRTC_APM_DEBUG_DUMP 0

#include <modules/audio_processing/include/audio_processing.h>

#define BLOCK_SIZE_US 10000

#define DEFAULT_HIGH_PASS_FILTER true
#define DEFAULT_NOISE_SUPPRESSION true
#define DEFAULT_TRANSIENT_NOISE_SUPPRESSION true
#define DEFAULT_ANALOG_GAIN_CONTROL true
#define DEFAULT_DIGITAL_GAIN_CONTROL false
#define DEFAULT_MOBILE false
#define DEFAULT_COMFORT_NOISE true
#define DEFAULT_DRIFT_COMPENSATION false
#define DEFAULT_VAD false
#define DEFAULT_AGC_START_VOLUME 85
#define DEFAULT_POSTAMP_ENABLE false
#define DEFAULT_POSTAMP_GAIN_DB 0
#define DEFAULT_PREAMP_ENABLE false
#define DEFAULT_PREAMP_GAIN_DB 0

#define WEBRTC_AGC_MAX_VOLUME 255
#define WEBRTC_POSTAMP_GAIN_MAX_DB 90
#define WEBRTC_PREAMP_GAIN_MAX_DB 90

static const char* const valid_modargs[] = {
    "agc_start_volume",
    "analog_gain_control",
    "digital_gain_control",
    "high_pass_filter",
    "mobile",
    "noise_suppression",
    "post_amplifier",
    "post_amplifier_gain",
    "pre_amplifier",
    "pre_amplifier_gain",
    "transient_noise_suppression",
    "voice_detection",
    NULL
};

static int webrtc_volume_from_pa(pa_volume_t v)
{
    return (v * WEBRTC_AGC_MAX_VOLUME) / PA_VOLUME_NORM;
}

static pa_volume_t webrtc_volume_to_pa(int v)
{
    return (v * PA_VOLUME_NORM) / WEBRTC_AGC_MAX_VOLUME;
}

static void webrtc_ec_fixate_spec(pa_sample_spec *rec_ss, pa_channel_map *rec_map,
                                  pa_sample_spec *play_ss, pa_channel_map *play_map,
                                  pa_sample_spec *out_ss, pa_channel_map *out_map)
{
    rec_ss->format = PA_SAMPLE_FLOAT32NE;
    play_ss->format = PA_SAMPLE_FLOAT32NE;

    /* AudioProcessing expects one of the following rates */
    if (rec_ss->rate >= 48000)
        rec_ss->rate = 48000;
    else if (rec_ss->rate >= 32000)
        rec_ss->rate = 32000;
    else if (rec_ss->rate >= 16000)
        rec_ss->rate = 16000;
    else
        rec_ss->rate = 8000;

    *out_ss = *rec_ss;
    *out_map = *rec_map;

    /* Playback stream rate needs to be the same as capture */
    play_ss->rate = rec_ss->rate;
}

bool pa_webrtc_ec_init(pa_core *c, pa_echo_canceller *ec,
                       pa_sample_spec *rec_ss, pa_channel_map *rec_map,
                       pa_sample_spec *play_ss, pa_channel_map *play_map,
                       pa_sample_spec *out_ss, pa_channel_map *out_map,
                       uint32_t *nframes, const char *args) {
    webrtc::AudioProcessing *apm = webrtc::AudioProcessingBuilder().Create();
    webrtc::ProcessingConfig pconfig;
    webrtc::AudioProcessing::Config config;
    bool hpf, ns, tns, agc, dgc, mobile, pre_amp, vad, post_amp;
    int i;
    uint32_t agc_start_volume, pre_amp_gain, post_amp_gain;
    pa_modargs *ma;

    if (!(ma = pa_modargs_new(args, valid_modargs))) {
        pa_log("Failed to parse submodule arguments.");
        goto fail;
    }

    hpf = DEFAULT_HIGH_PASS_FILTER;
    if (pa_modargs_get_value_boolean(ma, "high_pass_filter", &hpf) < 0) {
        pa_log("Failed to parse high_pass_filter value");
        goto fail;
    }

    ns = DEFAULT_NOISE_SUPPRESSION;
    if (pa_modargs_get_value_boolean(ma, "noise_suppression", &ns) < 0) {
        pa_log("Failed to parse noise_suppression value");
        goto fail;
    }

    tns = DEFAULT_TRANSIENT_NOISE_SUPPRESSION;
    if (pa_modargs_get_value_boolean(ma, "transient_noise_suppression", &tns) < 0) {
        pa_log("Failed to parse transient_noise_suppression value");
        goto fail;
    }

    agc = DEFAULT_ANALOG_GAIN_CONTROL;
    if (pa_modargs_get_value_boolean(ma, "analog_gain_control", &agc) < 0) {
        pa_log("Failed to parse analog_gain_control value");
        goto fail;
    }

    dgc = agc ? false : DEFAULT_DIGITAL_GAIN_CONTROL;
    if (pa_modargs_get_value_boolean(ma, "digital_gain_control", &dgc) < 0) {
        pa_log("Failed to parse digital_gain_control value");
        goto fail;
    }

    if (agc && dgc) {
        pa_log("You must pick only one between analog and digital gain control");
        goto fail;
    }

    pre_amp = DEFAULT_PREAMP_ENABLE;
    if (pa_modargs_get_value_boolean(ma, "pre_amplifier", &pre_amp) < 0) {
        pa_log("Failed to parse pre_amplifier value");
        goto fail;
    }
    pre_amp_gain = DEFAULT_PREAMP_GAIN_DB;
    if (pa_modargs_get_value_u32(ma, "pre_amplifier_gain", &pre_amp_gain) < 0) {
        pa_log("Failed to parse pre_amplifier_gain value");
        goto fail;
    }
    if (pre_amp_gain > WEBRTC_PREAMP_GAIN_MAX_DB) {
        pa_log("Preamp gain must not exceed %u", WEBRTC_PREAMP_GAIN_MAX_DB);
        goto fail;
    }

    post_amp = DEFAULT_POSTAMP_ENABLE;
    if (pa_modargs_get_value_boolean(ma, "post_amplifier", &post_amp) < 0) {
        pa_log("Failed to parse post_amplifier value");
        goto fail;
    }
    post_amp_gain = DEFAULT_POSTAMP_GAIN_DB;
    if (pa_modargs_get_value_u32(ma, "post_amplifier_gain", &post_amp_gain) < 0) {
        pa_log("Failed to parse post_amplifier_gain value");
        goto fail;
    }
    if (post_amp_gain > WEBRTC_POSTAMP_GAIN_MAX_DB) {
        pa_log("Postamp gain must not exceed %u", WEBRTC_POSTAMP_GAIN_MAX_DB);
        goto fail;
    }

    mobile = DEFAULT_MOBILE;
    if (pa_modargs_get_value_boolean(ma, "mobile", &mobile) < 0) {
        pa_log("Failed to parse mobile value");
        goto fail;
    }

    ec->params.drift_compensation = DEFAULT_DRIFT_COMPENSATION;

    vad = DEFAULT_VAD;
    if (pa_modargs_get_value_boolean(ma, "voice_detection", &vad) < 0) {
        pa_log("Failed to parse voice_detection value");
        goto fail;
    }

    agc_start_volume = DEFAULT_AGC_START_VOLUME;
    if (pa_modargs_get_value_u32(ma, "agc_start_volume", &agc_start_volume) < 0) {
        pa_log("Failed to parse agc_start_volume value");
        goto fail;
    }
    if (agc_start_volume > WEBRTC_AGC_MAX_VOLUME) {
        pa_log("AGC start volume must not exceed %u", WEBRTC_AGC_MAX_VOLUME);
        goto fail;
    }
    ec->params.webrtc.agc_start_volume = agc_start_volume;

    webrtc_ec_fixate_spec(rec_ss, rec_map, play_ss, play_map, out_ss, out_map);

    pconfig = {
        webrtc::StreamConfig(rec_ss->rate, rec_ss->channels, false), /* input stream */
        webrtc::StreamConfig(out_ss->rate, out_ss->channels, false), /* output stream */
        webrtc::StreamConfig(play_ss->rate, play_ss->channels, false), /* reverse input stream */
        webrtc::StreamConfig(play_ss->rate, play_ss->channels, false), /* reverse output stream */
    };
    if (apm->Initialize(pconfig) != webrtc::AudioProcessing::kNoError) {
        pa_log("Error initialising audio processing module");
        goto fail;
    }

    if (pre_amp) {
       config.pre_amplifier.enabled = true;
       config.pre_amplifier.fixed_gain_factor = (float)pre_amp_gain;
    } else
       config.pre_amplifier.enabled = false;

    if (hpf)
        config.high_pass_filter.enabled = true;
    else
        config.high_pass_filter.enabled = false;

    config.echo_canceller.enabled = true;
    config.pipeline.multi_channel_capture = rec_ss->channels > 1;
    config.pipeline.multi_channel_render = play_ss->channels > 1;

    if (!mobile)
        config.echo_canceller.mobile_mode = false;
    else
        config.echo_canceller.mobile_mode = true;

    if (ns)
       config.noise_suppression.enabled = true;
    else
       config.noise_suppression.enabled = false;

    if (tns)
       config.transient_suppression.enabled = true;
    else
       config.transient_suppression.enabled = false;

    if (dgc) {
        ec->params.webrtc.agc = false;
        config.gain_controller1.enabled = true;
        if (mobile)
            config.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kFixedDigital;
        else
            config.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
    } else if (agc) {
        ec->params.webrtc.agc = true;
        config.gain_controller1.enabled = true;
        config.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;
        config.gain_controller1.analog_level_minimum = 0;
        config.gain_controller1.analog_level_maximum = WEBRTC_AGC_MAX_VOLUME;
    }

    if (vad)
        config.voice_detection.enabled = true;
    else
        config.voice_detection.enabled = false;

    if (post_amp) {
        config.gain_controller2.enabled = true;
        config.gain_controller2.fixed_digital.gain_db = (float)post_amp_gain;
        config.gain_controller2.adaptive_digital.enabled = false;
    } else
        config.gain_controller2.enabled = false;

    ec->params.webrtc.apm = apm;
    ec->params.webrtc.rec_ss = *rec_ss;
    ec->params.webrtc.play_ss = *play_ss;
    ec->params.webrtc.out_ss = *out_ss;
    ec->params.webrtc.blocksize = (uint64_t) out_ss->rate * BLOCK_SIZE_US / PA_USEC_PER_SEC;
    *nframes = ec->params.webrtc.blocksize;
    ec->params.webrtc.first = true;

    apm->ApplyConfig(config);

    for (i = 0; i < rec_ss->channels; i++)
        ec->params.webrtc.rec_buffer[i] = pa_xnew(float, *nframes);
    for (i = 0; i < play_ss->channels; i++)
        ec->params.webrtc.play_buffer[i] = pa_xnew(float, *nframes);

    pa_modargs_free(ma);
    return true;

fail:
    if (ma)
        pa_modargs_free(ma);
    if (apm)
        delete apm;

    return false;
}

void pa_webrtc_ec_play(pa_echo_canceller *ec, const uint8_t *play) {
    webrtc::AudioProcessing *apm = (webrtc::AudioProcessing*)ec->params.webrtc.apm;
    const pa_sample_spec *ss = &ec->params.webrtc.play_ss;
    int n = ec->params.webrtc.blocksize;
    float **buf = ec->params.webrtc.play_buffer;
    webrtc::StreamConfig config(ss->rate, ss->channels, false);

    pa_deinterleave(play, (void **) buf, ss->channels, pa_sample_size(ss), n);

    pa_assert_se(apm->ProcessReverseStream(buf, config, config, buf) == webrtc::AudioProcessing::kNoError);
}

void pa_webrtc_ec_record(pa_echo_canceller *ec, const uint8_t *rec, uint8_t *out) {
    webrtc::AudioProcessing *apm = (webrtc::AudioProcessing*)ec->params.webrtc.apm;
    const pa_sample_spec *rec_ss = &ec->params.webrtc.rec_ss;
    const pa_sample_spec *out_ss = &ec->params.webrtc.out_ss;
    float **buf = ec->params.webrtc.rec_buffer;
    int n = ec->params.webrtc.blocksize;
    int old_volume, new_volume;
    webrtc::StreamConfig rec_config(rec_ss->rate, rec_ss->channels, false);
    webrtc::StreamConfig out_config(out_ss->rate, out_ss->channels, false);

    pa_deinterleave(rec, (void **) buf, rec_ss->channels, pa_sample_size(rec_ss), n);

    if (ec->params.webrtc.agc) {
        pa_volume_t v = pa_echo_canceller_get_capture_volume(ec);
        old_volume = webrtc_volume_from_pa(v);
        apm->set_stream_analog_level(old_volume);
    }

    apm->set_stream_delay_ms(0);
    pa_assert_se(apm->ProcessStream(buf, rec_config, out_config, buf) == webrtc::AudioProcessing::kNoError);

    if (ec->params.webrtc.agc) {
        if (PA_UNLIKELY(ec->params.webrtc.first)) {
            /* We start at a sane default volume (taken from the Chromium
             * condition on the experimental AGC in audio_processing.h). This is
             * needed to make sure that there's enough energy in the capture
             * signal for the AGC to work */
            ec->params.webrtc.first = false;
            new_volume = ec->params.webrtc.agc_start_volume;
        } else {
            new_volume = apm->recommended_stream_analog_level();
        }

        if (old_volume != new_volume)
            pa_echo_canceller_set_capture_volume(ec, webrtc_volume_to_pa(new_volume));
    }

    pa_interleave((const void **) buf, out_ss->channels, out, pa_sample_size(out_ss), n);
}

void pa_webrtc_ec_set_drift(pa_echo_canceller *ec, float drift) {
}

void pa_webrtc_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out) {
    pa_webrtc_ec_play(ec, play);
    pa_webrtc_ec_record(ec, rec, out);
}

void pa_webrtc_ec_done(pa_echo_canceller *ec) {
    int i;

    if (ec->params.webrtc.apm) {
        delete (webrtc::AudioProcessing*)ec->params.webrtc.apm;
        ec->params.webrtc.apm = NULL;
    }

    for (i = 0; i < ec->params.webrtc.rec_ss.channels; i++)
        pa_xfree(ec->params.webrtc.rec_buffer[i]);
    for (i = 0; i < ec->params.webrtc.play_ss.channels; i++)
        pa_xfree(ec->params.webrtc.play_buffer[i]);
}
