#ifndef foobluetoothutilhfoo
#define foobluetoothutilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2008-2009 Joao Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <dbus/dbus.h>

#include <pulsecore/llist.h>
#include <pulsecore/macro.h>

#define PA_BLUETOOTH_ERROR_NOT_SUPPORTED "org.bluez.Error.NotSupported"

/* UUID copied from bluez/audio/device.h */
#define GENERIC_AUDIO_UUID      "00001203-0000-1000-8000-00805f9b34fb"

#define HSP_HS_UUID             "00001108-0000-1000-8000-00805f9b34fb"
#define HSP_AG_UUID             "00001112-0000-1000-8000-00805f9b34fb"

#define HFP_HS_UUID             "0000111e-0000-1000-8000-00805f9b34fb"
#define HFP_AG_UUID             "0000111f-0000-1000-8000-00805f9b34fb"

#define ADVANCED_AUDIO_UUID     "0000110d-0000-1000-8000-00805f9b34fb"

#define A2DP_SOURCE_UUID        "0000110a-0000-1000-8000-00805f9b34fb"
#define A2DP_SINK_UUID          "0000110b-0000-1000-8000-00805f9b34fb"

typedef struct pa_bluetooth_uuid pa_bluetooth_uuid;
typedef struct pa_bluetooth_device pa_bluetooth_device;
typedef struct pa_bluetooth_discovery pa_bluetooth_discovery;
typedef struct pa_bluetooth_transport pa_bluetooth_transport;

struct userdata;

struct pa_bluetooth_uuid {
    char *uuid;
    PA_LLIST_FIELDS(pa_bluetooth_uuid);
};

enum profile {
    PROFILE_A2DP,
    PROFILE_A2DP_SOURCE,
    PROFILE_HSP,
    PROFILE_HFGW,
    PROFILE_OFF
};

struct pa_bluetooth_transport {
    pa_bluetooth_discovery *y;
    char *path;
    enum profile profile;
    uint8_t codec;
    uint8_t *config;
    int config_size;
    pa_bool_t nrec;
};

/* This enum is shared among Audio, Headset, AudioSink, and AudioSource, although not all values are acceptable in all profiles */
typedef enum pa_bt_audio_state {
    PA_BT_AUDIO_STATE_INVALID = -1,
    PA_BT_AUDIO_STATE_DISCONNECTED,
    PA_BT_AUDIO_STATE_CONNECTING,
    PA_BT_AUDIO_STATE_CONNECTED,
    PA_BT_AUDIO_STATE_PLAYING
} pa_bt_audio_state_t;

struct pa_bluetooth_device {
    pa_bool_t dead;

    int device_info_valid;      /* 0: no results yet; 1: good results; -1: bad results ... */

    /* Device information */
    char *name;
    char *path;
    pa_hashmap *transports;
    int paired;
    char *alias;
    int device_connected;
    PA_LLIST_HEAD(pa_bluetooth_uuid, uuids);
    char *address;
    int class;
    int trusted;

    /* Audio state */
    pa_bt_audio_state_t audio_state;

    /* AudioSink state */
    pa_bt_audio_state_t audio_sink_state;

    /* AudioSource state */
    pa_bt_audio_state_t audio_source_state;

    /* Headset state */
    pa_bt_audio_state_t headset_state;

    /* HandsfreeGateway state */
    pa_bt_audio_state_t hfgw_state;
};

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *core);
pa_bluetooth_discovery* pa_bluetooth_discovery_ref(pa_bluetooth_discovery *y);
void pa_bluetooth_discovery_unref(pa_bluetooth_discovery *d);

void pa_bluetooth_discovery_sync(pa_bluetooth_discovery *d);

const pa_bluetooth_device* pa_bluetooth_discovery_get_by_path(pa_bluetooth_discovery *d, const char* path);
const pa_bluetooth_device* pa_bluetooth_discovery_get_by_address(pa_bluetooth_discovery *d, const char* address);

const pa_bluetooth_transport* pa_bluetooth_discovery_get_transport(pa_bluetooth_discovery *y, const char *path);
const pa_bluetooth_transport* pa_bluetooth_device_get_transport(const pa_bluetooth_device *d, enum profile profile);

int pa_bluetooth_transport_acquire(const pa_bluetooth_transport *t, const char *accesstype, size_t *imtu, size_t *omtu);
void pa_bluetooth_transport_release(const pa_bluetooth_transport *t, const char *accesstype);
int pa_bluetooth_transport_parse_property(pa_bluetooth_transport *t, DBusMessageIter *i);

pa_hook* pa_bluetooth_discovery_hook(pa_bluetooth_discovery *d);

const char* pa_bluetooth_get_form_factor(uint32_t class);

char *pa_bluetooth_cleanup_name(const char *name);

pa_bool_t pa_bluetooth_uuid_has(pa_bluetooth_uuid *uuids, const char *uuid);
pa_bt_audio_state_t pa_bt_audio_state_from_string(const char* value);

#endif
