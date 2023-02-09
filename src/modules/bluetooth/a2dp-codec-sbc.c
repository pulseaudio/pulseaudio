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

#define SBC_BITPOOL_DEC_STEP 5
#define SBC_BITPOOL_INC_STEP 1

#define SBC_SYNCWORD    0x9C

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

    uint8_t nr_blocks;
    uint8_t nr_subbands;

    bool boost_source_volume;
    /* Size of SBC frame fragment left over from previous decoding iteration */
    size_t frame_fragment_size;
    /* Maximum SBC frame size is 512 bytes when SBC compression ratio > 1 */
    uint8_t frame_fragment[512];
};

static bool can_be_supported(bool for_encoding) {
    return true;
}

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

static bool can_accept_capabilities_xq(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding) {
    const a2dp_sbc_t *capabilities = (const a2dp_sbc_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities))
        return false;

    if (!(capabilities->frequency & (SBC_SAMPLING_FREQ_44100 | SBC_SAMPLING_FREQ_48000)))
        return false;

    if (!(capabilities->channel_mode & (SBC_CHANNEL_MODE_DUAL_CHANNEL)))
        return false;

    if (!(capabilities->allocation_method & (SBC_ALLOCATION_LOUDNESS)))
        return false;

    if (!(capabilities->subbands & (SBC_SUBBANDS_8)))
        return false;

    if (!(capabilities->block_length & (SBC_BLOCK_LENGTH_16)))
        return false;

    return true;
}

static bool can_accept_capabilities_faststream(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding) {
    const a2dp_faststream_t *capabilities = (const a2dp_faststream_t *) capabilities_buffer;

    if (capabilities_size != sizeof(*capabilities))
        return false;

    if (!(capabilities->direction & (FASTSTREAM_DIRECTION_SINK | FASTSTREAM_DIRECTION_SOURCE)))
        return false;

    if (!(capabilities->sink_frequency & (FASTSTREAM_SINK_SAMPLING_FREQ_44100 | FASTSTREAM_SINK_SAMPLING_FREQ_48000)))
        return false;

    if (!(capabilities->source_frequency & FASTSTREAM_SOURCE_SAMPLING_FREQ_16000))
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

static const char *choose_remote_endpoint_xq(const pa_hashmap *capabilities_hashmap, const pa_sample_spec *default_sample_spec, bool for_encoding) {
    const pa_a2dp_codec_capabilities *a2dp_capabilities;
    const char *key;
    void *state;

    /* There is no preference, just choose random valid entry */
    PA_HASHMAP_FOREACH_KV(key, a2dp_capabilities, capabilities_hashmap, state) {
        if (can_accept_capabilities_xq(a2dp_capabilities->buffer, a2dp_capabilities->size, for_encoding))
            return key;
    }

    return NULL;
}

static const char *choose_remote_endpoint_faststream(const pa_hashmap *capabilities_hashmap, const pa_sample_spec *default_sample_spec, bool for_encoding) {
    const pa_a2dp_codec_capabilities *a2dp_capabilities;
    const char *key;
    void *state;

    /* There is no preference, just choose random valid entry */
    PA_HASHMAP_FOREACH_KV(key, a2dp_capabilities, capabilities_hashmap, state) {
        pa_log_debug("choose_remote_endpoint_faststream checking peer endpoint '%s'", key);
        if (can_accept_capabilities_faststream(a2dp_capabilities->buffer, a2dp_capabilities->size, for_encoding))
            return key;
    }

    pa_log_debug("choose_remote_endpoint_faststream matched no peer endpoint");

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

static void set_info_and_sample_spec_from_sbc_config(struct sbc_info *sbc_info, pa_sample_spec *sample_spec, const a2dp_sbc_t *config) {
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
            sbc_info->nr_subbands = 4;
            break;
        case SBC_SUBBANDS_8:
            sbc_info->subbands = SBC_SB_8;
            sbc_info->nr_subbands = 8;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->block_length) {
        case SBC_BLOCK_LENGTH_4:
            sbc_info->blocks = SBC_BLK_4;
            sbc_info->nr_blocks = 4;
            break;
        case SBC_BLOCK_LENGTH_8:
            sbc_info->blocks = SBC_BLK_8;
            sbc_info->nr_blocks = 8;
            break;
        case SBC_BLOCK_LENGTH_12:
            sbc_info->blocks = SBC_BLK_12;
            sbc_info->nr_blocks = 12;
            break;
        case SBC_BLOCK_LENGTH_16:
            sbc_info->blocks = SBC_BLK_16;
            sbc_info->nr_blocks = 16;
            break;
        default:
            pa_assert_not_reached();
    }

    sbc_info->min_bitpool = config->min_bitpool;
    sbc_info->max_bitpool = config->max_bitpool;
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

static uint8_t sbc_get_max_bitpool_below_rate(a2dp_sbc_t *config, uint8_t lower_bound, uint8_t upper_bound, uint32_t bitrate_cap) {
    pa_sample_spec sample_spec;
    struct sbc_info sbc_info;
    int ret;

    pa_assert(config);

    ret = sbc_init(&sbc_info.sbc, 0);
    if (ret != 0) {
        pa_log_error("SBC initialization failed: %d", ret);
        return lower_bound;
    }

    set_info_and_sample_spec_from_sbc_config(&sbc_info, &sample_spec, config);

    while (upper_bound - lower_bound > 1) {
        size_t midpoint = (upper_bound + lower_bound) / 2;

        sbc_info.initial_bitpool = midpoint;
        set_params(&sbc_info);

        size_t bitrate = sbc_info.frame_length * 8 * sample_spec.rate / (sbc_info.nr_subbands * sbc_info.nr_blocks);

        if (bitrate > bitrate_cap)
            upper_bound = midpoint;
        else
            lower_bound = midpoint;
    }

    sbc_finish(&sbc_info.sbc);

    pa_log_debug("SBC target bitrate %u bitpool %u sample rate %u", bitrate_cap, lower_bound, sample_spec.rate);

    return lower_bound;
}

/* SBC XQ
 *
 * References:
 *   https://habr.com/en/post/456476/
 *   http://soundexpert.org/articles/-/blogs/audio-quality-of-sbc-xq-bluetooth-audio-codec
 *
 */
static uint8_t fill_capabilities_xq(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE], uint32_t bitrate_cap) {
    a2dp_sbc_t *capabilities = (a2dp_sbc_t *) capabilities_buffer;

    pa_zero(*capabilities);

    /* Bitpool value increases with sample rate. Prepare to calculate maximum viable
     * bitpool value at specified bitrate_cap, with rest of SBC parameters fixed. */
    capabilities->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
    capabilities->frequency = SBC_SAMPLING_FREQ_48000;
    capabilities->allocation_method = SBC_ALLOCATION_LOUDNESS;
    capabilities->subbands = SBC_SUBBANDS_8;
    capabilities->block_length = SBC_BLOCK_LENGTH_16;
    capabilities->min_bitpool = SBC_MIN_BITPOOL;
    capabilities->max_bitpool = SBC_MAX_BITPOOL; /* Upper boundary in calculation below. */

    /* Now calculate and write it back to be exposed through endpoint capabilities. */
    capabilities->max_bitpool = sbc_get_max_bitpool_below_rate(capabilities, capabilities->min_bitpool, capabilities->max_bitpool, bitrate_cap);

    /* Add back all supported frequencies exposed through endpoint capabilities, rest of SBC parameters are still fixed. */
    capabilities->frequency = SBC_SAMPLING_FREQ_44100 | SBC_SAMPLING_FREQ_48000;

    return sizeof(*capabilities);
}

static uint8_t fill_capabilities_faststream(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_faststream_t *capabilities = (a2dp_faststream_t *) capabilities_buffer;

    pa_zero(*capabilities);

    capabilities->info = A2DP_SET_VENDOR_ID_CODEC_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID);

    capabilities->direction = FASTSTREAM_DIRECTION_SINK | FASTSTREAM_DIRECTION_SOURCE;
    capabilities->sink_frequency = FASTSTREAM_SINK_SAMPLING_FREQ_44100 | FASTSTREAM_SINK_SAMPLING_FREQ_48000;
    capabilities->source_frequency = FASTSTREAM_SOURCE_SAMPLING_FREQ_16000;

    return sizeof(*capabilities);
}

static bool is_configuration_valid_faststream(const uint8_t *config_buffer, uint8_t config_size) {
    const a2dp_faststream_t *config = (const a2dp_faststream_t *) config_buffer;

    if (config_size != sizeof(*config)) {
        pa_log_error("Invalid size of config buffer");
        return false;
    }

    if (!(config->direction & (FASTSTREAM_DIRECTION_SINK | FASTSTREAM_DIRECTION_SOURCE))) {
        pa_log_error("Invalid FastStream direction in configuration");
        return false;
    }

    if (config->sink_frequency != FASTSTREAM_SINK_SAMPLING_FREQ_44100 && config->sink_frequency != FASTSTREAM_SINK_SAMPLING_FREQ_48000) {
        pa_log_error("Invalid FastStream sink sampling frequency in configuration");
        return false;
    }

    if (config->source_frequency != FASTSTREAM_SOURCE_SAMPLING_FREQ_16000) {
        pa_log_error("Invalid FastStream source sampling frequency in configuration");
        return false;
    }

    return true;
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

static uint8_t fill_preferred_configuration_faststream(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    a2dp_faststream_t *config = (a2dp_faststream_t *) config_buffer;
    const a2dp_faststream_t *capabilities = (const a2dp_faststream_t *) capabilities_buffer;
    int i;

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } sink_freq_table[] = {
        { 44100U, FASTSTREAM_SINK_SAMPLING_FREQ_44100 },
        { 48000U, FASTSTREAM_SINK_SAMPLING_FREQ_48000 }
    };

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } source_freq_table[] = {
        { 16000U, FASTSTREAM_SOURCE_SAMPLING_FREQ_16000 }
    };

    if (capabilities_size != sizeof(*capabilities)) {
        pa_log_error("Invalid size of FastStream capabilities buffer");
        return 0;
    }

    pa_zero(*config);

    /* Find the lowest freq that is at least as high as the requested sampling rate */
    for (i = 0; (unsigned) i < PA_ELEMENTSOF(sink_freq_table); i++)
        if (sink_freq_table[i].rate >= default_sample_spec->rate && (capabilities->sink_frequency & sink_freq_table[i].cap)) {
            config->sink_frequency = sink_freq_table[i].cap;
            break;
        }

    /* Match with endpoint capabilities */
    if ((unsigned) i == PA_ELEMENTSOF(sink_freq_table)) {
        for (--i; i >= 0; i--) {
            if (capabilities->sink_frequency & sink_freq_table[i].cap) {
                config->sink_frequency = sink_freq_table[i].cap;
                break;
            }
        }

        if (i < 0) {
            pa_log_error("Not suitable FastStream sink sample rate");
            return 0;
        }
    }

    pa_assert((unsigned) i < PA_ELEMENTSOF(sink_freq_table));

    /* Only single frequency (for now?) */
    config->source_frequency = FASTSTREAM_SOURCE_SAMPLING_FREQ_16000;
    i = 0;

    /* Match with endpoint capabilities */
    if ((unsigned) i == PA_ELEMENTSOF(source_freq_table)) {
        for (--i; i >= 0; i--) {
            if (capabilities->source_frequency & source_freq_table[i].cap) {
                config->source_frequency = source_freq_table[i].cap;
                break;
            }
        }

        if (i < 0) {
            pa_log_error("Not suitable FastStream source sample rate");
            return 0;
        }
    }

    pa_assert((unsigned) i < PA_ELEMENTSOF(source_freq_table));

    config->direction = FASTSTREAM_DIRECTION_SINK | FASTSTREAM_DIRECTION_SOURCE;

    config->info = A2DP_SET_VENDOR_ID_CODEC_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID);

    return sizeof(*config);
}

static uint8_t fill_preferred_configuration_xq(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE], uint32_t bitrate_cap) {
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
        if (capabilities->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
            config->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
        else {
            pa_log_error("No supported channel modes");
            return 0;
        }
    } else {
        if (capabilities->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
            config->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
        else {
            pa_log_error("No supported channel modes");
            return 0;
        }
    }

    if (capabilities->block_length & SBC_BLOCK_LENGTH_16)
        config->block_length = SBC_BLOCK_LENGTH_16;
    else {
        pa_log_error("No supported block lengths");
        return 0;
    }

    if (capabilities->subbands & SBC_SUBBANDS_8)
        config->subbands = SBC_SUBBANDS_8;
    else {
        pa_log_error("No supported subbands");
        return 0;
    }

    if (capabilities->allocation_method & SBC_ALLOCATION_LOUDNESS)
        config->allocation_method = SBC_ALLOCATION_LOUDNESS;
    else {
        pa_log_error("No supported allocation method");
        return 0;
    }

    config->min_bitpool = (uint8_t) PA_MAX(SBC_MIN_BITPOOL, capabilities->min_bitpool);
    config->max_bitpool = sbc_get_max_bitpool_below_rate(config, config->min_bitpool, capabilities->max_bitpool, bitrate_cap);

    if (config->min_bitpool > config->max_bitpool) {
        pa_log_error("No supported bitpool");
        return 0;
    }

    return sizeof(*config);
}

static uint8_t fill_capabilities_xq_453kbps(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    return fill_capabilities_xq(capabilities_buffer, 453000);
}

static uint8_t fill_preferred_configuration_xq_453kbps(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    return fill_preferred_configuration_xq(default_sample_spec, capabilities_buffer, capabilities_size, config_buffer, 453000);
}

static uint8_t fill_capabilities_xq_512kbps(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    return fill_capabilities_xq(capabilities_buffer, 512000);
}

static uint8_t fill_preferred_configuration_xq_512kbps(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    return fill_preferred_configuration_xq(default_sample_spec, capabilities_buffer, capabilities_size, config_buffer, 512000);
}

static uint8_t fill_capabilities_xq_552kbps(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]) {
    return fill_capabilities_xq(capabilities_buffer, 552000);
}

static uint8_t fill_preferred_configuration_xq_552kbps(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]) {
    return fill_preferred_configuration_xq(default_sample_spec, capabilities_buffer, capabilities_size, config_buffer, 552000);
}

static void *init(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
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

    set_info_and_sample_spec_from_sbc_config(sbc_info, sample_spec, config);

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

static void *init_faststream(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    struct sbc_info *sbc_info;
    const a2dp_faststream_t *config = (const a2dp_faststream_t *) config_buffer;
    int ret;

    pa_assert(config_size == sizeof(*config));

    sbc_info = pa_xnew0(struct sbc_info, 1);

    ret = sbc_init(&sbc_info->sbc, 0);
    if (ret != 0) {
        pa_xfree(sbc_info);
        pa_log_error("SBC initialization failed: %d", ret);
        return NULL;
    }

    sample_spec->format = PA_SAMPLE_S16LE;

    if (for_encoding != for_backchannel) {
        switch (config->sink_frequency) {
            case FASTSTREAM_SINK_SAMPLING_FREQ_44100:
                sbc_info->frequency = SBC_FREQ_44100;
                sample_spec->rate = 44100U;
                break;
            case FASTSTREAM_SINK_SAMPLING_FREQ_48000:
                sbc_info->frequency = SBC_FREQ_48000;
                sample_spec->rate = 48000U;
                break;
            default:
                pa_assert_not_reached();
        }

        sample_spec->channels = 2;

        sbc_info->mode = SBC_MODE_JOINT_STEREO;
        sbc_info->initial_bitpool = sbc_info->min_bitpool = sbc_info->max_bitpool = 29;
    } else {
        switch (config->source_frequency) {
            case FASTSTREAM_SOURCE_SAMPLING_FREQ_16000:
                sbc_info->frequency = SBC_FREQ_16000;
                sample_spec->rate = 16000U;
                break;
            default:
                pa_assert_not_reached();
        }

        sample_spec->channels = 2;

        sbc_info->mode = SBC_MODE_MONO;
        sbc_info->initial_bitpool = sbc_info->min_bitpool = sbc_info->max_bitpool = 32;
    }

    sbc_info->allocation = SBC_AM_LOUDNESS;
    sbc_info->subbands = SBC_SB_8;
    sbc_info->nr_subbands = 8;
    sbc_info->blocks = SBC_BLK_16;
    sbc_info->nr_blocks = 16;

    set_params(sbc_info);
    if (sbc_info->frame_length & 1)
        ++sbc_info->frame_length;

    pa_log_info("FastStream %s SBC parameters: allocation=%s, subbands=%u, blocks=%u, mode=%s bitpool=%u codesize=%u frame_length=%u",
                for_encoding ? "encoder" : "decoder",
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

    /* forget about source volume boost */
    sbc_info->boost_source_volume = false;

    /* forget last saved frame fragment */
    sbc_info->frame_fragment_size = 0;

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

static int reset_faststream(void *codec_info) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    int ret;

    ret = sbc_reinit(&sbc_info->sbc, 0);
    if (ret != 0) {
        pa_log_error("SBC reinitialization failed: %d", ret);
        return -1;
    }

    /* sbc_reinit() sets also default parameters, so reset them back */
    set_params(sbc_info);
    if (sbc_info->frame_length & 1)
        ++sbc_info->frame_length;

    sbc_info->seq_num = 0;
    return 0;
}

static size_t get_block_size(void *codec_info, size_t link_mtu) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    size_t rtp_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
    size_t frame_count = (link_mtu - rtp_size) / sbc_info->frame_length;

    /* frame_count is only 4 bit number */
    if (frame_count > 15)
        frame_count = 15;

    /* Code dealing with read/write block size expects it to be
     * non-zero to make progress, make it at least one frame.
     */
    if (frame_count < 1) {
        pa_log_warn("SBC packet size %lu is larger than link MTU %lu", sbc_info->frame_length + rtp_size, link_mtu);
        frame_count = 1;
    }

    return frame_count * sbc_info->codesize;
}

static size_t get_write_block_size_faststream(void *codec_info, size_t link_mtu) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    size_t frame_count = link_mtu / sbc_info->frame_length;

    /* 3 frames seem to work best, with minimal glitches */
    if (frame_count > 3)
        frame_count = 3;

    return frame_count * sbc_info->codesize;
}

static size_t get_read_block_size_faststream(void *codec_info, size_t link_mtu) {
    /* With SBC bitpool >= 29 and any combination of blocks, subbands
     * and channels maximum compression ratio 4:1 is achieved with
     * blocks=16, subbands=8, channels=2, bitpool=29
     *
     * Though smaller bitpools can yield higher compression ratio, faststream is
     * assumed to have fixed bitpool so maximum output size is link_mtu * 4.
     */
    return link_mtu * 4;
}

static size_t get_encoded_block_size(void *codec_info, size_t input_size) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    size_t rtp_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);

    /* input size should be aligned to codec input block size */
    pa_assert_fp(input_size % sbc_info->codesize == 0);

    return (input_size / sbc_info->codesize) * sbc_info->frame_length + rtp_size;
}

static size_t get_encoded_block_size_faststream(void *codec_info, size_t input_size) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;

    /* input size should be aligned to codec input block size */
    pa_assert_fp(input_size % sbc_info->codesize == 0);

    return (input_size / sbc_info->codesize) * sbc_info->frame_length;
}

static size_t reduce_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    uint8_t bitpool;

    bitpool = PA_MAX(sbc_info->sbc.bitpool - SBC_BITPOOL_DEC_STEP, sbc_info->min_bitpool);

    if (sbc_info->sbc.bitpool == bitpool)
        return 0;

    set_bitpool(sbc_info, bitpool);
    return get_block_size(codec_info, write_link_mtu);
}

static size_t increase_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    uint8_t bitpool;

    bitpool = PA_MIN(sbc_info->sbc.bitpool + SBC_BITPOOL_INC_STEP, sbc_info->max_bitpool);

    if (sbc_info->sbc.bitpool == bitpool)
        return 0;

    set_bitpool(sbc_info, bitpool);
    return get_block_size(codec_info, write_link_mtu);
}

static size_t encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    struct rtp_header *header;
    struct rtp_payload *payload;
    uint8_t *d;
    const uint8_t *p;
    size_t to_write, to_encode;
    uint8_t frame_count;

    header = (struct rtp_header*) output_buffer;
    payload = (struct rtp_payload*) (output_buffer + sizeof(*header));

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

static size_t encode_buffer_faststream(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;
    uint8_t *d;
    const uint8_t *p;
    size_t to_write, to_encode;
    uint8_t frame_count;

    frame_count = 0;

    p = input_buffer;
    to_encode = input_size;

    d = output_buffer;
    to_write = output_size;

    /* frame_count is only 4 bit number */
    while (PA_LIKELY(to_encode > 0 && to_write > 0)) {
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

        while (written < sbc_info->frame_length && written < to_write)
            d[written++] = 0;

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

    *processed = p - input_buffer;
    return d - output_buffer;
}

static size_t decode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;

    struct rtp_header *header;
    struct rtp_payload *payload;
    const uint8_t *p;
    uint8_t *d;
    size_t to_write, to_decode;
    uint8_t frame_count;

    header = (struct rtp_header *) input_buffer;
    payload = (struct rtp_payload*) (input_buffer + sizeof(*header));

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

static size_t decode_buffer_faststream(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;

    const uint8_t *p;
    uint8_t *d;
    size_t to_write, to_decode;
    pa_sample_spec decoded_sample_spec = {
            .format = PA_SAMPLE_S16LE,
            .channels = 2,
            .rate = 16000U
    };
    uint8_t decode_buffer[4096];
    uint8_t frame_buffer[4096];

    to_decode = input_size;

    /* append input buffer to fragment left from previous decode call */
    if (sbc_info->frame_fragment_size) {

        if (sbc_info->frame_fragment_size + to_decode > sizeof(frame_buffer)) {
            pa_log_debug("FastStream SBC input (saved + incoming) size %lu larger than buffer size %lu, input truncated to fit",
                    sbc_info->frame_fragment_size + to_decode, sizeof(frame_buffer));
            to_decode = sizeof(frame_buffer) - sbc_info->frame_fragment_size;
        }

        memcpy(frame_buffer, sbc_info->frame_fragment, sbc_info->frame_fragment_size);
        memcpy(frame_buffer + sbc_info->frame_fragment_size, input_buffer, to_decode);

        to_decode += sbc_info->frame_fragment_size;
        p = frame_buffer;

        /* clear saved fragment */
        sbc_info->frame_fragment_size = 0;
    } else
        p = input_buffer;

    d = output_buffer;
    to_write = output_size;

    while (PA_LIKELY(to_decode > 0 && to_write > 0)) {
        size_t written = 0;
        ssize_t decoded;

        /* skip to SBC sync word before attempting decode */
        if (*p != SBC_SYNCWORD) {
            ++p;
            --to_decode;
            continue;
        }

        if (to_decode < sbc_info->frame_length) {
            pa_log_debug("FastStream SBC input %lu is too short (expected frame length %lu)", to_decode, sbc_info->frame_length);
            break;
        }

        decoded = sbc_decode(&sbc_info->sbc,
                             p, to_decode,
                             decode_buffer, sizeof(decode_buffer),
                             &written);

        if (PA_UNLIKELY(decoded <= 0)) {
            /* sbc_decode returns -1 if input too short,
             * break from loop to save this frame fragment for next decode iteration */
            if (decoded == -1) {
                pa_log_debug("FastStream SBC decoding error (%li) input %lu is too short", (long) decoded, to_decode);
                break;
            }

            /* otherwise failed to decode frame, skip to next SBC sync word */
            pa_log_error("FastStream SBC decoding error (%li)", (long) decoded);
            decoded = 1;
        } else {
            /* Reset codesize and frame_length to values found by decoder */
            sbc_info->codesize = sbc_get_codesize(&sbc_info->sbc);
            sbc_info->frame_length = sbc_get_frame_length(&sbc_info->sbc);

            if (sbc_info->mode != sbc_info->sbc.mode)
                sbc_info->mode = sbc_info->sbc.mode;

            if (sbc_info->frequency != sbc_info->sbc.frequency) {
                /* some devices unexpectedly return SBC frequency different from 16000
                 * remember this, and keep incoming sample rate at 16000 */
                pa_log_debug("FastStream decoder detected SBC frequency %u, expected %u", sbc_info->sbc.frequency, sbc_info->frequency);
                sbc_info->frequency = sbc_info->sbc.frequency;

                /* volume is too low for known devices with unexpected source SBC frequency */
                pa_log_debug("FastStream decoder requesting 20dB boost for source volume");
                sbc_info->boost_source_volume = true;
            }

            if (sbc_info->sbc.mode == SBC_MODE_MONO) {
                const void *interleave_buf[2] = {decode_buffer, decode_buffer};
                /* mono->stereo conversion needs to fit into remaining output space */
                written = PA_MIN(to_write / 2, written);
                pa_interleave(interleave_buf, 2, d, pa_sample_size(&decoded_sample_spec), written / pa_sample_size(&decoded_sample_spec));
                written *= 2;
            } else
                memcpy(d, decode_buffer, written);
        }

        pa_assert_fp((size_t) decoded <= to_decode);
        pa_assert_fp((size_t) written <= to_write);

        p += decoded;
        to_decode -= decoded;

        d += written;
        to_write -= written;
    }

    if (to_decode) {
        if (to_decode > sizeof(sbc_info->frame_fragment)) {
            pa_log_debug("FastStream remaining SBC fragment size %lu larger than buffer size %lu, remainder truncated to fit",
                    to_decode, sizeof(sbc_info->frame_fragment));
            p += to_decode - sizeof(sbc_info->frame_fragment);
            to_decode = sizeof(sbc_info->frame_fragment);
        }

        pa_log_debug("FastStream saving SBC fragment size %lu for next decoding iteration", to_decode);
        memcpy(sbc_info->frame_fragment, p, to_decode);
        sbc_info->frame_fragment_size = to_decode;
    }

    *processed = input_size;

    return d - output_buffer;
}

/* Boost sink backchannel mic volume by 20dB as it appears too quiet */
double get_source_output_volume_factor_dB_faststream(void *codec_info) {
    struct sbc_info *sbc_info = (struct sbc_info *) codec_info;

    if (sbc_info->boost_source_volume)
        return 20.;

    return 1.0;
}

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc = {
    .id = { A2DP_CODEC_SBC, 0, 0 },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities,
    .choose_remote_endpoint = choose_remote_endpoint,
    .fill_capabilities = fill_capabilities,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration,
    .bt_codec = {
        .name = "sbc",
        .description = "SBC",
        .init = init,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .increase_encoder_bitrate = increase_encoder_bitrate,
        .encode_buffer = encode_buffer,
        .decode_buffer = decode_buffer,
    },
};

/* There are multiple definitions of SBC XQ, but in all cases this is
 * SBC codec in Dual Channel mode, 8 bands, block length 16, allocation method Loudness,
 * with bitpool adjusted to match target bitrates.
 *
 * Most commonly choosen bitrates and reasons are:
 * 453000 - this yields most efficient packing of frames on Android for bluetooth EDR 2mbps
 * 512000 - this looks to be old limit stated in bluetooth documents
 * 552000 - this yields most efficient packing of frames on Android for bluetooth EDR 3mbps
 *
 * Efficient packing considerations do not apply on Linux (yet?) but still
 * we can gain from increased bitrate.
 */

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc_xq_453 = {
    .id = { A2DP_CODEC_SBC, 0, 0 },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities_xq,
    .choose_remote_endpoint = choose_remote_endpoint_xq,
    .fill_capabilities = fill_capabilities_xq_453kbps,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration_xq_453kbps,
    .bt_codec = {
        .name = "sbc_xq_453",
        .description = "SBC XQ 453kbps",
        .init = init,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .increase_encoder_bitrate = increase_encoder_bitrate,
        .encode_buffer = encode_buffer,
        .decode_buffer = decode_buffer,
    },
};

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc_xq_512 = {
    .id = { A2DP_CODEC_SBC, 0, 0 },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities_xq,
    .choose_remote_endpoint = choose_remote_endpoint_xq,
    .fill_capabilities = fill_capabilities_xq_512kbps,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration_xq_512kbps,
    .bt_codec = {
        .name = "sbc_xq_512",
        .description = "SBC XQ 512kbps",
        .init = init,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .increase_encoder_bitrate = increase_encoder_bitrate,
        .encode_buffer = encode_buffer,
        .decode_buffer = decode_buffer,
    },
};

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc_xq_552 = {
    .id = { A2DP_CODEC_SBC, 0, 0 },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities_xq,
    .choose_remote_endpoint = choose_remote_endpoint_xq,
    .fill_capabilities = fill_capabilities_xq_552kbps,
    .is_configuration_valid = is_configuration_valid,
    .fill_preferred_configuration = fill_preferred_configuration_xq_552kbps,
    .bt_codec = {
        .name = "sbc_xq_552",
        .description = "SBC XQ 552kbps",
        .init = init,
        .deinit = deinit,
        .reset = reset,
        .get_read_block_size = get_block_size,
        .get_write_block_size = get_block_size,
        .get_encoded_block_size = get_encoded_block_size,
        .reduce_encoder_bitrate = reduce_encoder_bitrate,
        .increase_encoder_bitrate = increase_encoder_bitrate,
        .encode_buffer = encode_buffer,
        .decode_buffer = decode_buffer,
    },
};

/* FastStream codec is just SBC codec with fixed parameters.
 *
 * Sink stream parameters:
 *     48.0kHz or 44.1kHz,
 *     Blocks 16,
 *     Sub-bands 8,
 *     Joint Stereo,
 *     Allocation method Loudness,
 *     Bitpool = 29
 * (data rate = 212kbps, packet size = (71+1)3 <= DM5 = 220, with 3 SBC frames).
 * SBC frame size is 71 bytes, but FastStream is zero-padded to the even size (72).
 *
 * Source stream parameters:
 *     16kHz,
 *     Mono,
 *     Blocks 16,
 *     Sub-bands 8,
 *     Allocation method Loudness,
 *     Bitpool = 32
 * (data rate = 72kbps, packet size = 723 <= DM5 = 220, with 3 SBC frames).
 */

const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_faststream = {
    .id = { A2DP_CODEC_VENDOR, FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID },
    .can_be_supported = can_be_supported,
    .can_accept_capabilities = can_accept_capabilities_faststream,
    .choose_remote_endpoint = choose_remote_endpoint_faststream,
    .fill_capabilities = fill_capabilities_faststream,
    .is_configuration_valid = is_configuration_valid_faststream,
    .fill_preferred_configuration = fill_preferred_configuration_faststream,
    .bt_codec = {
        .name = "faststream",
        .description = "FastStream",
        .support_backchannel = true,
        .init = init_faststream,
        .deinit = deinit,
        .reset = reset_faststream,
        .get_read_block_size = get_read_block_size_faststream,
        .get_write_block_size = get_write_block_size_faststream,
        .get_encoded_block_size = get_encoded_block_size_faststream,
        .encode_buffer = encode_buffer_faststream,
        .decode_buffer = decode_buffer_faststream,
        .get_source_output_volume_factor_dB = get_source_output_volume_factor_dB_faststream,
    },
};
