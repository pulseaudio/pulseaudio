#ifndef fooa2dpcodechfoo
#define fooa2dpcodechfoo

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

#include <pulsecore/core.h>

#define MAX_A2DP_CAPS_SIZE 254

typedef struct pa_a2dp_codec_capabilities {
    uint8_t size;
    uint8_t buffer[]; /* max size is 254 bytes */
} pa_a2dp_codec_capabilities;

typedef struct pa_a2dp_codec_id {
    uint8_t codec_id;
    uint32_t vendor_id;
    uint16_t vendor_codec_id;
} pa_a2dp_codec_id;

typedef struct pa_a2dp_codec {
    /* Unique name of the codec, lowercase and without whitespaces, used for
     * constructing identifier, D-Bus paths, ... */
    const char *name;
    /* Human readable codec description */
    const char *description;

    /* A2DP codec id */
    pa_a2dp_codec_id id;

    /* True if codec is bi-directional and supports backchannel */
    bool support_backchannel;

    /* Returns true if codec accepts capabilities, for_encoding is true when
     * capabilities are used for encoding */
    bool (*can_accept_capabilities)(const uint8_t *capabilities_buffer, uint8_t capabilities_size, bool for_encoding);
    /* Choose remote endpoint based on capabilities from hash map
     * (const char *endpoint -> const pa_a2dp_codec_capabilities *capability)
     * and returns corresponding endpoint key (or NULL when there is no valid),
     * for_encoder is true when capabilities hash map is used for encoding */
    const char *(*choose_remote_endpoint)(const pa_hashmap *capabilities_hashmap, const pa_sample_spec *default_sample_spec, bool for_encoding);
    /* Fill codec capabilities, returns size of filled buffer */
    uint8_t (*fill_capabilities)(uint8_t capabilities_buffer[MAX_A2DP_CAPS_SIZE]);
    /* Validate codec configuration, returns true on success */
    bool (*is_configuration_valid)(const uint8_t *config_buffer, uint8_t config_size);
    /* Fill preferred codec configuration, returns size of filled buffer or 0 on failure */
    uint8_t (*fill_preferred_configuration)(const pa_sample_spec *default_sample_spec, const uint8_t *capabilities_buffer, uint8_t capabilities_size, uint8_t config_buffer[MAX_A2DP_CAPS_SIZE]);

    /* Initialize codec, returns codec info data and set sample_spec,
     * for_encoding is true when codec_info is used for encoding,
     * for_backchannel is true when codec_info is used for backchannel */
    void *(*init)(bool for_encoding, bool for_backchannel, const uint8_t *config_buffer, uint8_t config_size, pa_sample_spec *sample_spec);
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

    /* Reduce encoder bitrate for codec, returns new write block size or zero
     * if not changed, called when socket is not accepting encoded data fast
     * enough */
    size_t (*reduce_encoder_bitrate)(void *codec_info, size_t write_link_mtu);

    /* Encode input_buffer of input_size to output_buffer of output_size,
     * returns size of filled ouput_buffer and set processed to size of
     * processed input_buffer */
    size_t (*encode_buffer)(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed);
    /* Decode input_buffer of input_size to output_buffer of output_size,
     * returns size of filled ouput_buffer and set processed to size of
     * processed input_buffer */
    size_t (*decode_buffer)(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed);
} pa_a2dp_codec;

#endif
