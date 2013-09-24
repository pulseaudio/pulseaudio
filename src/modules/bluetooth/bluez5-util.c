/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita

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

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>

#include "bluez5-util.h"

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_MEDIA_TRANSPORT_INTERFACE BLUEZ_SERVICE ".MediaTransport1"

struct pa_bluetooth_discovery {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_dbus_connection *connection;
    bool filter_added;
    bool matches_added;
    pa_hook hooks[PA_BLUETOOTH_HOOK_MAX];
    pa_hashmap *adapters;
    pa_hashmap *devices;
    pa_hashmap *transports;
};

pa_bluetooth_transport *pa_bluetooth_transport_new(pa_bluetooth_device *d, const char *owner, const char *path,
                                                   pa_bluetooth_profile_t p, const uint8_t *config, size_t size) {
    pa_bluetooth_transport *t;

    t = pa_xnew0(pa_bluetooth_transport, 1);
    t->device = d;
    t->owner = pa_xstrdup(owner);
    t->path = pa_xstrdup(path);
    t->profile = p;
    t->config_size = size;

    if (size > 0) {
        t->config = pa_xnew(uint8_t, size);
        memcpy(t->config, config, size);
    }

    pa_assert_se(pa_hashmap_put(d->discovery->transports, t->path, t) >= 0);

    return t;
}

static const char *transport_state_to_string(pa_bluetooth_transport_state_t state) {
    switch(state) {
        case PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED:
            return "disconnected";
        case PA_BLUETOOTH_TRANSPORT_STATE_IDLE:
            return "idle";
        case PA_BLUETOOTH_TRANSPORT_STATE_PLAYING:
            return "playing";
    }

    return "invalid";
}

static void transport_state_changed(pa_bluetooth_transport *t, pa_bluetooth_transport_state_t state) {
    bool old_any_connected;

    pa_assert(t);

    if (t->state == state)
        return;

    old_any_connected = pa_bluetooth_device_any_transport_connected(t->device);

    pa_log_debug("Transport %s state changed from %s to %s",
                 t->path, transport_state_to_string(t->state), transport_state_to_string(state));

    t->state = state;
    if (state == PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED)
        t->device->transports[t->profile] = NULL;

    pa_hook_fire(&t->device->discovery->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_STATE_CHANGED], t);

    if (old_any_connected != pa_bluetooth_device_any_transport_connected(t->device))
        pa_hook_fire(&t->device->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], t->device);
}

void pa_bluetooth_transport_put(pa_bluetooth_transport *t) {
    transport_state_changed(t, PA_BLUETOOTH_TRANSPORT_STATE_IDLE);
}

void pa_bluetooth_transport_free(pa_bluetooth_transport *t) {
    pa_assert(t);

    pa_hashmap_remove(t->device->discovery->transports, t->path);
    pa_xfree(t->owner);
    pa_xfree(t->path);
    pa_xfree(t->config);
    pa_xfree(t);
}

static int bluez5_transport_acquire_cb(pa_bluetooth_transport *t, bool optional, size_t *imtu, size_t *omtu) {
    DBusMessage *m, *r;
    DBusError err;
    int ret;
    uint16_t i, o;
    const char *method = optional ? "TryAcquire" : "Acquire";

    pa_assert(t);
    pa_assert(t->device);
    pa_assert(t->device->discovery);

    pa_assert_se(m = dbus_message_new_method_call(t->owner, t->path, BLUEZ_MEDIA_TRANSPORT_INTERFACE, method));

    dbus_error_init(&err);

    r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(t->device->discovery->connection), m, -1, &err);
    if (!r) {
        if (optional && pa_streq(err.name, "org.bluez.Error.NotAvailable"))
            pa_log_info("Failed optional acquire of unavailable transport %s", t->path);
        else
            pa_log_error("Transport %s() failed for transport %s (%s)", method, t->path, err.message);

        dbus_error_free(&err);
        return -1;
    }

    if (!dbus_message_get_args(r, &err, DBUS_TYPE_UNIX_FD, &ret, DBUS_TYPE_UINT16, &i, DBUS_TYPE_UINT16, &o,
                               DBUS_TYPE_INVALID)) {
        pa_log_error("Failed to parse %s() reply: %s", method, err.message);
        dbus_error_free(&err);
        ret = -1;
        goto finish;
    }

    if (imtu)
        *imtu = i;

    if (omtu)
        *omtu = o;

finish:
    dbus_message_unref(r);
    return ret;
}

static void bluez5_transport_release_cb(pa_bluetooth_transport *t) {
    DBusMessage *m;
    DBusError err;

    pa_assert(t);
    pa_assert(t->device);
    pa_assert(t->device->discovery);

    dbus_error_init(&err);

    if (t->state <= PA_BLUETOOTH_TRANSPORT_STATE_IDLE) {
        pa_log_info("Transport %s auto-released by BlueZ or already released", t->path);
        return;
    }

    pa_assert_se(m = dbus_message_new_method_call(t->owner, t->path, BLUEZ_MEDIA_TRANSPORT_INTERFACE, "Release"));
    dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(t->device->discovery->connection), m, -1, &err);

    if (dbus_error_is_set(&err)) {
        pa_log_error("Failed to release transport %s: %s", t->path, err.message);
        dbus_error_free(&err);
    } else
        pa_log_info("Transport %s released", t->path);
}

bool pa_bluetooth_device_any_transport_connected(const pa_bluetooth_device *d) {
    unsigned i;

    pa_assert(d);

    if (d->device_info_valid != 1)
        return false;

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++)
        if (d->transports[i] && d->transports[i]->state != PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED)
            return true;

    return false;
}

static pa_bluetooth_device* device_create(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(path);

    d = pa_xnew0(pa_bluetooth_device, 1);
    d->discovery = y;
    d->path = pa_xstrdup(path);

    pa_hashmap_put(y->devices, d->path, d);

    return d;
}

pa_bluetooth_device* pa_bluetooth_discovery_get_device_by_path(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(path);

    if ((d = pa_hashmap_get(y->devices, path)))
        if (d->device_info_valid == 1)
            return d;

    return NULL;
}

pa_bluetooth_device* pa_bluetooth_discovery_get_device_by_address(pa_bluetooth_discovery *y, const char *remote, const char *local) {
    pa_bluetooth_device *d;
    void *state = NULL;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(remote);
    pa_assert(local);

    while ((d = pa_hashmap_iterate(y->devices, &state, NULL)))
        if (pa_streq(d->address, remote) && pa_streq(d->adapter->address, local))
            return d->device_info_valid == 1 ? d : NULL;

    return NULL;
}

static void device_free(pa_bluetooth_device *d) {
    unsigned i;

    pa_assert(d);

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++) {
        pa_bluetooth_transport *t;

        if (!(t = d->transports[i]))
            continue;

        transport_state_changed(t, PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED);
        pa_bluetooth_transport_free(t);
    }

    d->discovery = NULL;
    d->adapter = NULL;
    pa_xfree(d->path);
    pa_xfree(d->alias);
    pa_xfree(d->address);
    pa_xfree(d);
}

static void device_remove(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_device *d;

    if (!(d = pa_hashmap_remove(y->devices, path)))
        pa_log_warn("Unknown device removed %s", path);
    else {
        pa_log_debug("Device %s removed", path);
        device_free(d);
    }
}

static void device_remove_all(pa_bluetooth_discovery *y) {
    pa_bluetooth_device *d;

    pa_assert(y);

    while ((d = pa_hashmap_steal_first(y->devices))) {
        d->device_info_valid = -1;
        pa_hook_fire(&y->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], d);
        device_free(d);
   }
}

static pa_bluetooth_adapter* adapter_create(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_adapter *a;

    pa_assert(y);
    pa_assert(path);

    a = pa_xnew0(pa_bluetooth_adapter, 1);
    a->discovery = y;
    a->path = pa_xstrdup(path);

    pa_hashmap_put(y->adapters, a->path, a);

    return a;
}

static void adapter_remove_all(pa_bluetooth_discovery *y) {
    pa_bluetooth_adapter *a;

    pa_assert(y);

    /* When this function is called all devices have already been freed */

    while ((a = pa_hashmap_steal_first(y->adapters))) {
        pa_xfree(a->path);
        pa_xfree(a->address);
        pa_xfree(a);
    }
}

pa_hook* pa_bluetooth_discovery_hook(pa_bluetooth_discovery *y, pa_bluetooth_hook_t hook) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    return &y->hooks[hook];
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y;
    DBusError err;

    pa_assert(bus);
    pa_assert(m);
    pa_assert_se(y = userdata);

    dbus_error_init(&err);

    if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;

        if (!dbus_message_get_args(m, &err,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            pa_log_error("Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
            goto fail;
        }

        if (pa_streq(name, BLUEZ_SERVICE)) {
            if (old_owner && *old_owner) {
                pa_log_debug("Bluetooth daemon disappeared");
                device_remove_all(y);
                adapter_remove_all(y);
            }

            if (new_owner && *new_owner) {
                pa_log_debug("Bluetooth daemon appeared");
                /* TODO: get managed objects */
            }
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

fail:
    dbus_error_free(&err);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *c) {
    pa_bluetooth_discovery *y;
    DBusError err;
    DBusConnection *conn;
    unsigned i;

    if ((y = pa_shared_get(c, "bluetooth-discovery")))
        return pa_bluetooth_discovery_ref(y);

    y = pa_xnew0(pa_bluetooth_discovery, 1);
    PA_REFCNT_INIT(y);
    y->core = c;
    y->adapters = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    y->devices = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    y->transports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    for (i = 0; i < PA_BLUETOOTH_HOOK_MAX; i++)
        pa_hook_init(&y->hooks[i], y);

    pa_shared_set(c, "bluetooth-discovery", y);

    dbus_error_init(&err);

    if (!(y->connection = pa_dbus_bus_get(y->core, DBUS_BUS_SYSTEM, &err))) {
        pa_log_error("Failed to get D-Bus connection: %s", err.message);
        goto fail;
    }

    conn = pa_dbus_connection_get(y->connection);

    /* dynamic detection of bluetooth audio devices */
    if (!dbus_connection_add_filter(conn, filter_cb, y, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }
    y->filter_added = true;

    if (pa_dbus_add_matches(conn, &err,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'"
            ",arg0='" BLUEZ_SERVICE "'",
            NULL) < 0) {
        pa_log_error("Failed to add D-Bus matches: %s", err.message);
        goto fail;
    }
    y->matches_added = true;

    return y;

fail:
    pa_bluetooth_discovery_unref(y);
    dbus_error_free(&err);

    return NULL;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_ref(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    PA_REFCNT_INC(y);

    return y;
}

void pa_bluetooth_discovery_unref(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    if (PA_REFCNT_DEC(y) > 0)
        return;

    if (y->devices) {
        device_remove_all(y);
        pa_hashmap_free(y->devices);
    }

    if (y->adapters) {
        adapter_remove_all(y);
        pa_hashmap_free(y->adapters);
    }

    if (y->transports) {
        pa_assert(pa_hashmap_isempty(y->transports));
        pa_hashmap_free(y->transports);
    }

    if (y->connection) {

        if (y->matches_added)
            pa_dbus_remove_matches(pa_dbus_connection_get(y->connection),
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                "arg0='" BLUEZ_SERVICE "'",
                NULL);

        if (y->filter_added)
            dbus_connection_remove_filter(pa_dbus_connection_get(y->connection), filter_cb, y);

        pa_dbus_connection_unref(y->connection);
    }

    pa_shared_remove(y->core, "bluetooth-discovery");
    pa_xfree(y);
}
