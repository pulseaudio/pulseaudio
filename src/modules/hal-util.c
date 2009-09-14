/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/log.h>
#include <pulsecore/dbus-shared.h>

#include <hal/libhal.h>

#include "hal-util.h"

int pa_hal_get_info(pa_core *core, pa_proplist *p, int card) {
    pa_dbus_connection *c = NULL;
    LibHalContext *hal = NULL;
    DBusError error;
    int r = -1;
    char **udis = NULL, *t;
    int n, i;

    pa_assert(core);
    pa_assert(p);
    pa_assert(card >= 0);

    dbus_error_init(&error);

    if (!(c = pa_dbus_bus_get(core, DBUS_BUS_SYSTEM, &error)) || dbus_error_is_set(&error)) {
        pa_log_error("Unable to contact DBUS system bus: %s: %s", error.name, error.message);
        goto finish;
    }


    if (!(hal = libhal_ctx_new())) {
        pa_log_error("libhal_ctx_new() finished");
        goto finish;
    }

    if (!libhal_ctx_set_dbus_connection(hal, pa_dbus_connection_get(c))) {
        pa_log_error("Error establishing DBUS connection: %s: %s", error.name, error.message);
        goto finish;
    }

    if (!libhal_ctx_init(hal, &error)) {
        pa_log_error("Couldn't connect to hald: %s: %s", error.name, error.message);
        goto finish;
    }

    if (!(udis = libhal_find_device_by_capability(hal, "sound", &n, &error))) {
        pa_log_error("Couldn't find devices: %s: %s", error.name, error.message);
        goto finish;
    }

    for (i = 0; i < n; i++) {
        dbus_int32_t this_card;

        this_card = libhal_device_get_property_int(hal, udis[i], "sound.card", &error);
        if (dbus_error_is_set(&error)) {
            dbus_error_free(&error);
            continue;
        }

        if (this_card == card)
            break;

    }

    if (i >= n)
        goto finish;

    pa_proplist_sets(p, "hal.udi", udis[i]);

    /* The data HAL stores in info.product is not actually a product
     * string but simply the ALSA card name. We will hence not write
     * it to PA_PROP_DEVICE_PRODUCT_NAME */
    t = libhal_device_get_property_string(hal, udis[i], "info.product", &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    if (t) {
        pa_proplist_sets(p, "hal.product", t);
        libhal_free_string(t);
    }

    t = libhal_device_get_property_string(hal, udis[i], "sound.card_id", &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    if (t) {
        pa_proplist_sets(p, "hal.card_id", t);
        libhal_free_string(t);
    }

    r = 0;

finish:

    if (udis)
        libhal_free_string_array(udis);

    dbus_error_free(&error);

    if (hal) {
        libhal_ctx_shutdown(hal, &error);
        libhal_ctx_free(hal);
        dbus_error_free(&error);
    }

    if (c)
        pa_dbus_connection_unref(c);

    return r;
}
