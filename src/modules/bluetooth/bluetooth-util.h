#ifndef foobluetoothutilhfoo
#define foobluetoothutilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2008 Joao Paulo Rechi Vita

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

typedef struct pa_bluetooth_uuid pa_bluetooth_uuid;
typedef struct pa_bluetooth_device pa_bluetooth_device;
typedef struct pa_bluetooth_discovery pa_bluetooth_discovery;

struct userdata;

struct pa_bluetooth_uuid {
    char *uuid;
    PA_LLIST_FIELDS(pa_bluetooth_uuid);
};

struct pa_bluetooth_device {
    void *data; /* arbitrary information for the one owning the discovery object */

    int device_info_valid;      /* 0: no results yet; 1: good results; -1: bad results ... */
    int audio_sink_info_valid;  /* ... same here ... */
    int headset_info_valid;     /* ... and here */

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

    /* AudioSink information */
    int audio_sink_connected;

    /* Headset information */
    int headset_connected;
};

void pa_bluetooth_device_free(pa_bluetooth_device *d);

pa_bluetooth_device* pa_bluetooth_get_device(DBusConnection *c, const char* path);
pa_bluetooth_device* pa_bluetooth_find_device(DBusConnection *c, const char* address);

typedef void (*pa_bluetooth_device_callback_t)(struct userdata *u, pa_bluetooth_device *d, pa_bool_t good);
pa_bluetooth_discovery* pa_bluetooth_discovery_new(DBusConnection *c, pa_bluetooth_device_callback_t cb, struct userdata *u);
void pa_bluetooth_discovery_free(pa_bluetooth_discovery *d);
void pa_bluetooth_discovery_sync(pa_bluetooth_discovery *d);

const char*pa_bluetooth_get_form_factor(uint32_t class);

char *pa_bluetooth_cleanup_name(const char *name);

#endif
