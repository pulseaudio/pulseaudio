/***
  This file is part of PulseAudio.

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

#include <pulsecore/core.h>
#include "bt-codec-api.h"

typedef struct codec_info {
    pa_sample_spec sample_spec;
} codec_info_t;

static void *init(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core) {
    codec_info_t *info;

    info = pa_xnew0(codec_info_t, 1);

    info->sample_spec.format = PA_SAMPLE_S16LE;
    info->sample_spec.channels = 1;
    info->sample_spec.rate = 8000;

    *sample_spec = info->sample_spec;

    return info;
}

static void deinit(void *codec_info) {
    pa_xfree(codec_info);
}

static int reset(void *codec_info) {
    return 0;
}

static size_t get_block_size(void *codec_info, size_t link_mtu) {
    codec_info_t *info = (codec_info_t *) codec_info;
    size_t block_size = link_mtu;

    if (!pa_frame_aligned(block_size, &info->sample_spec)) {
        pa_log_debug("Got invalid block size: %lu, rounding down", block_size);
        block_size = pa_frame_align(block_size, &info->sample_spec);
    }

    return block_size;
}

static size_t get_encoded_block_size(void *codec_info, size_t input_size) {
    codec_info_t *info = (codec_info_t *) codec_info;

    /* input size should be aligned to sample spec */
    pa_assert_fp(pa_frame_aligned(input_size, &info->sample_spec));

    return input_size;
}

static size_t reduce_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    return 0;
}

static size_t increase_encoder_bitrate(void *codec_info, size_t write_link_mtu) {
    return 0;
}

static size_t encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    pa_assert(input_size <= output_size);

    memcpy(output_buffer, input_buffer, input_size);
    *processed = input_size;

    return input_size;
}

static size_t decode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed) {
    codec_info_t *info = (codec_info_t *) codec_info;

    *processed = input_size;

    /* In some rare occasions, we might receive packets of a very strange
     * size. This could potentially be possible if the SCO packet was
     * received partially over-the-air, or more probably due to hardware
     * issues in our Bluetooth adapter. In these cases, in order to avoid
     * an assertion failure due to unaligned data, just discard the whole
     * packet */
    if (!pa_frame_aligned(input_size, &info->sample_spec)) {
        pa_log_warn("SCO packet received of unaligned size: %zu", input_size);
        return 0;
    }

    memcpy(output_buffer, input_buffer, input_size);

    return input_size;
}

/* dummy passthrough codec used with HSP/HFP CVSD */
const pa_bt_codec pa_bt_codec_cvsd = {
    .name = "CVSD",
    .description = "CVSD",
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
};
