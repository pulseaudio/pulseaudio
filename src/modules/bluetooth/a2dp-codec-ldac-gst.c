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

    if (!for_encoding)
        return false;

    element_factory = gst_element_factory_find("ldacenc");
    if (element_factory == NULL) {
        pa_log_info("LDAC encoder element `ldacenc` not found");
        return false;
    }
    gst_object_unref(element_factory);

    return true;
}

static bool can_accept_capabilities_common(const a2dp_ldac_t *capabilities, uint32_t vendor_id, uint16_t codec_id) {
    if (A2DP_GET_VENDOR_ID(capabilities->info) != vendor_id || A2DP_GET_CODEC_ID(capabilities->info) != codec_id)
        return false;

    if (!(capabilities->frequency & (LDAC_SAMPLING_FREQ_44100 | LDAC_SAMPLING_FREQ_48000 |
                                     LDAC_SAMPLING_FREQ_88200 | LDAC_SAMPLING_FREQ_96000)))
        return false;

    if (!(capabilities->channel_mode & LDAC_CHANNEL_MODE_STEREO))
        return false;

    return true;
}

static bool can_accept_capabilities(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding) {
    const a2dp_ldac_t *capabilities = (const a2dp_ldac_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities))
        return false;

    return can_accept_capabilities_common(capabilities, LDAC_VENDOR_ID, LDAC_CODEC_ID);
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

static void fill_capabilities_common(a2dp_ldac_t *capabilities, uint32_t vendor_id, uint16_t codec_id) {
    capabilities->info = A2DP_SET_VENDOR_ID_CODEC_ID(vendor_id, codec_id);
    capabilities->channel_mode = LDAC_CHANNEL_MODE_STEREO;
    capabilities->frequency = LDAC_SAMPLING_FREQ_44100 | LDAC_SAMPLING_FREQ_48000 |
                              LDAC_SAMPLING_FREQ_88200 | LDAC_SAMPLING_FREQ_96000;
}

static uint8_t fill_capabilities(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_ldac_t *capabilities = (a2dp_ldac_t *) capabilities_buffer;

    pa_zero(*capabilities);
    fill_capabilities_common(capabilities, LDAC_VENDOR_ID, LDAC_CODEC_ID);
    return sizeof(*capabilities);
}

static bool is_configuration_valid(const uint8_t *config_buffer, uint8_t config_size) {
    const a2dp_ldac_t *config = (const a2dp_ldac_t *) config_buffer;

    if (config_size != sizeof(*config)) {
        pa_log_error("Invalid size of config buffer");
        return false;
    }

    if (A2DP_GET_VENDOR_ID(config->info) != LDAC_VENDOR_ID || A2DP_GET_CODEC_ID(config->info) != LDAC_CODEC_ID) {
        pa_log_error("Invalid vendor codec information in configuration");
        return false;
    }

    if (config->frequency != LDAC_SAMPLING_FREQ_44100 && config->frequency != LDAC_SAMPLING_FREQ_48000 &&
        config->frequency != LDAC_SAMPLING_FREQ_88200 && config->frequency != LDAC_SAMPLING_FREQ_96000) {
        pa_log_error("Invalid sampling frequency in configuration");
        return false;
    }

    if (config->channel_mode != LDAC_CHANNEL_MODE_STEREO) {
        pa_log_error("Invalid channel mode in configuration");
        return false;
    }

    return true;
}

static int fill_preferred_configuration_common(const pa_sample_spec *default_sample_spec, const a2dp_ldac_t *capabilities, a2dp_ldac_t *config, uint32_t vendor_id, uint16_t codec_id) {
    int i;

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } freq_table[] = {
        { 44100U, LDAC_SAMPLING_FREQ_44100 },
        { 48000U, LDAC_SAMPLING_FREQ_48000 },
        { 88200U, LDAC_SAMPLING_FREQ_88200 },
        { 96000U, LDAC_SAMPLING_FREQ_96000 }
    };

    if (A2DP_GET_VENDOR_ID(capabilities->info) != LDAC_VENDOR_ID || A2DP_GET_CODEC_ID(capabilities->info) != LDAC_CODEC_ID) {
        pa_log_error("No supported vendor codec information");
        return -1;
    }

    config->info = A2DP_SET_VENDOR_ID_CODEC_ID(vendor_id, codec_id);

    if (!(capabilities->channel_mode & LDAC_CHANNEL_MODE_STEREO)) {
        pa_log_error("No supported channel modes");
        return -1;
    }

    config->channel_mode = LDAC_CHANNEL_MODE_STEREO;

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
    a2dp_ldac_t *config = (a2dp_ldac_t *) config_buffer;
    const a2dp_ldac_t *capabilities = (const a2dp_ldac_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities)) {
        pa_log_error("Invalid size of capabilities buffer");
        return 0;
    }

    pa_zero(*config);

    if (fill_preferred_configuration_common(default_sample_spec, capabilities, config, LDAC_VENDOR_ID, LDAC_CODEC_ID) < 0)
        return 0;

    return sizeof(*config);
}

GstElement *gst_init_ldac(struct gst_info *info, pa_sample_spec *ss, bool for_encoding) {
    GstElement *bin;
    GstElement *enc;
    GstPad *pad;

    if (!for_encoding) {
        pa_log_error("LDAC does not support decoding");
        return NULL;
    }

    ss->format = PA_SAMPLE_FLOAT32LE;

    switch (info->a2dp_codec_t.ldac_config->frequency) {
        case LDAC_SAMPLING_FREQ_44100:
            ss->rate = 44100u;
            break;
        case LDAC_SAMPLING_FREQ_48000:
            ss->rate = 48000u;
            break;
        case LDAC_SAMPLING_FREQ_88200:
            ss->rate = 88200;
            break;
        case LDAC_SAMPLING_FREQ_96000:
            ss->rate = 96000;
            break;
        default:
            pa_log_error("LDAC invalid frequency %d", info->a2dp_codec_t.ldac_config->frequency);
            goto fail;
    }

    switch (info->a2dp_codec_t.ldac_config->channel_mode) {
        case LDAC_CHANNEL_MODE_STEREO:
            ss->channels = 2;
            break;
        case LDAC_CHANNEL_MODE_MONO:
            ss->channels = 1;
            break;
        case LDAC_CHANNEL_MODE_DUAL:
            ss->channels = 1;
            break;
        default:
            pa_log_error("LDAC invalid channel mode %d", info->a2dp_codec_t.ldac_config->channel_mode);
            goto fail;
    }

    enc = gst_element_factory_make("ldacenc", "ldac_enc");
    if (!enc) {
        pa_log_error("Could not create LDAC encoder element");
        goto fail;
    }

    switch (info->codec_type) {
        case LDAC_EQMID_HQ:
            g_object_set(enc, "eqmid", 0, NULL);
            break;
        case LDAC_EQMID_SQ:
            g_object_set(enc, "eqmid", 1, NULL);
            break;
        case LDAC_EQMID_MQ:
            g_object_set(enc, "eqmid", 2, NULL);
            break;
        default:
            goto fail;
    }

    bin = gst_bin_new("ldac_enc_bin");
    pa_assert(bin);

    gst_bin_add_many(GST_BIN(bin), enc, NULL);

    pad = gst_element_get_static_pad(enc, "sink");
    pa_assert_se(gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad)));
    gst_object_unref(GST_OBJECT(pad));

    pad = gst_element_get_static_pad(enc, "src");
    pa_assert_se(gst_element_add_pad(bin, gst_ghost_pad_new("src", pad)));
    gst_object_unref(GST_OBJECT(pad));

    return bin;

fail:
    pa_log_error("LDAC encoder initialisation failed");
    return NULL;
}

static void *init_common(enum a2dp_codec_type codec_type, bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    GstElement *bin;
    struct gst_info *info = NULL;

    if (!for_encoding) {
        pa_log_error("LDAC decoder not supported");
        return NULL;
    }

    info = pa_xnew0(struct gst_info, 1);
    pa_assert(info);

    info->core = core;
    info->ss = sample_spec;

    info->codec_type = codec_type;
    info->a2dp_codec_t.ldac_config = (const a2dp_ldac_t *) config_buffer;
    pa_assert(config_size == sizeof(*(info->a2dp_codec_t.ldac_config)));

    if (!(bin = gst_init_ldac(info, sample_spec, for_encoding)))
        goto fail;

    if (!gst_codec_init(info, for_encoding, bin))
        goto fail;

    return info;

fail:
    if (info)
        pa_xfree(info);

    return NULL;
}

static void *init_hq(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    return init_common(LDAC_EQMID_HQ, for_encoding, for_backchannel, config_buffer, config_size, sample_spec, core);
}

static void *init_sq(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    return init_common(LDAC_EQMID_SQ, for_encoding, for_backchannel, config_buffer, config_size, sample_spec, core);
}

static void *init_mq(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    return init_common(LDAC_EQMID_MQ, for_encoding, for_backchannel, config_buffer, config_size, sample_spec, core);
}

static void deinit(void *codec_info) {
    return gst_codec_deinit(codec_info);
}

static int reset(void *codec_info) {
    return 0;
}

static uint32_t get_ldac_num_samples(void *codec_info) {
    struct gst_info *info = (struct gst_info *) codec_info;

    switch (info->a2dp_codec_t.ldac_config->frequency) {
        case LDAC_SAMPLING_FREQ_44100:
        case LDAC_SAMPLING_FREQ_48000:
            return 128;
            break;
        case LDAC_SAMPLING_FREQ_88200:
        case LDAC_SAMPLING_FREQ_96000:
            return 256;
            break;
        default:
            break;
    }

    return 128;
}

static uint8_t get_ldac_num_frames(void *codec_info, enum a2dp_codec_type codec_type) {
    struct gst_info *info = (struct gst_info *) codec_info;
    uint8_t channels;

    switch (info->a2dp_codec_t.ldac_config->channel_mode) {
        case LDAC_CHANNEL_MODE_STEREO:
            channels = 2;
            break;
        case LDAC_CHANNEL_MODE_MONO:
        case LDAC_CHANNEL_MODE_DUAL:
            channels = 1;
            break;
        default:
            break;
    }

    switch (codec_type) {
        case LDAC_EQMID_HQ:
            return 4 / channels;
        case LDAC_EQMID_SQ:
            return 6 / channels;
        case LDAC_EQMID_MQ:
            return 12 / channels;
        default:
            break;
    }

    return 6 / channels;
}

static size_t get_block_size(void *codec_info, size_t link_mtu) {
    struct gst_info *info = (struct gst_info *) codec_info;

    return get_ldac_num_samples(codec_info) * get_ldac_num_frames(codec_info, info->codec_type) * pa_frame_size(info->ss);
}

static size_t get_encoded_block_size(void *codec_info, size_t input_size) {
    /* encoded block size is not exactly known, report input_size */
    return input_size;
}

static size_t reduce_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    return 0;
}

static size_t encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct gst_info *info = (struct gst_info *) codec_info;
    struct rtp_header *header;
    struct rtp_payload *payload;
    size_t written;

    if (PA_UNLIKELY(output_size < sizeof(*header) + sizeof(*payload))) {
        *processed = 0;
        return 0;
    }

    written = gst_transcode_buffer(codec_info, timestamp, input_buffer, input_size, output_buffer + sizeof(*header) + sizeof(*payload), output_size - sizeof(*header) - sizeof(*payload), processed);
    if (PA_UNLIKELY(*processed != input_size))
        pa_log_error("LDAC encoding error");

    if (PA_LIKELY(written > 0)) {
        header = (struct rtp_header *) output_buffer;
        pa_zero(*header);
        header->v = 2;
        header->pt = 96;
        header->sequence_number = htons(info->seq_num++);
        header->timestamp = htonl(timestamp);
        header->ssrc = htonl(1);
        payload = (struct rtp_payload*) (output_buffer + sizeof(*header));
        payload->frame_count = get_ldac_num_frames(codec_info, info->codec_type);
        written += sizeof(*header) + sizeof(*payload);
    }

    return written;
}

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_ldac_eqmid_hq = {
    .id = { A2DP_CODEC_VENDOR, LDAC_VENDOR_ID, LDAC_CODEC_ID },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities,
    .choose_remote_endpoint = choose_remote_endpoint,
    .fill_capabilities = fill_capabilities,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration,
    .bt_codec = {
        .name = "ldac_hq",
        .description = "LDAC (High Quality)",
        .init = init_hq,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .encode_buffer = encode_buffer,
    },
};

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_ldac_eqmid_sq = {
    .id = { A2DP_CODEC_VENDOR, LDAC_VENDOR_ID, LDAC_CODEC_ID },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities,
    .choose_remote_endpoint = choose_remote_endpoint,
    .fill_capabilities = fill_capabilities,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration,
    .bt_codec = {
        .name = "ldac_sq",
        .description = "LDAC (Standard Quality)",
        .init = init_sq,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .encode_buffer = encode_buffer,
    },
};

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_ldac_eqmid_mq = {
    .id = { A2DP_CODEC_VENDOR, LDAC_VENDOR_ID, LDAC_CODEC_ID },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities,
    .choose_remote_endpoint = choose_remote_endpoint,
    .fill_capabilities = fill_capabilities,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration,
    .bt_codec = {
        .name = "ldac_mq",
        .description = "LDAC (Mobile Quality)",
        .init = init_mq,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .encode_buffer = encode_buffer,
    },
};
