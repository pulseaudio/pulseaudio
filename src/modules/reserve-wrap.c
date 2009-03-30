/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/i18n.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/shared.h>
#include <pulsecore/dbus-shared.h>

#include "reserve.h"
#include "reserve-wrap.h"

struct pa_reserve_wrapper {
    PA_REFCNT_DECLARE;
    pa_core *core;
    pa_dbus_connection *connection;
    pa_hook hook;
    struct rd_device *device;
    char *shared_name;
};

static void reserve_wrapper_free(pa_reserve_wrapper *r) {
    pa_assert(r);

    if (r->device)
        rd_release(r->device);

    pa_hook_done(&r->hook);

    if (r->connection)
        pa_dbus_connection_unref(r->connection);

    if (r->shared_name) {
        pa_assert_se(pa_shared_remove(r->core, r->shared_name) >= 0);
        pa_xfree(r->shared_name);
    }

    pa_xfree(r);
}

static int request_cb(rd_device *d, int forced) {
    pa_reserve_wrapper *r;
    int k;

    pa_assert(d);
    pa_assert_se(r = rd_get_userdata(d));
    pa_assert(PA_REFCNT_VALUE(r) >= 1);

    PA_REFCNT_INC(r);

    k = pa_hook_fire(&r->hook, PA_INT_TO_PTR(forced));
    pa_log_debug("Device unlock has been requested and %s.", k < 0 ? "failed" : "succeeded");

    pa_reserve_wrapper_unref(r);

    return k < 0 ? -1 : 1;
}

pa_reserve_wrapper* pa_reserve_wrapper_get(pa_core *c, const char *device_name) {
    pa_reserve_wrapper *r;
    DBusError error;
    int k;
    char *t;

    dbus_error_init(&error);

    pa_assert(c);
    pa_assert(device_name);

    t = pa_sprintf_malloc("reserve-wrapper@%s", device_name);

    if ((r = pa_shared_get(c, t))) {
        pa_xfree(t);

        pa_assert(PA_REFCNT_VALUE(r) >= 1);
        PA_REFCNT_INC(r);

        return r;
    }

    r = pa_xnew0(pa_reserve_wrapper, 1);
    PA_REFCNT_INIT(r);
    r->core = c;
    pa_hook_init(&r->hook, r);
    r->shared_name = t;

    pa_assert_se(pa_shared_set(c, r->shared_name, r) >= 0);

    if (!(r->connection = pa_dbus_bus_get(c, DBUS_BUS_SESSION, &error)) || dbus_error_is_set(&error)) {
        pa_log_warn("Unable to contact D-Bus session bus: %s: %s", error.name, error.message);

        /* We don't treat this as error here because we want allow PA
         * to run even when no session bus is available. */
        return r;
    }

    if ((k = rd_acquire(
                 &r->device,
                 pa_dbus_connection_get(r->connection),
                 device_name,
                 _("PulseAudio Sound Server"),
                 0,
                 request_cb,
                 NULL)) < 0) {

        pa_log_error("Failed to acquire reservation lock on device '%s': %s", device_name, pa_cstrerror(-k));
        goto fail;
    }

    pa_log_debug("Successfully acquired reservation lock on device '%s'", device_name);

    rd_set_userdata(r->device, r);

    return r;

fail:
    dbus_error_free(&error);

    reserve_wrapper_free(r);

    return NULL;
}

void pa_reserve_wrapper_unref(pa_reserve_wrapper *r) {
    pa_assert(r);
    pa_assert(PA_REFCNT_VALUE(r) >= 1);

    if (PA_REFCNT_DEC(r) > 0)
        return;

    reserve_wrapper_free(r);
}

pa_hook* pa_reserve_wrapper_hook(pa_reserve_wrapper *r) {
    pa_assert(r);
    pa_assert(PA_REFCNT_VALUE(r) >= 1);

    return &r->hook;
}

void pa_reserve_wrapper_set_application_device_name(pa_reserve_wrapper *r, const char *name) {
    pa_assert(r);
    pa_assert(PA_REFCNT_VALUE(r) >= 1);

    rd_set_application_device_name(r->device, name);
}
