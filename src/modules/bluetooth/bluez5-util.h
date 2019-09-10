#ifndef foobluez5utilhfoo
#define foobluez5utilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita
  Copyrigth 2018-2019 Pali Rohár <pali.rohar@gmail.com>

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

#include "a2dp-codec-util.h"

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_ADAPTER_INTERFACE BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_DEVICE_INTERFACE BLUEZ_SERVICE ".Device1"
#define BLUEZ_MEDIA_ENDPOINT_INTERFACE BLUEZ_SERVICE ".MediaEndpoint1"
#define BLUEZ_MEDIA_INTERFACE BLUEZ_SERVICE ".Media1"
#define BLUEZ_MEDIA_TRANSPORT_INTERFACE BLUEZ_SERVICE ".MediaTransport1"
#define BLUEZ_PROFILE_INTERFACE BLUEZ_SERVICE ".Profile1"
#define BLUEZ_PROFILE_MANAGER_INTERFACE BLUEZ_SERVICE ".ProfileManager1"

#define BLUEZ_ERROR_INVALID_ARGUMENTS BLUEZ_SERVICE ".Error.InvalidArguments"
#define BLUEZ_ERROR_NOT_AVAILABLE BLUEZ_SERVICE ".Error.NotAvailable"
#define BLUEZ_ERROR_NOT_SUPPORTED BLUEZ_SERVICE ".Error.NotSupported"

#define PA_BLUETOOTH_UUID_A2DP_SOURCE "0000110a-0000-1000-8000-00805f9b34fb"
#define PA_BLUETOOTH_UUID_A2DP_SINK   "0000110b-0000-1000-8000-00805f9b34fb"

/* There are two HSP HS UUIDs. The first one (older?) is used both as the HSP
 * profile identifier and as the HS role identifier, while the second one is
 * only used to identify the role. As far as PulseAudio is concerned, the two
 * UUIDs mean exactly the same thing. */
#define PA_BLUETOOTH_UUID_HSP_HS      "00001108-0000-1000-8000-00805f9b34fb"
#define PA_BLUETOOTH_UUID_HSP_HS_ALT  "00001131-0000-1000-8000-00805f9b34fb"

#define PA_BLUETOOTH_UUID_HSP_AG      "00001112-0000-1000-8000-00805f9b34fb"
#define PA_BLUETOOTH_UUID_HFP_HF      "0000111e-0000-1000-8000-00805f9b34fb"
#define PA_BLUETOOTH_UUID_HFP_AG      "0000111f-0000-1000-8000-00805f9b34fb"

#define A2DP_MAX_GAIN 127
#define HSP_MAX_GAIN 15

typedef struct pa_bluetooth_transport pa_bluetooth_transport;
typedef struct pa_bluetooth_device pa_bluetooth_device;
typedef struct pa_bluetooth_adapter pa_bluetooth_adapter;
typedef struct pa_bluetooth_discovery pa_bluetooth_discovery;
typedef struct pa_bluetooth_backend pa_bluetooth_backend;

typedef enum pa_bluetooth_hook {
    PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED,        /* Call data: pa_bluetooth_device */
    PA_BLUETOOTH_HOOK_DEVICE_UNLINK,                    /* Call data: pa_bluetooth_device */
    PA_BLUETOOTH_HOOK_TRANSPORT_STATE_CHANGED,          /* Call data: pa_bluetooth_transport */
    PA_BLUETOOTH_HOOK_TRANSPORT_SOURCE_VOLUME_CHANGED,  /* Call data: pa_bluetooth_transport */
    PA_BLUETOOTH_HOOK_TRANSPORT_SINK_VOLUME_CHANGED,    /* Call data: pa_bluetooth_transport */
    PA_BLUETOOTH_HOOK_MAX
} pa_bluetooth_hook_t;

typedef enum profile {
    PA_BLUETOOTH_PROFILE_A2DP_SINK,
    PA_BLUETOOTH_PROFILE_A2DP_SOURCE,
    PA_BLUETOOTH_PROFILE_HSP_HS,
    PA_BLUETOOTH_PROFILE_HSP_AG,
    PA_BLUETOOTH_PROFILE_HFP_HF,
    PA_BLUETOOTH_PROFILE_HFP_AG,
    PA_BLUETOOTH_PROFILE_OFF
} pa_bluetooth_profile_t;
#define PA_BLUETOOTH_PROFILE_COUNT PA_BLUETOOTH_PROFILE_OFF

typedef enum pa_bluetooth_profile_status {
  PA_BLUETOOTH_PROFILE_STATUS_INACTIVE,
  PA_BLUETOOTH_PROFILE_STATUS_ACTIVE,
  PA_BLUETOOTH_PROFILE_STATUS_REGISTERING,
  PA_BLUETOOTH_PROFILE_STATUS_REGISTERED
} pa_bluetooth_profile_status_t;

typedef enum pa_bluetooth_transport_state {
    PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED,
    PA_BLUETOOTH_TRANSPORT_STATE_IDLE,
    PA_BLUETOOTH_TRANSPORT_STATE_PLAYING
} pa_bluetooth_transport_state_t;

typedef int (*pa_bluetooth_transport_acquire_cb)(pa_bluetooth_transport *t, bool optional, size_t *imtu, size_t *omtu);
typedef void (*pa_bluetooth_transport_release_cb)(pa_bluetooth_transport *t);
typedef void (*pa_bluetooth_transport_destroy_cb)(pa_bluetooth_transport *t);
typedef pa_volume_t (*pa_bluetooth_transport_set_volume_cb)(pa_bluetooth_transport *t, pa_volume_t volume);
typedef ssize_t (*pa_bluetooth_transport_write_cb)(pa_bluetooth_transport *t, int fd, const void* buffer, size_t size, size_t write_mtu);
typedef int (*pa_bluetooth_transport_setsockopt_cb)(pa_bluetooth_transport *t, int fd);

struct pa_bluetooth_transport {
    pa_bluetooth_device *device;

    char *owner;
    char *path;
    pa_bluetooth_profile_t profile;

    void *config;
    size_t config_size;

    const pa_bt_codec *bt_codec;
    int stream_write_type;
    size_t last_read_size;

    pa_volume_t source_volume;
    pa_volume_t sink_volume;

    pa_bluetooth_transport_state_t state;

    pa_bluetooth_transport_acquire_cb acquire;
    pa_bluetooth_transport_release_cb release;
    pa_bluetooth_transport_write_cb write;
    pa_bluetooth_transport_setsockopt_cb setsockopt;
    pa_bluetooth_transport_destroy_cb destroy;
    pa_bluetooth_transport_set_volume_cb set_sink_volume;
    pa_bluetooth_transport_set_volume_cb set_source_volume;
    void *userdata;
};

struct pa_bluetooth_device {
    pa_bluetooth_discovery *discovery;
    pa_bluetooth_adapter *adapter;

    bool enable_hfp_hf;
    bool properties_received;
    bool tried_to_link_with_adapter;
    bool valid;
    bool autodetect_mtu;
    bool codec_switching_in_progress;
    bool avrcp_absolute_volume;
    uint32_t output_rate_refresh_interval_ms;

    /* Device information */
    char *path;
    char *adapter_path;
    char *alias;
    char *address;
    uint32_t class_of_device;
    pa_hashmap *uuids; /* char* -> char* (hashmap-as-a-set) */
    /* pa_a2dp_codec_id* -> pa_hashmap ( char* (remote endpoint) -> struct a2dp_codec_capabilities* ) */
    pa_hashmap *a2dp_sink_endpoints;
    pa_hashmap *a2dp_source_endpoints;

    pa_bluetooth_transport *transports[PA_BLUETOOTH_PROFILE_COUNT];

    pa_time_event *wait_for_profiles_timer;
};

struct pa_bluetooth_adapter {
    pa_bluetooth_discovery *discovery;
    char *path;
    char *address;
    pa_hashmap *uuids; /* char* -> char* (hashmap-as-a-set) */

    bool valid;
    bool application_registered;
};

#ifdef HAVE_BLUEZ_5_OFONO_HEADSET
pa_bluetooth_backend *pa_bluetooth_ofono_backend_new(pa_core *c, pa_bluetooth_discovery *y);
void pa_bluetooth_ofono_backend_free(pa_bluetooth_backend *b);
#else
static inline pa_bluetooth_backend *pa_bluetooth_ofono_backend_new(pa_core *c, pa_bluetooth_discovery *y) {
    return NULL;
}
static inline void pa_bluetooth_ofono_backend_free(pa_bluetooth_backend *b) {}
#endif

#ifdef HAVE_BLUEZ_5_NATIVE_HEADSET
pa_bluetooth_backend *pa_bluetooth_native_backend_new(pa_core *c, pa_bluetooth_discovery *y, bool enable_shared_profiles);
void pa_bluetooth_native_backend_free(pa_bluetooth_backend *b);
void pa_bluetooth_native_backend_enable_shared_profiles(pa_bluetooth_backend *b, bool enable);
#else
static inline pa_bluetooth_backend *pa_bluetooth_native_backend_new(pa_core *c, pa_bluetooth_discovery *y, bool enable_shared_profiles) {
    return NULL;
}
static inline void pa_bluetooth_native_backend_free(pa_bluetooth_backend *b) {}
static inline void pa_bluetooth_native_backend_enable_shared_profiles(pa_bluetooth_backend *b, bool enable) {}
#endif

pa_bluetooth_profile_status_t profile_status_get(pa_bluetooth_discovery *y, pa_bluetooth_profile_t profile);
void profile_status_set(pa_bluetooth_discovery *y, pa_bluetooth_profile_t profile, pa_bluetooth_profile_status_t status);

pa_bluetooth_transport *pa_bluetooth_transport_new(pa_bluetooth_device *d, const char *owner, const char *path,
                                                   pa_bluetooth_profile_t p, const uint8_t *config, size_t size);

void pa_bluetooth_transport_reconfigure(pa_bluetooth_transport *t, const pa_bt_codec *bt_codec,
                                        pa_bluetooth_transport_write_cb write_cb, pa_bluetooth_transport_setsockopt_cb setsockopt_cb);

void pa_bluetooth_transport_set_state(pa_bluetooth_transport *t, pa_bluetooth_transport_state_t state);
void pa_bluetooth_transport_put(pa_bluetooth_transport *t);
void pa_bluetooth_transport_unlink(pa_bluetooth_transport *t);
void pa_bluetooth_transport_free(pa_bluetooth_transport *t);
void pa_bluetooth_transport_load_a2dp_sink_volume(pa_bluetooth_transport *t);

bool pa_bluetooth_device_any_transport_connected(const pa_bluetooth_device *d);
bool pa_bluetooth_device_switch_codec(pa_bluetooth_device *device, pa_bluetooth_profile_t profile, pa_hashmap *capabilities_hashmap, const pa_a2dp_endpoint_conf *endpoint_conf, void (*codec_switch_cb)(bool, pa_bluetooth_profile_t profile, void *), void *userdata);

pa_bluetooth_device* pa_bluetooth_discovery_get_device_by_path(pa_bluetooth_discovery *y, const char *path);
pa_bluetooth_device* pa_bluetooth_discovery_get_device_by_address(pa_bluetooth_discovery *y, const char *remote, const char *local);

pa_hook* pa_bluetooth_discovery_hook(pa_bluetooth_discovery *y, pa_bluetooth_hook_t hook);

const char *pa_bluetooth_profile_to_string(pa_bluetooth_profile_t profile);
bool pa_bluetooth_profile_should_attenuate_volume(pa_bluetooth_profile_t profile);
bool pa_bluetooth_profile_is_a2dp(pa_bluetooth_profile_t profile);

static inline bool pa_bluetooth_uuid_is_hsp_hs(const char *uuid) {
    return pa_streq(uuid, PA_BLUETOOTH_UUID_HSP_HS) || pa_streq(uuid, PA_BLUETOOTH_UUID_HSP_HS_ALT);
}

#define HEADSET_BACKEND_OFONO 0
#define HEADSET_BACKEND_NATIVE 1
#define HEADSET_BACKEND_AUTO 2

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *core, int headset_backend, bool enable_native_hsp_hs, bool enable_native_hfp_hf, bool enable_msbc);
pa_bluetooth_discovery* pa_bluetooth_discovery_ref(pa_bluetooth_discovery *y);
void pa_bluetooth_discovery_unref(pa_bluetooth_discovery *y);
void pa_bluetooth_discovery_set_ofono_running(pa_bluetooth_discovery *y, bool is_running);
bool pa_bluetooth_discovery_get_enable_native_hsp_hs(pa_bluetooth_discovery *y);
bool pa_bluetooth_discovery_get_enable_native_hfp_hf(pa_bluetooth_discovery *y);
bool pa_bluetooth_discovery_get_enable_msbc(pa_bluetooth_discovery *y);
#endif
