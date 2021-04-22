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
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/shared.h>

#include "bluez5-util.h"

PA_MODULE_AUTHOR("João Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Detect available BlueZ 5 Bluetooth audio devices and load BlueZ 5 Bluetooth audio drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
    "headset=ofono|native|auto"
    "autodetect_mtu=<boolean>"
    "enable_msbc=<boolean, enable mSBC support in native and oFono backends, default is true>"
    "output_rate_refresh_interval_ms=<interval between attempts to improve output rate in milliseconds>"
    "enable_native_hsp_hs=<boolean, enable HSP support in native backend>"
    "enable_native_hfp_hf=<boolean, enable HFP support in native backend>"
    "avrcp_absolute_volume=<synchronize volume with peer, true by default>"
);

static const char* const valid_modargs[] = {
    "headset",
    "autodetect_mtu",
    "enable_msbc",
    "output_rate_refresh_interval_ms",
    "enable_native_hsp_hs",
    "enable_native_hfp_hf",
    "avrcp_absolute_volume",
    NULL
};

struct userdata {
    pa_module *module;
    pa_core *core;
    pa_hashmap *loaded_device_paths;
    pa_hook_slot *device_connection_changed_slot;
    pa_bluetooth_discovery *discovery;
    bool autodetect_mtu;
    bool avrcp_absolute_volume;
    uint32_t output_rate_refresh_interval_ms;
};

static pa_hook_result_t device_connection_changed_cb(pa_bluetooth_discovery *y, const pa_bluetooth_device *d, struct userdata *u) {
    bool module_loaded;

    pa_assert(d);
    pa_assert(u);

    module_loaded = pa_hashmap_get(u->loaded_device_paths, d->path) ? true : false;

    /* When changing A2DP codec there is no transport connected, ensure that no module is unloaded */
    if (module_loaded && !pa_bluetooth_device_any_transport_connected(d) &&
            !d->codec_switching_in_progress) {
        /* disconnection, the module unloads itself */
        pa_log_debug("Unregistering module for %s", d->path);
        pa_hashmap_remove(u->loaded_device_paths, d->path);
        return PA_HOOK_OK;
    }

    if (!module_loaded && pa_bluetooth_device_any_transport_connected(d)) {
        /* a new device has been connected */
        pa_module *m;
        char *args = pa_sprintf_malloc("path=%s autodetect_mtu=%i output_rate_refresh_interval_ms=%u"
                                       " avrcp_absolute_volume=%i",
                                       d->path,
                                       (int)u->autodetect_mtu,
                                       u->output_rate_refresh_interval_ms,
                                       (int)u->avrcp_absolute_volume);

        pa_log_debug("Loading module-bluez5-device %s", args);
        pa_module_load(&m, u->module->core, "module-bluez5-device", args);
        pa_xfree(args);

        if (m)
            /* No need to duplicate the path here since the device object will
             * exist for the whole hashmap entry lifespan */
            pa_hashmap_put(u->loaded_device_paths, d->path, d->path);
        else
            pa_log_warn("Failed to load module for device %s", d->path);

        return PA_HOOK_OK;
    }

    return PA_HOOK_OK;
}

#ifdef HAVE_BLUEZ_5_NATIVE_HEADSET
const char *default_headset_backend = "native";
#else
const char *default_headset_backend = "ofono";
#endif

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_modargs *ma;
    const char *headset_str;
    int headset_backend;
    bool autodetect_mtu;
    bool enable_msbc;
    bool avrcp_absolute_volume;
    uint32_t output_rate_refresh_interval_ms;
    bool enable_native_hsp_hs;
    bool enable_native_hfp_hf;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments.");
        goto fail;
    }

    pa_assert_se(headset_str = pa_modargs_get_value(ma, "headset", default_headset_backend));
    if (pa_streq(headset_str, "ofono"))
        headset_backend = HEADSET_BACKEND_OFONO;
    else if (pa_streq(headset_str, "native"))
        headset_backend = HEADSET_BACKEND_NATIVE;
    else if (pa_streq(headset_str, "auto"))
        headset_backend = HEADSET_BACKEND_AUTO;
    else {
        pa_log("headset parameter must be either ofono, native or auto (found %s)", headset_str);
        goto fail;
    }

    /* default value if no module parameter */
    enable_native_hfp_hf = (headset_backend == HEADSET_BACKEND_NATIVE);

    autodetect_mtu = false;
    if (pa_modargs_get_value_boolean(ma, "autodetect_mtu", &autodetect_mtu) < 0) {
        pa_log("Invalid boolean value for autodetect_mtu parameter");
    }
    enable_msbc = true;
    if (pa_modargs_get_value_boolean(ma, "enable_msbc", &enable_msbc) < 0) {
        pa_log("Invalid boolean value for enable_msbc parameter");
    }
    enable_native_hfp_hf = true;
    if (pa_modargs_get_value_boolean(ma, "enable_native_hfp_hf", &enable_native_hfp_hf) < 0) {
        pa_log("enable_native_hfp_hf must be true or false");
        goto fail;
    }
    enable_native_hsp_hs = !enable_native_hfp_hf;
    if (pa_modargs_get_value_boolean(ma, "enable_native_hsp_hs", &enable_native_hsp_hs) < 0) {
        pa_log("enable_native_hsp_hs must be true or false");
        goto fail;
    }

    avrcp_absolute_volume = true;
    if (pa_modargs_get_value_boolean(ma, "avrcp_absolute_volume", &avrcp_absolute_volume) < 0) {
        pa_log("avrcp_absolute_volume must be true or false");
        goto fail;
    }

    output_rate_refresh_interval_ms = DEFAULT_OUTPUT_RATE_REFRESH_INTERVAL_MS;
    if (pa_modargs_get_value_u32(ma, "output_rate_refresh_interval_ms", &output_rate_refresh_interval_ms) < 0) {
        pa_log("Invalid value for output_rate_refresh_interval parameter.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;
    u->autodetect_mtu = autodetect_mtu;
    u->avrcp_absolute_volume = avrcp_absolute_volume;
    u->output_rate_refresh_interval_ms = output_rate_refresh_interval_ms;
    u->loaded_device_paths = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    if (!(u->discovery = pa_bluetooth_discovery_get(u->core, headset_backend, enable_native_hsp_hs, enable_native_hfp_hf, enable_msbc)))
        goto fail;

    u->device_connection_changed_slot =
        pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery, PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED),
                        PA_HOOK_NORMAL, (pa_hook_cb_t) device_connection_changed_cb, u);

    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
    pa__done(m);
    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->device_connection_changed_slot)
        pa_hook_slot_free(u->device_connection_changed_slot);

    if (u->loaded_device_paths)
        pa_hashmap_free(u->loaded_device_paths);

    if (u->discovery)
        pa_bluetooth_discovery_unref(u->discovery);

    pa_xfree(u);
}
