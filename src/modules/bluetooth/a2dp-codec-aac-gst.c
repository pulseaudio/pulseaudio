/***
  This file is part of PulseAudio.

  Copyright (C) 2020 Asymptotic <sanchayan@asymptotic.io>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/once.h>
#include <pulse/sample.h>

#include <arpa/inet.h>

#include "a2dp-codecs.h"
#include "a2dp-codec-api.h"
#include "a2dp-codec-gst.h"
#include "rtp.h"

static bool can_be_supported(bool for_encoding) {
    GstElementFactory *element_factory;

    if (for_encoding) {
        element_factory = gst_element_factory_find("fdkaacenc");
        if (element_factory == NULL) {
            pa_log_info("AAC encoder element `fdkaacenc` not found");
            return false;
        }

        gst_object_unref(element_factory);
    } else {
        element_factory = gst_element_factory_find("fdkaacdec");
        if (element_factory == NULL) {
            pa_log_info("AAC decoder element `fdkaacdec` not found");
            return false;
        }

        gst_object_unref(element_factory);
    }

    return true;
}

static bool can_accept_capabilities(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding) {
    const a2dp_aac_t *capabilities = (const a2dp_aac_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities))
        return false;

    if (!(capabilities->object_type & (AAC_OBJECT_TYPE_MPEG2_AAC_LC | AAC_OBJECT_TYPE_MPEG4_AAC_LC))) {
        pa_log_error("Invalid object type in AAC configuration: %d", capabilities->object_type);
        return false;
    }

    if (!(AAC_GET_FREQUENCY(*capabilities) &
                                    (AAC_SAMPLING_FREQ_8000  | AAC_SAMPLING_FREQ_11025 |
                                     AAC_SAMPLING_FREQ_12000 | AAC_SAMPLING_FREQ_16000 |
                                     AAC_SAMPLING_FREQ_22050 | AAC_SAMPLING_FREQ_24000 |
                                     AAC_SAMPLING_FREQ_32000 | AAC_SAMPLING_FREQ_44100 |
                                     AAC_SAMPLING_FREQ_48000 | AAC_SAMPLING_FREQ_64000 |
                                     AAC_SAMPLING_FREQ_88200 | AAC_SAMPLING_FREQ_96000)))
        return false;

    if (!(capabilities->channels & (AAC_CHANNELS_1 | AAC_CHANNELS_2)))
        return false;

    return true;
}

static const char *choose_remote_endpoint(const pa_hashmap *capabilities_hashmap, const pa_sample_spec *default_sample_spec, bool for_encoding) {
    const pa_a2dp_codec_capabilities *a2dp_capabilities;
    const char *key;
    void *state;

    /* There is no preference, just choose random valid entry */
    PA_HASHMAP_FOREACH_KV(key, a2dp_capabilities, capabilities_hashmap, state) {
        if (can_accept_capabilities(a2dp_capabilities->buffer, a2dp_capabilities->size, for_encoding))
            return key;
    }

    return NULL;
}

static uint8_t fill_capabilities(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_aac_t *capabilities = (a2dp_aac_t *) capabilities_buffer;

    pa_zero(*capabilities);

    capabilities->object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC | AAC_OBJECT_TYPE_MPEG4_AAC_LC;
    capabilities->channels = AAC_CHANNELS_1 | AAC_CHANNELS_2;
    capabilities->vbr = 0;
    AAC_SET_BITRATE(*capabilities, 0xFFFFF);
    AAC_SET_FREQUENCY(*capabilities,
            (AAC_SAMPLING_FREQ_8000 | AAC_SAMPLING_FREQ_11025 | AAC_SAMPLING_FREQ_12000 |
            AAC_SAMPLING_FREQ_16000 | AAC_SAMPLING_FREQ_22050 | AAC_SAMPLING_FREQ_24000 |
            AAC_SAMPLING_FREQ_32000 | AAC_SAMPLING_FREQ_44100 | AAC_SAMPLING_FREQ_48000 |
            AAC_SAMPLING_FREQ_64000 | AAC_SAMPLING_FREQ_88200 | AAC_SAMPLING_FREQ_96000));

    return sizeof(*capabilities);
}

static bool is_configuration_valid(const uint8_t *config_buffer, uint8_t config_size) {
    const a2dp_aac_t *config = (const a2dp_aac_t *) config_buffer;
    uint32_t frequency;

    if (config_size != sizeof(*config)) {
        pa_log_error("Invalid size of config buffer");
        return false;
    }

    switch (config->object_type) {
        /*
         * AAC Long Term Prediction and AAC Scalable are not supported by the
         * FDK-AAC library.
         */
        case AAC_OBJECT_TYPE_MPEG4_AAC_LC:
        case AAC_OBJECT_TYPE_MPEG2_AAC_LC:
            break;
        default:
            pa_log_error("Invalid object type in AAC configuration");
            return false;
    }

    frequency = AAC_GET_FREQUENCY(*config);

    if (frequency != AAC_SAMPLING_FREQ_8000 && frequency != AAC_SAMPLING_FREQ_11025 &&
        frequency != AAC_SAMPLING_FREQ_12000 && frequency != AAC_SAMPLING_FREQ_16000 &&
        frequency != AAC_SAMPLING_FREQ_22050 && frequency != AAC_SAMPLING_FREQ_24000 &&
        frequency != AAC_SAMPLING_FREQ_32000 && frequency != AAC_SAMPLING_FREQ_44100 &&
        frequency != AAC_SAMPLING_FREQ_48000 && frequency != AAC_SAMPLING_FREQ_64000 &&
        frequency != AAC_SAMPLING_FREQ_88200 && frequency != AAC_SAMPLING_FREQ_96000) {
        pa_log_error("Invalid sampling frequency in configuration");
        return false;
    }

    if (config->channels != AAC_CHANNELS_1 && config->channels != AAC_CHANNELS_2) {
        pa_log_error("Invalid channel number in configuration");
        return false;
    }

    return true;
}

static uint8_t fill_preferred_configuration(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_aac_t *config = (a2dp_aac_t *) config_buffer;
    const a2dp_aac_t *capabilities = (const a2dp_aac_t *) capabilities_buffer;
    int i;

    static const struct {
        uint32_t rate;
        uint32_t cap;
    } freq_table[] = {
        { 8000 , AAC_SAMPLING_FREQ_8000  },
        { 11025, AAC_SAMPLING_FREQ_11025 },
        { 12000, AAC_SAMPLING_FREQ_12000 },
        { 16000, AAC_SAMPLING_FREQ_16000 },
        { 22050, AAC_SAMPLING_FREQ_22050 },
        { 24000, AAC_SAMPLING_FREQ_24000 },
        { 32000, AAC_SAMPLING_FREQ_32000 },
        { 44100, AAC_SAMPLING_FREQ_44100 },
        { 48000, AAC_SAMPLING_FREQ_48000 },
        { 64000, AAC_SAMPLING_FREQ_64000 },
        { 88200, AAC_SAMPLING_FREQ_88200 },
        { 96000, AAC_SAMPLING_FREQ_96000 }
    };

    if (capabilities_size != sizeof(*capabilities)) {
        pa_log_error("Invalid size of capabilities buffer");
        return 0;
    }

    pa_zero(*config);

    if (capabilities->object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC)
        config->object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC;
    else if (capabilities->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC)
        config->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
    else if (capabilities->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP)
        config->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LTP;
    else if (capabilities->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA)
        config->object_type = AAC_OBJECT_TYPE_MPEG4_AAC_SCA;
    else {
        pa_log_error("No supported aac object type");
        return 0;
    }

    if (!(capabilities->channels & (AAC_CHANNELS_1 | AAC_CHANNELS_2))) {
        pa_log_error("No supported channel modes");
        return 0;
    }

    switch (default_sample_spec->channels) {
        case 1:
            config->channels = AAC_CHANNELS_1;
            break;
        case 2:
            config->channels = AAC_CHANNELS_2;
            break;
        default:
            pa_log_error("Invalid channel in default sample spec");
            return 0;
    }

    AAC_SET_BITRATE(*config, AAC_GET_BITRATE(*capabilities));
    pa_log_info("AAC bitrate in preferred configuration: %d", AAC_GET_BITRATE(*capabilities));

    config->vbr = 0;

    /* Find the lowest freq that is at least as high as the requested sampling rate */
    for (i = 0; (unsigned) i < PA_ELEMENTSOF(freq_table); i++) {
        if (freq_table[i].rate >= default_sample_spec->rate && (AAC_GET_FREQUENCY(*capabilities) & freq_table[i].cap)) {
            AAC_SET_FREQUENCY((*config), freq_table[i].cap);
            break;
        }
    }

    if ((unsigned) i == PA_ELEMENTSOF(freq_table)) {
        for (--i; i >= 0; i--) {
            if (AAC_GET_FREQUENCY(*capabilities) & freq_table[i].cap) {
                AAC_SET_FREQUENCY((*config), freq_table[i].cap);
                break;
            }
        }

        if (i < 0) {
            pa_log_error("Not suitable sample rate");
            return 0;
        }
    }

    return sizeof(*config);
}

GstElement *gst_init_aac(struct gst_info *info, pa_sample_spec *ss, bool for_encoding) {
    GstElement *bin, *sink, *src, *capsf;
    GstCaps *caps;
    GstPad *pad;
    unsigned int mpegversion;

    ss->format = PA_SAMPLE_S16LE;

    switch (AAC_GET_FREQUENCY(*(info->a2dp_codec_t.aac_config))) {
        case AAC_SAMPLING_FREQ_8000:
            ss->rate = 8000u;
            break;
        case AAC_SAMPLING_FREQ_11025:
            ss->rate = 11025u;
            break;
        case AAC_SAMPLING_FREQ_12000:
            ss->rate = 12000u;
            break;
        case AAC_SAMPLING_FREQ_16000:
            ss->rate = 16000u;
            break;
        case AAC_SAMPLING_FREQ_22050:
            ss->rate = 22050u;
            break;
        case AAC_SAMPLING_FREQ_24000:
            ss->rate = 24000u;
            break;
        case AAC_SAMPLING_FREQ_32000:
            ss->rate = 32000u;
            break;
        case AAC_SAMPLING_FREQ_44100:
            ss->rate = 44100u;
            break;
        case AAC_SAMPLING_FREQ_48000:
            ss->rate = 48000u;
            break;
        case AAC_SAMPLING_FREQ_64000:
            ss->rate = 64000u;
            break;
        case AAC_SAMPLING_FREQ_88200:
            ss->rate = 88200u;
            break;
        case AAC_SAMPLING_FREQ_96000:
            ss->rate = 96000u;
            break;
        default:
            pa_log_error("Invalid AAC frequency configuration");
            goto fail;
    }

    switch (info->a2dp_codec_t.aac_config->channels) {
        case AAC_CHANNELS_1:
            ss->channels = 1;
            break;
        case AAC_CHANNELS_2:
            ss->channels = 2;
            break;
        default:
            pa_log_error("Invalid AAC channel configuration");
            goto fail;
    }

    /*
     * As per section 4.5.4 Media Payload Format of A2DP profile, MPEG-2,4
     * AAC uses the media payload format defined in RFC3016. The specification
     * defines the payload format only for MPEG-4 audio; in use of MPEG-2
     * AAC LC, the audio stream shall be transformed to MPEG-4 AAC LC in
     * the SRC by modifying the codec information and adapted into MPEG-4
     * LATM format before being put into Media Payload Format. The SNK
     * shall retransform the stream into MPEG-2 AAC LC, if necessary.
     *
     * As a result, even if we get MPEG2 AAC LC as the object type, we
     * keep the MPEG version as 4 in the caps below and use LATM-MCP1.
     */

    if (info->a2dp_codec_t.aac_config->object_type == AAC_OBJECT_TYPE_MPEG2_AAC_LC)
        mpegversion = 2;
    else if (info->a2dp_codec_t.aac_config->object_type == AAC_OBJECT_TYPE_MPEG4_AAC_LC)
        mpegversion = 4;
    else {
        pa_log_error("Unknown codec object type %#x", info->a2dp_codec_t.aac_config->object_type);
        goto fail;
    }

    pa_log_debug("Got object type MPEG%d AAC LC", mpegversion);

    capsf = gst_element_factory_make("capsfilter", "aac_capsfilter");
    if (!capsf) {
        pa_log_error("Could not create AAC capsfilter element");
        goto fail;
    }

    caps = gst_caps_new_simple("audio/mpeg",
            "mpegversion", G_TYPE_INT, (int) 4,
            "rate", G_TYPE_INT, (int) ss->rate,
            "channels", G_TYPE_INT, (int) ss->channels,
            "stream-format", G_TYPE_STRING, "latm-mcp1",
            NULL);
    g_object_set(capsf, "caps", caps, NULL);
    gst_caps_unref(caps);

    if (for_encoding) {
        guint bitrate;

        sink = gst_element_factory_make("fdkaacenc", "aac_enc");
        src = capsf;
        if (!sink) {
            pa_log_error("Could not create AAC encoder element");
            goto fail_enc_dec;
        }

        /*
         * General negotiated MTU for bluetooth seems to be 894/895. Hardcode
         * this for now. Ideally MTU would have been provided to us at init,
         * but, the get_block_size function is called later in the current
         * code flow path.
         *
         * We cannot handle fragmentation. Fix the bitrate to not overshoot
         * the MTU. Any greater than the calculated value here or above 320
         * Kbps will result in payloads > MTU = 894.
         */
        bitrate = ((894 - sizeof(struct rtp_header)) * 8 * ss->rate) / 1024;
        bitrate = PA_MIN(bitrate, AAC_GET_BITRATE(*(info->a2dp_codec_t.aac_config)));

        /*
         * Note that it has been observed that some devices do not work if
         * header-period is not set to this value. We enable afterburner here
         * for better quality.
         *
         * For a value of '0', for the bitrate, the GStreamer fdkaac element
         * will decide the bitrate based on the recommended bitrate and
         * sampling combinations as per below.
         * http://wiki.hydrogenaud.io/index.php?title=Fraunhofer_FDK_AAC#Recommended_Sampling_Rate_and_Bitrate_Combinations
         *
         * We set peak bitrate to fix the maximum bits per audio frame. While
         * the library mentions this will affect the audio quality by a large
         * amount, considering bluetooth bandwidth we need to set this. We do
         * not handle fragmentation and this combined with the bitrate
         * calculation above, should make sure we not do overshoot above MTU.
         */
        g_object_set(sink, "bitrate", bitrate /* CBR */, "peak-bitrate", bitrate, "header-period", 1, "afterburner", TRUE, NULL);

        bin = gst_bin_new("aac_enc_bin");
    } else {
        sink = capsf;
        src = gst_element_factory_make("fdkaacdec", "aac_dec");
        if (!src) {
            pa_log_error("Could not create AAC decoder element");
            goto fail_enc_dec;
        }

        bin = gst_bin_new("aac_dec_bin");
    }

    pa_assert(bin);

    gst_bin_add_many(GST_BIN(bin), sink, src, NULL);
    pa_assert_se(gst_element_link_many(sink, src, NULL));

    pad = gst_element_get_static_pad(sink, "sink");
    pa_assert_se(gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad)));
    gst_object_unref(GST_OBJECT(pad));

    pad = gst_element_get_static_pad(src, "src");
    pa_assert_se(gst_element_add_pad(bin, gst_ghost_pad_new("src", pad)));
    gst_object_unref(GST_OBJECT(pad));

    return bin;

fail_enc_dec:
    gst_object_unref(GST_OBJECT(capsf));

fail:
    pa_log_error("AAC encoder initialisation failed");
    return NULL;
}

static void *init(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    GstElement *bin;
    struct gst_info *info = NULL;

    info = pa_xnew0(struct gst_info, 1);
    pa_assert(info);

    info->core = core;
    info->ss = sample_spec;

    info->codec_type = AAC;
    info->a2dp_codec_t.aac_config = (const a2dp_aac_t *) config_buffer;
    pa_assert(config_size == sizeof(*(info->a2dp_codec_t.aac_config)));

    if (!(bin = gst_init_aac(info, sample_spec, for_encoding)))
        goto fail;

    if (!gst_codec_init(info, for_encoding, bin))
        goto fail;

    return info;

fail:
    if (info)
        pa_xfree(info);

    return NULL;
}

static void deinit(void *codec_info) {
    return gst_codec_deinit(codec_info);
}

static int reset(void *codec_info) {
    struct gst_info *info = (struct gst_info *) codec_info;

    info->seq_num = 0;

    return 0;
}

static size_t get_block_size(void *codec_info, size_t link_mtu) {
    struct gst_info *info = (struct gst_info *) codec_info;

    /* aacEncoder.pdf Section 3.2.1
     * AAC-LC audio frame contains 1024 PCM samples per channel */
    return 1024 * pa_frame_size(info->ss);
}

static size_t reduce_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    return 0;
}

static size_t encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct gst_info *info = (struct gst_info *) codec_info;
    struct rtp_header *header;
    size_t written;

    if (PA_UNLIKELY(output_size < sizeof(*header))) {
        *processed = 0;
        return 0;
    }

    written = gst_transcode_buffer(codec_info, input_buffer, input_size, output_buffer + sizeof(*header), output_size - sizeof(*header), processed);

    if (PA_LIKELY(written > 0)) {
        header = (struct rtp_header *) output_buffer;
        pa_zero(*header);
        header->v = 2;
        header->pt = 96;
        header->sequence_number = htons(info->seq_num++);
        header->timestamp = htonl(timestamp);
        header->ssrc = htonl(1);
        written += sizeof(*header);
    }

    return written;
}

static size_t decode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct rtp_header *header;
    size_t written;

    if (PA_UNLIKELY(input_size < sizeof(*header))) {
        *processed = 0;
        return 0;
    }

    header = (struct rtp_header *) input_buffer;
    written = gst_transcode_buffer(codec_info, input_buffer + sizeof(*header), input_size - sizeof(*header), output_buffer, output_size, processed);
    *processed += sizeof(*header);
    return written;
}

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_aac = {
    .id = { A2DP_CODEC_MPEG24, 0, 0 },
    .support_backchannel = false,
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities,
    .choose_remote_endpoint = choose_remote_endpoint,
    .fill_capabilities = fill_capabilities,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration,
    .bt_codec = {
        .name = "aac",
        .description = "Advanced Audio Coding (AAC)",
        .init = init,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .encode_buffer = encode_buffer,
        .decode_buffer = decode_buffer,
    },
};
