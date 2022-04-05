#pragma once

/***
  This file is part of PulseAudio.

  Copyright 2022 Dylan Van Assche <me@dylanvanassche.be>

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

#include "bluez5-util.h"

#define UPOWER_SERVICE "org.freedesktop.UPower"
#define UPOWER_DEVICE_INTERFACE ".Device"
#define UPOWER_DISPLAY_DEVICE_OBJECT "/org/freedesktop/UPower/devices/DisplayDevice"

struct pa_upower_backend {
    pa_core *core;
    pa_dbus_connection *connection;
    pa_bluetooth_discovery *discovery;
    unsigned int battery_level;

    PA_LLIST_HEAD(pa_dbus_pending, pending);
};

pa_upower_backend *pa_upower_backend_new(pa_core *c, pa_bluetooth_discovery *d);
void pa_upower_backend_free(pa_upower_backend *backend);
unsigned int pa_upower_get_battery_level(pa_upower_backend *backend);
