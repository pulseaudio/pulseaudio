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
#include <pulsecore/core-util.h>

/* UUID copied from bluez/audio/device.h */
#define GENERIC_AUDIO_UUID      "00001203-0000-1000-8000-00805F9B34FB"

#define HSP_HS_UUID             "00001108-0000-1000-8000-00805F9B34FB"
#define HSP_AG_UUID             "00001112-0000-1000-8000-00805F9B34FB"

#define HFP_HS_UUID             "0000111E-0000-1000-8000-00805F9B34FB"
#define HFP_AG_UUID             "0000111F-0000-1000-8000-00805F9B34FB"

#define ADVANCED_AUDIO_UUID     "0000110D-0000-1000-8000-00805F9B34FB"

#define A2DP_SOURCE_UUID        "0000110A-0000-1000-8000-00805F9B34FB"
#define A2DP_SINK_UUID          "0000110B-0000-1000-8000-00805F9B34FB"

typedef struct pa_bluetooth_uuid pa_bluetooth_uuid;
typedef struct pa_bluetooth_device pa_bluetooth_device;
typedef struct pa_bluetooth_discovery pa_bluetooth_discovery;

struct userdata;

struct pa_bluetooth_uuid {
    char *uuid;
    PA_LLIST_FIELDS(pa_bluetooth_uuid);
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
};

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *core);
pa_bluetooth_discovery* pa_bluetooth_discovery_ref(pa_bluetooth_discovery *y);
void pa_bluetooth_discovery_unref(pa_bluetooth_discovery *d);

void pa_bluetooth_discovery_sync(pa_bluetooth_discovery *d);

const pa_bluetooth_device* pa_bluetooth_discovery_get_by_path(pa_bluetooth_discovery *d, const char* path);
const pa_bluetooth_device* pa_bluetooth_discovery_get_by_address(pa_bluetooth_discovery *d, const char* address);

pa_hook* pa_bluetooth_discovery_hook(pa_bluetooth_discovery *d);

const char* pa_bluetooth_get_form_factor(uint32_t class);

char *pa_bluetooth_cleanup_name(const char *name);

pa_bool_t pa_bluetooth_uuid_has(pa_bluetooth_uuid *uuids, const char *uuid);

#endif
