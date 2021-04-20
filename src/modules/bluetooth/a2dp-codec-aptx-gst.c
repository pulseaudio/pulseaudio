/***
  This file is part of PulseAudio.

  Copyright 2020 Sanchayan Maity <sanchayan@asymptotic.io>

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
        element_factory = gst_element_factory_find("openaptxenc");
        if (element_factory == NULL) {
            pa_log_info("aptX encoder element `openaptxenc` not found");
            return false;
        }

        gst_object_unref(element_factory);
    } else {
        element_factory = gst_element_factory_find("openaptxdec");
        if (element_factory == NULL) {
            pa_log_info("aptX decoder element `openaptxdec` not found");
            return false;
        }

        gst_object_unref(element_factory);
    }

    return true;
}

static bool can_accept_capabilities_common(const a2dp_aptx_t *capabilities, uint32_t vendor_id, uint16_t codec_id) {
    if (A2DP_GET_VENDOR_ID(capabilities->info) != vendor_id || A2DP_GET_CODEC_ID(capabilities->info) != codec_id)
        return false;

    if (!(capabilities->frequency & (APTX_SAMPLING_FREQ_16000 | APTX_SAMPLING_FREQ_32000 |
                                     APTX_SAMPLING_FREQ_44100 | APTX_SAMPLING_FREQ_48000)))
        return false;

    if (!(capabilities->channel_mode & APTX_CHANNEL_MODE_STEREO))
        return false;

    return true;
}

static bool can_accept_capabilities(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding) {
    const a2dp_aptx_t *capabilities = (const a2dp_aptx_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities))
        return false;

    return can_accept_capabilities_common(capabilities, APTX_VENDOR_ID, APTX_CODEC_ID);
}

static bool can_accept_capabilities_hd(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding) {
    const a2dp_aptx_hd_t *capabilities = (const a2dp_aptx_hd_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities))
        return false;

    return can_accept_capabilities_common(&capabilities->aptx, APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID);
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

static const char *choose_remote_endpoint_hd(const pa_hashmap *capabilities_hashmap, const pa_sample_spec *default_sample_spec, bool for_encoding) {
    const pa_a2dp_codec_capabilities *a2dp_capabilities;
    const char *key;
    void *state;

    /* There is no preference, just choose random valid entry */
    PA_HASHMAP_FOREACH_KV(key, a2dp_capabilities, capabilities_hashmap, state) {
        if (can_accept_capabilities_hd(a2dp_capabilities->buffer, a2dp_capabilities->size, for_encoding))
            return key;
    }

    return NULL;
}

static void fill_capabilities_common(a2dp_aptx_t *capabilities, uint32_t vendor_id, uint16_t codec_id) {
    capabilities->info = A2DP_SET_VENDOR_ID_CODEC_ID(vendor_id, codec_id);
    capabilities->channel_mode = APTX_CHANNEL_MODE_STEREO;
    capabilities->frequency = APTX_SAMPLING_FREQ_16000 | APTX_SAMPLING_FREQ_32000 |
                              APTX_SAMPLING_FREQ_44100 | APTX_SAMPLING_FREQ_48000;
}

static uint8_t fill_capabilities(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_aptx_t *capabilities = (a2dp_aptx_t *) capabilities_buffer;

    pa_zero(*capabilities);
    fill_capabilities_common(capabilities, APTX_VENDOR_ID, APTX_CODEC_ID);
    return sizeof(*capabilities);
}

static uint8_t fill_capabilities_hd(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_aptx_hd_t *capabilities = (a2dp_aptx_hd_t *) capabilities_buffer;

    pa_zero(*capabilities);
    fill_capabilities_common(&capabilities->aptx, APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID);
    return sizeof(*capabilities);
}

static bool is_configuration_valid_common(const a2dp_aptx_t *config, uint32_t vendor_id, uint16_t codec_id) {
    if (A2DP_GET_VENDOR_ID(config->info) != vendor_id || A2DP_GET_CODEC_ID(config->info) != codec_id) {
        pa_log_error("Invalid vendor codec information in configuration");
        return false;
    }

    if (config->frequency != APTX_SAMPLING_FREQ_16000 && config->frequency != APTX_SAMPLING_FREQ_32000 &&
        config->frequency != APTX_SAMPLING_FREQ_44100 && config->frequency != APTX_SAMPLING_FREQ_48000) {
        pa_log_error("Invalid sampling frequency in configuration");
        return false;
    }

    if (config->channel_mode != APTX_CHANNEL_MODE_STEREO) {
        pa_log_error("Invalid channel mode in configuration");
        return false;
    }

    return true;
}

static bool is_configuration_valid(const uint8_t *config_buffer, uint8_t config_size) {
    const a2dp_aptx_t *config = (const a2dp_aptx_t *) config_buffer;

    if (config_size != sizeof(*config)) {
        pa_log_error("Invalid size of config buffer");
        return false;
    }

    return is_configuration_valid_common(config, APTX_VENDOR_ID, APTX_CODEC_ID);
}

static bool is_configuration_valid_hd(const uint8_t *config_buffer, uint8_t config_size) {
    const a2dp_aptx_hd_t *config = (const a2dp_aptx_hd_t *) config_buffer;

    if (config_size != sizeof(*config)) {
        pa_log_error("Invalid size of config buffer");
        return false;
    }

    return is_configuration_valid_common(&config->aptx, APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID);
}

static int fill_preferred_configuration_common(const pa_sample_spec *default_sample_spec, const a2dp_aptx_t *capabilities, a2dp_aptx_t *config, uint32_t vendor_id, uint16_t codec_id) {
    int i;

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } freq_table[] = {
        { 16000U, APTX_SAMPLING_FREQ_16000 },
        { 32000U, APTX_SAMPLING_FREQ_32000 },
        { 44100U, APTX_SAMPLING_FREQ_44100 },
        { 48000U, APTX_SAMPLING_FREQ_48000 }
    };

    if (A2DP_GET_VENDOR_ID(capabilities->info) != vendor_id || A2DP_GET_CODEC_ID(capabilities->info) != codec_id) {
        pa_log_error("No supported vendor codec information");
        return -1;
    }

    config->info = A2DP_SET_VENDOR_ID_CODEC_ID(vendor_id, codec_id);

    if (!(capabilities->channel_mode & APTX_CHANNEL_MODE_STEREO)) {
        pa_log_error("No supported channel modes");
        return -1;
    }

    config->channel_mode = APTX_CHANNEL_MODE_STEREO;

    /* Find the lowest freq that is at least as high as the requested sampling rate */
    for (i = 0; (unsigned) i < PA_ELEMENTSOF(freq_table); i++) {
        if (freq_table[i].rate >= default_sample_spec->rate && (capabilities->frequency & freq_table[i].cap)) {
            config->frequency = freq_table[i].cap;
            break;
        }
    }

    if ((unsigned) i == PA_ELEMENTSOF(freq_table)) {
        for (--i; i >= 0; i--) {
            if (capabilities->frequency & freq_table[i].cap) {
                config->frequency = freq_table[i].cap;
                break;
            }
        }

        if (i < 0) {
            pa_log_error("Not suitable sample rate");
            return false;
        }
    }

    return 0;
}

static uint8_t fill_preferred_configuration(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_aptx_t *config = (a2dp_aptx_t *) config_buffer;
    const a2dp_aptx_t *capabilities = (const a2dp_aptx_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities)) {
        pa_log_error("Invalid size of capabilities buffer");
        return 0;
    }

    pa_zero(*config);

    if (fill_preferred_configuration_common(default_sample_spec, capabilities, config, APTX_VENDOR_ID, APTX_CODEC_ID) < 0)
        return 0;

    return sizeof(*config);
}

static uint8_t fill_preferred_configuration_hd(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_aptx_hd_t *config = (a2dp_aptx_hd_t *) config_buffer;
    const a2dp_aptx_hd_t *capabilities = (const a2dp_aptx_hd_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities)) {
        pa_log_error("Invalid size of capabilities buffer");
        return 0;
    }

    pa_zero(*config);

    if (fill_preferred_configuration_common(default_sample_spec, &capabilities->aptx, &config->aptx, APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID) < 0)
        return 0;

    return sizeof(*config);
}

GstElement *gst_init_aptx(struct gst_info *info, pa_sample_spec *ss, bool for_encoding) {
    GstElement *bin, *sink, *src, *capsf;
    GstCaps *caps;
    GstPad *pad;
    const char *aptx_codec_media_type;

    ss->format = PA_SAMPLE_S24LE;

    if (info->codec_type == APTX_HD) {
        switch (info->a2dp_codec_t.aptx_hd_config->aptx.frequency) {
            case APTX_SAMPLING_FREQ_16000:
                ss->rate = 16000u;
                break;
            case APTX_SAMPLING_FREQ_32000:
                ss->rate = 32000u;
                break;
            case APTX_SAMPLING_FREQ_44100:
                ss->rate = 44100u;
                break;
            case APTX_SAMPLING_FREQ_48000:
                ss->rate = 48000u;
                break;
            default:
                pa_log_error("aptX HD invalid frequency %d", info->a2dp_codec_t.aptx_hd_config->aptx.frequency);
                goto fail;
        }

        switch (info->a2dp_codec_t.aptx_hd_config->aptx.channel_mode) {
            case APTX_CHANNEL_MODE_STEREO:
                ss->channels = 2;
                break;
            default:
                pa_log_error("aptX HD invalid channel mode %d", info->a2dp_codec_t.aptx_hd_config->aptx.frequency);
                goto fail;
        }
    } else {
        switch (info->a2dp_codec_t.aptx_config->frequency) {
            case APTX_SAMPLING_FREQ_16000:
                ss->rate = 16000u;
                break;
            case APTX_SAMPLING_FREQ_32000:
                ss->rate = 32000u;
                break;
            case APTX_SAMPLING_FREQ_44100:
                ss->rate = 44100u;
                break;
            case APTX_SAMPLING_FREQ_48000:
                ss->rate = 48000u;
                break;
            default:
                pa_log_error("aptX invalid frequency %d", info->a2dp_codec_t.aptx_config->frequency);
                goto fail;
        }

        switch (info->a2dp_codec_t.aptx_config->channel_mode) {
            case APTX_CHANNEL_MODE_STEREO:
                ss->channels = 2;
                break;
            default:
                pa_log_error("aptX invalid channel mode %d", info->a2dp_codec_t.aptx_config->frequency);
                goto fail;
        }
    }

    aptx_codec_media_type = info->codec_type == APTX_HD ? "audio/aptx-hd" : "audio/aptx";

    capsf = gst_element_factory_make("capsfilter", "aptx_capsfilter");
    if (!capsf) {
        pa_log_error("Could not create aptX capsfilter element");
        goto fail;
    }

    caps = gst_caps_new_simple(aptx_codec_media_type,
            "rate", G_TYPE_INT, (int) ss->rate,
            "channels", G_TYPE_INT, (int) ss->channels,
            NULL);
    g_object_set(capsf, "caps", caps, NULL);
    gst_caps_unref(caps);

    if (for_encoding) {
        sink = gst_element_factory_make("openaptxenc", "aptx_encoder");
        src = capsf;

        if (sink == NULL) {
            pa_log_error("Could not create aptX encoder element");
            goto fail_enc_dec;
        }

        bin = gst_bin_new("aptx_enc_bin");
    } else {
        sink = capsf;
        src = gst_element_factory_make("openaptxdec", "aptx_decoder");

        if (src == NULL) {
            pa_log_error("Could not create aptX decoder element");
            goto fail_enc_dec;
        }

        bin = gst_bin_new("aptx_dec_bin");
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
    pa_log_error("aptX initialisation failed");
    return NULL;
}

static void *init_common(enum a2dp_codec_type codec_type, bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    GstElement *bin;
    struct gst_info *info = NULL;

    info = pa_xnew0(struct gst_info, 1);
    pa_assert(info);

    info->core = core;
    info->ss = sample_spec;

    if (codec_type == APTX) {
        info->codec_type = APTX;
        info->a2dp_codec_t.aptx_config = (const a2dp_aptx_t *) config_buffer;
        pa_assert(config_size == sizeof(*(info->a2dp_codec_t.aptx_config)));
    } else if (codec_type == APTX_HD) {
        info->codec_type = APTX_HD;
        info->a2dp_codec_t.aptx_hd_config = (const a2dp_aptx_hd_t *) config_buffer;
        pa_assert(config_size == sizeof(*(info->a2dp_codec_t.aptx_hd_config)));
    } else
        pa_assert_not_reached();

    if (!(bin = gst_init_aptx(info, sample_spec, for_encoding)))
        goto fail;

    if (!gst_codec_init(info, for_encoding, bin))
        goto fail;

    return info;

fail:
    if (info)
        pa_xfree(info);

    return NULL;
}

static void *init(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    return init_common(APTX, for_encoding, for_backchannel, config_buffer, config_size, sample_spec, core);
}

static void *init_hd(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    return init_common(APTX_HD, for_encoding, for_backchannel, config_buffer, config_size, sample_spec, core);
}

static void deinit(void *codec_info) {
    return gst_codec_deinit(codec_info);
}

static int reset(void *codec_info) {
    return 0;
}

static int reset_hd(void *codec_info) {
    struct gst_info *info = (struct gst_info *) codec_info;

    info->seq_num = 0;

    return 0;
}

static size_t get_block_size(void *codec_info, size_t link_mtu) {
    /* aptX compression ratio is 6:1 and we need to process one aptX frame (4 bytes) at once */
    size_t frame_count = (link_mtu / 4);

    return frame_count * 4 * 6;
}

static size_t get_encoded_block_size(void *codec_info, size_t input_size) {
    /* input size should be aligned to codec input block size */
    pa_assert_fp(input_size % (4 * 6) == 0);

    return (input_size / (4 * 6)) * 4;
}

static size_t get_block_size_hd(void *codec_info, size_t link_mtu) {
    /* aptX HD compression ratio is 4:1 and we need to process one aptX HD frame (6 bytes) at once, plus aptX HD frames are encapsulated in RTP */
    size_t rtp_size = sizeof(struct rtp_header);
    size_t frame_count = (link_mtu - rtp_size) / 6;

    return frame_count * 6 * 4;
}

static size_t get_encoded_block_size_hd(void *codec_info, size_t input_size) {
    size_t rtp_size = sizeof(struct rtp_header);

    /* input size should be aligned to codec input block size */
    pa_assert_fp(input_size % (4 * 6) == 0);

    return (input_size / (4 * 6)) * 6 + rtp_size;
}

static size_t reduce_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    return 0;
}

static size_t encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    size_t written;

    written = gst_transcode_buffer(codec_info, timestamp, input_buffer, input_size, output_buffer, output_size, processed);
    if (PA_UNLIKELY(*processed == 0 || *processed != input_size))
        pa_log_error("aptX encoding error");

    return written;
}

static size_t encode_buffer_hd(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct gst_info *info = (struct gst_info *) codec_info;
    struct rtp_header *header;
    size_t written;

    if (PA_UNLIKELY(output_size < sizeof(*header))) {
        *processed = 0;
        return 0;
    }

    written = encode_buffer(codec_info, timestamp, input_buffer, input_size, output_buffer + sizeof(*header), output_size - sizeof(*header), processed);

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
    size_t written;

    written = gst_transcode_buffer(codec_info, -1, input_buffer, input_size, output_buffer, output_size, processed);

    /* Due to aptX latency, aptx_decode starts filling output buffer after 90 input samples.
     * If input buffer contains less than 90 samples, aptx_decode returns zero (=no output)
     * but set *processed to non zero as input samples were processed. So do not check for
     * return value of aptx_decode, zero is valid. Decoding error is indicating by fact that
     * not all input samples were processed. */
    if (PA_UNLIKELY(*processed != input_size))
        pa_log_error("aptX decoding error");

    return written;
}

static size_t decode_buffer_hd(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct rtp_header *header;
    size_t written;

    if (PA_UNLIKELY(input_size < sizeof(*header))) {
        *processed = 0;
        return 0;
    }

    header = (struct rtp_header *) input_buffer;
    written = decode_buffer(codec_info, input_buffer + sizeof(*header), input_size - sizeof(*header), output_buffer, output_size, processed);
    *processed += sizeof(*header);
    return written;
}

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_aptx = {
    .id = { A2DP_CODEC_VENDOR, APTX_VENDOR_ID, APTX_CODEC_ID },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities,
    .choose_remote_endpoint = choose_remote_endpoint,
    .fill_capabilities = fill_capabilities,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration,
    .bt_codec = {
        .name = "aptx",
        .description = "aptX",
        .init = init,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .encode_buffer = encode_buffer,
        .decode_buffer = decode_buffer,
    },
};

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_aptx_hd = {
    .id = { A2DP_CODEC_VENDOR, APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities_hd,
    .choose_remote_endpoint = choose_remote_endpoint_hd,
    .fill_capabilities = fill_capabilities_hd,
    .is_configuration_valid = is_configuration_valid_hd,
    .fill_preferred_configuration = fill_preferred_configuration_hd,
    .bt_codec = {
        .name = "aptx_hd",
        .description = "aptX HD",
        .init = init_hd,
        .deinit = deinit,
        .reset = reset_hd,
        .get_read_block_size = get_block_size_hd,
        .get_write_block_size = get_block_size_hd,
        .get_encoded_block_size = get_encoded_block_size_hd,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .encode_buffer = encode_buffer_hd,
        .decode_buffer = decode_buffer_hd,
    },
};
