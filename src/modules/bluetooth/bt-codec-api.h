#pragma once

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

#include <pulsecore/core.h>

typedef struct pa_bt_codec {
    /* Unique name of the codec, lowercase and without whitespaces, used for
     * constructing identifier, D-Bus paths, ... */
    const char *name;
    /* Human readable codec description */
    const char *description;

    /* True if codec is bi-directional and supports backchannel */
    bool support_backchannel;

    /* Initialize codec, returns codec info data and set sample_spec,
     * for_encoding is true when codec_info is used for encoding,
     * for_backchannel is true when codec_info is used for backchannel */
    void *(*init)(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec, pa_core *core);
    /* Deinitialize and release codec info data in codec_info */
    void (*deinit)(void *codec_info);
    /* Reset internal state of codec info data in codec_info, returns
     * a negative value on failure */
    int (*reset)(void *codec_info);

    /* Get read block size for codec, it is minimal size of buffer
     * needed to decode read_link_mtu bytes of encoded data */
    size_t (*get_read_block_size)(void *codec_info, size_t read_link_mtu);
    /* Get write block size for codec, it is maximal size of buffer
     * which can produce at most write_link_mtu bytes of encoded data */
    size_t (*get_write_block_size)(void *codec_info, size_t write_link_mtu);
    /* Get encoded block size for codec to hold one encoded frame.
     * Note HFP mSBC codec encoded block may not fit into one MTU and is sent out in chunks. */
    size_t (*get_encoded_block_size)(void *codec_info, size_t input_size);

    /* Reduce encoder bitrate for codec, returns new write block size or zero
     * if not changed, called when socket is not accepting encoded data fast
     * enough */
    size_t (*reduce_encoder_bitrate)(void *codec_info, size_t write_link_mtu);

    /* Increase encoder bitrate for codec, returns new write block size or zero
     * if not changed, called periodically when socket is keeping up with
     * encoded data */
    size_t (*increase_encoder_bitrate)(void *codec_info, size_t write_link_mtu);

    /* Encode input_buffer of input_size to output_buffer of output_size,
     * returns size of filled ouput_buffer and set processed to size of
     * processed input_buffer */
    size_t (*encode_buffer)(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed);
    /* Decode input_buffer of input_size to output_buffer of output_size,
     * returns size of filled ouput_buffer and set processed to size of
     * processed input_buffer */
    size_t (*decode_buffer)(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed);

    /* Get volume factor which needs to be applied to output samples */
    double (*get_source_output_volume_factor_dB)(void *codec_info);
} pa_bt_codec;
