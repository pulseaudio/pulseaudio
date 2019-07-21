/***
  This file is part of PulseAudio.

  Copyright 2018-2019 Pali Rohár <pali.rohar@gmail.com>

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

#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/once.h>
#include <pulse/sample.h>
#include <pulse/xmalloc.h>

#include <arpa/inet.h>

#include <sbc/sbc.h>

#include "a2dp-codecs.h"
#include "a2dp-codec-api.h"
#include "rtp.h"

#define SBC_BITPOOL_DEC_LIMIT 32
#define SBC_BITPOOL_DEC_STEP 5

struct sbc_info {
    sbc_t sbc;                           /* Codec data */
    size_t codesize, frame_length;       /* SBC Codesize, frame_length. We simply cache those values here */
    uint16_t seq_num;                    /* Cumulative packet sequence */
    uint8_t frequency;
    uint8_t blocks;
    uint8_t subbands;
    uint8_t mode;
    uint8_t allocation;
    uint8_t initial_bitpool;
    uint8_t min_bitpool;
    uint8_t max_bitpool;
};

static bool can_accept_capabilities(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding) {
    const a2dp_sbc_t *capabilities = (const a2dp_sbc_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities))
        return false;

    if (!(capabilities->frequency & (SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_32000 | SBC_SAMPLING_FREQ_44100 | SBC_SAMPLING_FREQ_48000)))
        return false;

    if (!(capabilities->channel_mode & (SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_DUAL_CHANNEL | SBC_CHANNEL_MODE_STEREO | SBC_CHANNEL_MODE_JOINT_STEREO)))
        return false;

    if (!(capabilities->allocation_method & (SBC_ALLOCATION_SNR | SBC_ALLOCATION_LOUDNESS)))
        return false;

    if (!(capabilities->subbands & (SBC_SUBBANDS_4 | SBC_SUBBANDS_8)))
        return false;

    if (!(capabilities->block_length & (SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8 | SBC_BLOCK_LENGTH_12 | SBC_BLOCK_LENGTH_16)))
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
    a2dp_sbc_t *capabilities = (a2dp_sbc_t *) capabilities_buffer;

    pa_zero(*capabilities);

    capabilities->channel_mode = SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_DUAL_CHANNEL | SBC_CHANNEL_MODE_STEREO |
                                 SBC_CHANNEL_MODE_JOINT_STEREO;
    capabilities->frequency = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_32000 | SBC_SAMPLING_FREQ_44100 |
                              SBC_SAMPLING_FREQ_48000;
    capabilities->allocation_method = SBC_ALLOCATION_SNR | SBC_ALLOCATION_LOUDNESS;
    capabilities->subbands = SBC_SUBBANDS_4 | SBC_SUBBANDS_8;
    capabilities->block_length = SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8 | SBC_BLOCK_LENGTH_12 | SBC_BLOCK_LENGTH_16;
    capabilities->min_bitpool = SBC_MIN_BITPOOL;
    capabilities->max_bitpool = SBC_BITPOOL_HQ_JOINT_STEREO_44100;

    return sizeof(*capabilities);
}

static bool is_configuration_valid(const uint8_t *config_buffer, uint8_t config_size) {
    const a2dp_sbc_t *config = (const a2dp_sbc_t *) config_buffer;

    if (config_size != sizeof(*config)) {
        pa_log_error("Invalid size of config buffer");
        return false;
    }

    if (config->frequency != SBC_SAMPLING_FREQ_16000 && config->frequency != SBC_SAMPLING_FREQ_32000 &&
        config->frequency != SBC_SAMPLING_FREQ_44100 && config->frequency != SBC_SAMPLING_FREQ_48000) {
        pa_log_error("Invalid sampling frequency in configuration");
        return false;
    }

    if (config->channel_mode != SBC_CHANNEL_MODE_MONO && config->channel_mode != SBC_CHANNEL_MODE_DUAL_CHANNEL &&
        config->channel_mode != SBC_CHANNEL_MODE_STEREO && config->channel_mode != SBC_CHANNEL_MODE_JOINT_STEREO) {
        pa_log_error("Invalid channel mode in configuration");
        return false;
    }

    if (config->allocation_method != SBC_ALLOCATION_SNR && config->allocation_method != SBC_ALLOCATION_LOUDNESS) {
        pa_log_error("Invalid allocation method in configuration");
        return false;
    }

    if (config->subbands != SBC_SUBBANDS_4 && config->subbands != SBC_SUBBANDS_8) {
        pa_log_error("Invalid SBC subbands in configuration");
        return false;
    }

    if (config->block_length != SBC_BLOCK_LENGTH_4 && config->block_length != SBC_BLOCK_LENGTH_8 &&
        config->block_length != SBC_BLOCK_LENGTH_12 && config->block_length != SBC_BLOCK_LENGTH_16) {
        pa_log_error("Invalid block length in configuration");
        return false;
    }

    if (config->min_bitpool > config->max_bitpool) {
        pa_log_error("Invalid bitpool in configuration");
        return false;
    }

    return true;
}

static uint8_t default_bitpool(uint8_t freq, uint8_t mode) {
    /* These bitpool values were chosen based on the A2DP spec recommendation */
    switch (freq) {
        case SBC_SAMPLING_FREQ_16000:
        case SBC_SAMPLING_FREQ_32000:
            switch (mode) {
                case SBC_CHANNEL_MODE_MONO:
                case SBC_CHANNEL_MODE_DUAL_CHANNEL:
                case SBC_CHANNEL_MODE_STEREO:
                case SBC_CHANNEL_MODE_JOINT_STEREO:
                    return SBC_BITPOOL_HQ_JOINT_STEREO_44100;
            }
            break;

        case SBC_SAMPLING_FREQ_44100:
            switch (mode) {
                case SBC_CHANNEL_MODE_MONO:
                case SBC_CHANNEL_MODE_DUAL_CHANNEL:
                    return SBC_BITPOOL_HQ_MONO_44100;

                case SBC_CHANNEL_MODE_STEREO:
                case SBC_CHANNEL_MODE_JOINT_STEREO:
                    return SBC_BITPOOL_HQ_JOINT_STEREO_44100;
            }
            break;

        case SBC_SAMPLING_FREQ_48000:
            switch (mode) {
                case SBC_CHANNEL_MODE_MONO:
                case SBC_CHANNEL_MODE_DUAL_CHANNEL:
                    return SBC_BITPOOL_HQ_MONO_48000;

                case SBC_CHANNEL_MODE_STEREO:
                case SBC_CHANNEL_MODE_JOINT_STEREO:
                    return SBC_BITPOOL_HQ_JOINT_STEREO_48000;
            }
            break;
    }

    pa_assert_not_reached();
}

static uint8_t fill_preferred_configuration(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_sbc_t *config = (a2dp_sbc_t *) config_buffer;
    const a2dp_sbc_t *capabilities = (const a2dp_sbc_t *) capabilities_buffer;
    int i;

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } freq_table[] = {
        { 16000U, SBC_SAMPLING_FREQ_16000 },
        { 32000U, SBC_SAMPLING_FREQ_32000 },
        { 44100U, SBC_SAMPLING_FREQ_44100 },
        { 48000U, SBC_SAMPLING_FREQ_48000 }
    };

    if (capabilities_size != sizeof(*capabilities)) {
        pa_log_error("Invalid size of capabilities buffer");
        return 0;
    }

    pa_zero(*config);

    /* Find the lowest freq that is at least as high as the requested sampling rate */
    for (i = 0; (unsigned) i < PA_ELEMENTSOF(freq_table); i++)
        if (freq_table[i].rate >= default_sample_spec->rate && (capabilities->frequency & freq_table[i].cap)) {
            config->frequency = freq_table[i].cap;
            break;
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
            return 0;
        }
    }

    pa_assert((unsigned) i < PA_ELEMENTSOF(freq_table));

    if (default_sample_spec->channels <= 1) {
        if (capabilities->channel_mode & SBC_CHANNEL_MODE_MONO)
            config->channel_mode = SBC_CHANNEL_MODE_MONO;
        else if (capabilities->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
            config->channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
        else if (capabilities->channel_mode & SBC_CHANNEL_MODE_STEREO)
            config->channel_mode = SBC_CHANNEL_MODE_STEREO;
        else if (capabilities->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
            config->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
        else {
            pa_log_error("No supported channel modes");
            return 0;
        }
    } else {
        if (capabilities->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
            config->channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
        else if (capabilities->channel_mode & SBC_CHANNEL_MODE_STEREO)
            config->channel_mode = SBC_CHANNEL_MODE_STEREO;
        else if (capabilities->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
            config->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
        else if (capabilities->channel_mode & SBC_CHANNEL_MODE_MONO)
            config->channel_mode = SBC_CHANNEL_MODE_MONO;
        else {
            pa_log_error("No supported channel modes");
            return 0;
        }
    }

    if (capabilities->block_length & SBC_BLOCK_LENGTH_16)
        config->block_length = SBC_BLOCK_LENGTH_16;
    else if (capabilities->block_length & SBC_BLOCK_LENGTH_12)
        config->block_length = SBC_BLOCK_LENGTH_12;
    else if (capabilities->block_length & SBC_BLOCK_LENGTH_8)
        config->block_length = SBC_BLOCK_LENGTH_8;
    else if (capabilities->block_length & SBC_BLOCK_LENGTH_4)
        config->block_length = SBC_BLOCK_LENGTH_4;
    else {
        pa_log_error("No supported block lengths");
        return 0;
    }

    if (capabilities->subbands & SBC_SUBBANDS_8)
        config->subbands = SBC_SUBBANDS_8;
    else if (capabilities->subbands & SBC_SUBBANDS_4)
        config->subbands = SBC_SUBBANDS_4;
    else {
        pa_log_error("No supported subbands");
        return 0;
    }

    if (capabilities->allocation_method & SBC_ALLOCATION_LOUDNESS)
        config->allocation_method = SBC_ALLOCATION_LOUDNESS;
    else if (capabilities->allocation_method & SBC_ALLOCATION_SNR)
        config->allocation_method = SBC_ALLOCATION_SNR;
    else {
        pa_log_error("No supported allocation method");
        return 0;
    }

    config->min_bitpool = (uint8_t) PA_MAX(SBC_MIN_BITPOOL, capabilities->min_bitpool);
    config->max_bitpool = (uint8_t) PA_MIN(default_bitpool(config->frequency, config->channel_mode), capabilities->max_bitpool);

    if (config->min_bitpool > config->max_bitpool) {
        pa_log_error("No supported bitpool");
        return 0;
    }

    return sizeof(*config);
}

static void set_params(struct sbc_info *sbc_info) {
    sbc_info->sbc.frequency = sbc_info->frequency;
    sbc_info->sbc.blocks = sbc_info->blocks;
    sbc_info->sbc.subbands = sbc_info->subbands;
    sbc_info->sbc.mode = sbc_info->mode;
    sbc_info->sbc.allocation = sbc_info->allocation;
    sbc_info->sbc.bitpool = sbc_info->initial_bitpool;
    sbc_info->sbc.endian = SBC_LE;

    sbc_info->codesize = sbc_get_codesize(&sbc_info->sbc);
    sbc_info->frame_length = sbc_get_frame_length(&sbc_info->sbc);
}

static void *init(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec) {
    struct sbc_info *sbc_info;
    const a2dp_sbc_t *config = (const a2dp_sbc_t *) config_buffer;
    int ret;

    pa_assert(config_size == sizeof(*config));
    pa_assert(!for_backchannel);

    sbc_info = pa_xnew0(struct sbc_info, 1);

    ret = sbc_init(&sbc_info->sbc, 0);
    if (ret != 0) {
        pa_xfree(sbc_info);
        pa_log_error("SBC initialization failed: %d", ret);
        return NULL;
    }

    sample_spec->format = PA_SAMPLE_S16LE;

    switch (config->frequency) {
        case SBC_SAMPLING_FREQ_16000:
            sbc_info->frequency = SBC_FREQ_16000;
            sample_spec->rate = 16000U;
            break;
        case SBC_SAMPLING_FREQ_32000:
            sbc_info->frequency = SBC_FREQ_32000;
            sample_spec->rate = 32000U;
            break;
        case SBC_SAMPLING_FREQ_44100:
            sbc_info->frequency = SBC_FREQ_44100;
            sample_spec->rate = 44100U;
            break;
        case SBC_SAMPLING_FREQ_48000:
            sbc_info->frequency = SBC_FREQ_48000;
            sample_spec->rate = 48000U;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->channel_mode) {
        case SBC_CHANNEL_MODE_MONO:
            sbc_info->mode = SBC_MODE_MONO;
            sample_spec->channels = 1;
            break;
        case SBC_CHANNEL_MODE_DUAL_CHANNEL:
            sbc_info->mode = SBC_MODE_DUAL_CHANNEL;
            sample_spec->channels = 2;
            break;
        case SBC_CHANNEL_MODE_STEREO:
            sbc_info->mode = SBC_MODE_STEREO;
            sample_spec->channels = 2;
            break;
        case SBC_CHANNEL_MODE_JOINT_STEREO:
            sbc_info->mode = SBC_MODE_JOINT_STEREO;
            sample_spec->channels = 2;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->allocation_method) {
        case SBC_ALLOCATION_SNR:
            sbc_info->allocation = SBC_AM_SNR;
            break;
        case SBC_ALLOCATION_LOUDNESS:
            sbc_info->allocation = SBC_AM_LOUDNESS;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->subbands) {
        case SBC_SUBBANDS_4:
            sbc_info->subbands = SBC_SB_4;
            break;
        case SBC_SUBBANDS_8:
            sbc_info->subbands = SBC_SB_8;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->block_length) {
        case SBC_BLOCK_LENGTH_4:
            sbc_info->blocks = SBC_BLK_4;
            break;
        case SBC_BLOCK_LENGTH_8:
            sbc_info->blocks = SBC_BLK_8;
            break;
        case SBC_BLOCK_LENGTH_12:
            sbc_info->blocks = SBC_BLK_12;
            break;
        case SBC_BLOCK_LENGTH_16:
            sbc_info->blocks = SBC_BLK_16;
            break;
        default:
            pa_assert_not_reached();
    }

    sbc_info->min_bitpool = config->min_bitpool;
    sbc_info->max_bitpool = config->max_bitpool;

    /* Set minimum bitpool for source to get the maximum possible block_size
     * in get_block_size() function. This block_size is length of buffer used
     * for decoded audio data and so is inversely proportional to frame length
     * which depends on bitpool value. Bitpool is controlled by other side from
     * range [min_bitpool, max_bitpool]. */
    sbc_info->initial_bitpool = for_encoding ? sbc_info->max_bitpool : sbc_info->min_bitpool;

    set_params(sbc_info);

    pa_log_info("SBC parameters: allocation=%s, subbands=%u, blocks=%u, mode=%s bitpool=%u codesize=%u frame_length=%u",
                sbc_info->sbc.allocation ? "SNR" : "Loudness", sbc_info->sbc.subbands ? 8 : 4,
                (sbc_info->sbc.blocks+1)*4, sbc_info->sbc.mode == SBC_MODE_MONO ? "Mono" :
                sbc_info->sbc.mode == SBC_MODE_DUAL_CHANNEL ? "DualChannel" :
                sbc_info->sbc.mode == SBC_MODE_STEREO ? "Stereo" : "JointStereo",
                sbc_info->sbc.bitpool, (unsigned)sbc_info->codesize, (unsigned)sbc_info->frame_length);

    return sbc_info;
}

static void deinit(void *codec_info) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;

    sbc_finish(&sbc_info->sbc);
    pa_xfree(sbc_info);
}

static void set_bitpool(struct sbc_info *sbc_info, uint8_t bitpool) {
    if (bitpool > sbc_info->max_bitpool)
        bitpool = sbc_info->max_bitpool;
    else if (bitpool < sbc_info->min_bitpool)
        bitpool = sbc_info->min_bitpool;

    sbc_info->sbc.bitpool = bitpool;

    sbc_info->codesize = sbc_get_codesize(&sbc_info->sbc);
    sbc_info->frame_length = sbc_get_frame_length(&sbc_info->sbc);

    pa_log_debug("Bitpool has changed to %u", sbc_info->sbc.bitpool);
}

static int reset(void *codec_info) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    int ret;

    ret = sbc_reinit(&sbc_info->sbc, 0);
    if (ret != 0) {
        pa_log_error("SBC reinitialization failed: %d", ret);
        return -1;
    }

    /* sbc_reinit() sets also default parameters, so reset them back */
    set_params(sbc_info);

    sbc_info->seq_num = 0;
    return 0;
}

static size_t get_block_size(void *codec_info, size_t link_mtu) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    size_t rtp_size = sizeof(struct rtp_header) + sizeof(struct rtp_sbc_payload);
    size_t frame_count = (link_mtu - rtp_size) / sbc_info->frame_length;

    /* frame_count is only 4 bit number */
    if (frame_count > 15)
        frame_count = 15;

    return frame_count * sbc_info->codesize;
}

static size_t reduce_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    uint8_t bitpool;

    /* Check if bitpool is already at its limit */
    if (sbc_info->sbc.bitpool <= SBC_BITPOOL_DEC_LIMIT)
        return 0;

    bitpool = sbc_info->sbc.bitpool - SBC_BITPOOL_DEC_STEP;

    if (bitpool < SBC_BITPOOL_DEC_LIMIT)
        bitpool = SBC_BITPOOL_DEC_LIMIT;

    if (sbc_info->sbc.bitpool == bitpool)
        return 0;

    set_bitpool(sbc_info, bitpool);
    return get_block_size(codec_info, write_link_mtu);
}

static size_t encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    struct rtp_header *header;
    struct rtp_sbc_payload *payload;
    uint8_t *d;
    const uint8_t *p;
    size_t to_write, to_encode;
    uint8_t frame_count;

    header = (struct rtp_header*) output_buffer;
    payload = (struct rtp_sbc_payload*) (output_buffer + sizeof(*header));

    frame_count = 0;

    p = input_buffer;
    to_encode = input_size;

    d = output_buffer + sizeof(*header) + sizeof(*payload);
    to_write = output_size - sizeof(*header) - sizeof(*payload);

    /* frame_count is only 4 bit number */
    while (PA_LIKELY(to_encode > 0 && to_write > 0 && frame_count < 15)) {
        ssize_t written;
        ssize_t encoded;

        encoded = sbc_encode(&sbc_info->sbc,
                             p, to_encode,
                             d, to_write,
                             &written);

        if (PA_UNLIKELY(encoded <= 0)) {
            pa_log_error("SBC encoding error (%li)", (long) encoded);
            break;
        }

        if (PA_UNLIKELY(written < 0)) {
            pa_log_error("SBC encoding error (%li)", (long) written);
            break;
        }

        pa_assert_fp((size_t) encoded <= to_encode);
        pa_assert_fp((size_t) encoded == sbc_info->codesize);

        pa_assert_fp((size_t) written <= to_write);
        pa_assert_fp((size_t) written == sbc_info->frame_length);

        p += encoded;
        to_encode -= encoded;

        d += written;
        to_write -= written;

        frame_count++;
    }

    PA_ONCE_BEGIN {
        pa_log_debug("Using SBC codec implementation: %s", pa_strnull(sbc_get_implementation_info(&sbc_info->sbc)));
    } PA_ONCE_END;

    if (PA_UNLIKELY(frame_count == 0)) {
        *processed = 0;
        return 0;
    }

    /* write it to the fifo */
    pa_memzero(output_buffer, sizeof(*header) + sizeof(*payload));
    header->v = 2;

    /* A2DP spec: "A payload type in the RTP dynamic range shall be chosen".
     * RFC3551 defines the dynamic range to span from 96 to 127, and 96 appears
     * to be the most common choice in A2DP implementations. */
    header->pt = 96;

    header->sequence_number = htons(sbc_info->seq_num++);
    header->timestamp = htonl(timestamp);
    header->ssrc = htonl(1);
    payload->frame_count = frame_count;

    *processed = p - input_buffer;
    return d - output_buffer;
}

static size_t decode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;

    struct rtp_header *header;
    struct rtp_sbc_payload *payload;
    const uint8_t *p;
    uint8_t *d;
    size_t to_write, to_decode;
    uint8_t frame_count;

    header = (struct rtp_header *) input_buffer;
    payload = (struct rtp_sbc_payload*) (input_buffer + sizeof(*header));

    frame_count = payload->frame_count;

    /* TODO: Add support for decoding fragmented SBC frames */
    if (payload->is_fragmented) {
        pa_log_error("Unsupported fragmented SBC frame");
        *processed = 0;
        return 0;
    }

    p = input_buffer + sizeof(*header) + sizeof(*payload);
    to_decode = input_size - sizeof(*header) - sizeof(*payload);

    d = output_buffer;
    to_write = output_size;

    while (PA_LIKELY(to_decode > 0 && to_write > 0 && frame_count > 0)) {
        size_t written;
        ssize_t decoded;

        decoded = sbc_decode(&sbc_info->sbc,
                             p, to_decode,
                             d, to_write,
                             &written);

        if (PA_UNLIKELY(decoded <= 0)) {
            pa_log_error("SBC decoding error (%li)", (long) decoded);
            break;
        }

        /* Reset frame length, it can be changed due to bitpool change */
        sbc_info->frame_length = sbc_get_frame_length(&sbc_info->sbc);

        pa_assert_fp((size_t) decoded <= to_decode);
        pa_assert_fp((size_t) decoded == sbc_info->frame_length);

        pa_assert_fp((size_t) written <= to_write);
        pa_assert_fp((size_t) written == sbc_info->codesize);

        p += decoded;
        to_decode -= decoded;

        d += written;
        to_write -= written;

        frame_count--;
    }

    *processed = p - input_buffer;
    return d - output_buffer;
}

const pa_a2dp_codec pa_a2dp_codec_sbc = {
    .name = "sbc",
    .description = "SBC",
    .id = { A2DP_CODEC_SBC, 0, 0 },
    .support_backchannel = false,
    .can_accept_capabilities = can_accept_capabilities,
    .choose_remote_endpoint = choose_remote_endpoint,
    .fill_capabilities = fill_capabilities,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration,
    .init = init,
    .deinit = deinit,
    .reset = reset,
    .get_read_block_size = get_block_size,
    .get_write_block_size = get_block_size,
    .reduce_encoder_bitrate = reduce_encoder_bitrate,
    .encode_buffer = encode_buffer,
    .decode_buffer = decode_buffer,
};
