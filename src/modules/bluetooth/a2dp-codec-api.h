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

#include "bt-codec-api.h"

#define MAX_A2DP_CAPS_SIZE 254
#define DEFAULT_OUTPUT_RATE_REFRESH_INTERVAL_MS 500

typedef struct pa_a2dp_codec_capabilities {
    uint8_t size;
    uint8_t buffer[]; /* max size is 254 bytes */
} pa_a2dp_codec_capabilities;

typedef struct pa_a2dp_codec_id {
    uint8_t codec_id;
    uint32_t vendor_id;
    uint16_t vendor_codec_id;
} pa_a2dp_codec_id;

typedef struct pa_a2dp_endpoint_conf {
    /* A2DP codec id */
    pa_a2dp_codec_id id;

    /* Returns true if the codec can be supported on the system */
    bool (*can_be_supported)(bool for_encoding);

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

    /* Bluetooth codec */
    pa_bt_codec bt_codec;
} pa_a2dp_endpoint_conf;

#endif
