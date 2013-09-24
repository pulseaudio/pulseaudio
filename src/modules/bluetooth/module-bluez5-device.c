/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita
  Copyright 2011-2013 BMW Car IT GmbH.

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

#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>

#include "bluez5-util.h"

#include "module-bluez5-device-symdef.h"

PA_MODULE_AUTHOR("João Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("BlueZ 5 Bluetooth audio sink and source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE("path=<device object path>");

static const char* const valid_modargs[] = {
    "path",
    NULL
};

struct userdata {
    pa_module *module;
    pa_core *core;

    pa_hook_slot *device_connection_changed_slot;

    pa_bluetooth_discovery *discovery;
    pa_bluetooth_device *device;

    pa_card *card;
    pa_bluetooth_profile_t profile;
    char *output_port_name;
    char *input_port_name;
};

typedef enum pa_bluetooth_form_factor {
    PA_BLUETOOTH_FORM_FACTOR_UNKNOWN,
    PA_BLUETOOTH_FORM_FACTOR_HEADSET,
    PA_BLUETOOTH_FORM_FACTOR_HANDSFREE,
    PA_BLUETOOTH_FORM_FACTOR_MICROPHONE,
    PA_BLUETOOTH_FORM_FACTOR_SPEAKER,
    PA_BLUETOOTH_FORM_FACTOR_HEADPHONE,
    PA_BLUETOOTH_FORM_FACTOR_PORTABLE,
    PA_BLUETOOTH_FORM_FACTOR_CAR,
    PA_BLUETOOTH_FORM_FACTOR_HIFI,
    PA_BLUETOOTH_FORM_FACTOR_PHONE,
} pa_bluetooth_form_factor_t;

/* Run from main thread */
static pa_bluetooth_form_factor_t form_factor_from_class(uint32_t class_of_device) {
    unsigned major, minor;
    pa_bluetooth_form_factor_t r;

    static const pa_bluetooth_form_factor_t table[] = {
        [1] = PA_BLUETOOTH_FORM_FACTOR_HEADSET,
        [2] = PA_BLUETOOTH_FORM_FACTOR_HANDSFREE,
        [4] = PA_BLUETOOTH_FORM_FACTOR_MICROPHONE,
        [5] = PA_BLUETOOTH_FORM_FACTOR_SPEAKER,
        [6] = PA_BLUETOOTH_FORM_FACTOR_HEADPHONE,
        [7] = PA_BLUETOOTH_FORM_FACTOR_PORTABLE,
        [8] = PA_BLUETOOTH_FORM_FACTOR_CAR,
        [10] = PA_BLUETOOTH_FORM_FACTOR_HIFI
    };

    /*
     * See Bluetooth Assigned Numbers:
     * https://www.bluetooth.org/Technical/AssignedNumbers/baseband.htm
     */
    major = (class_of_device >> 8) & 0x1F;
    minor = (class_of_device >> 2) & 0x3F;

    switch (major) {
        case 2:
            return PA_BLUETOOTH_FORM_FACTOR_PHONE;
        case 4:
            break;
        default:
            pa_log_debug("Unknown Bluetooth major device class %u", major);
            return PA_BLUETOOTH_FORM_FACTOR_UNKNOWN;
    }

    r = minor < PA_ELEMENTSOF(table) ? table[minor] : PA_BLUETOOTH_FORM_FACTOR_UNKNOWN;

    if (!r)
        pa_log_debug("Unknown Bluetooth minor device class %u", minor);

    return r;
}

/* Run from main thread */
static const char *form_factor_to_string(pa_bluetooth_form_factor_t ff) {
    switch (ff) {
        case PA_BLUETOOTH_FORM_FACTOR_UNKNOWN:
            return "unknown";
        case PA_BLUETOOTH_FORM_FACTOR_HEADSET:
            return "headset";
        case PA_BLUETOOTH_FORM_FACTOR_HANDSFREE:
            return "hands-free";
        case PA_BLUETOOTH_FORM_FACTOR_MICROPHONE:
            return "microphone";
        case PA_BLUETOOTH_FORM_FACTOR_SPEAKER:
            return "speaker";
        case PA_BLUETOOTH_FORM_FACTOR_HEADPHONE:
            return "headphone";
        case PA_BLUETOOTH_FORM_FACTOR_PORTABLE:
            return "portable";
        case PA_BLUETOOTH_FORM_FACTOR_CAR:
            return "car";
        case PA_BLUETOOTH_FORM_FACTOR_HIFI:
            return "hifi";
        case PA_BLUETOOTH_FORM_FACTOR_PHONE:
            return "phone";
    }

    pa_assert_not_reached();
}

/* Run from main thread */
static char *cleanup_name(const char *name) {
    char *t, *s, *d;
    bool space = false;

    pa_assert(name);

    while ((*name >= 1 && *name <= 32) || *name >= 127)
        name++;

    t = pa_xstrdup(name);

    for (s = d = t; *s; s++) {

        if (*s <= 32 || *s >= 127 || *s == '_') {
            space = true;
            continue;
        }

        if (space) {
            *(d++) = ' ';
            space = false;
        }

        *(d++) = *s;
    }

    *d = 0;

    return t;
}

/* Run from main thread */
static pa_direction_t get_profile_direction(pa_bluetooth_profile_t p) {
    static const pa_direction_t profile_direction[] = {
        [PA_BLUETOOTH_PROFILE_A2DP_SINK] = PA_DIRECTION_OUTPUT,
        [PA_BLUETOOTH_PROFILE_A2DP_SOURCE] = PA_DIRECTION_INPUT,
        [PA_BLUETOOTH_PROFILE_OFF] = 0
    };

    return profile_direction[p];
}

/* Run from main thread */
static pa_available_t get_port_availability(struct userdata *u, pa_direction_t direction) {
    pa_available_t result = PA_AVAILABLE_NO;
    unsigned i;

    pa_assert(u);
    pa_assert(u->device);

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++) {
        pa_bluetooth_transport *transport;

        if (!(get_profile_direction(i) & direction))
            continue;

        if (!(transport = u->device->transports[i]))
            continue;

        switch(transport->state) {
            case PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED:
                continue;

            case PA_BLUETOOTH_TRANSPORT_STATE_IDLE:
                if (result == PA_AVAILABLE_NO)
                    result = PA_AVAILABLE_UNKNOWN;

                break;

            case PA_BLUETOOTH_TRANSPORT_STATE_PLAYING:
                return PA_AVAILABLE_YES;
        }
    }

    return result;
}

/* Run from main thread */
static void create_card_ports(struct userdata *u, pa_hashmap *ports) {
    pa_device_port *port;
    pa_device_port_new_data port_data;
    const char *name_prefix, *input_description, *output_description;

    pa_assert(u);
    pa_assert(ports);
    pa_assert(u->device);

    name_prefix = "unknown";
    input_description = _("Bluetooth Input");
    output_description = _("Bluetooth Output");

    switch (form_factor_from_class(u->device->class_of_device)) {
        case PA_BLUETOOTH_FORM_FACTOR_HEADSET:
            name_prefix = "headset";
            input_description = output_description = _("Headset");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_HANDSFREE:
            name_prefix = "handsfree";
            input_description = output_description = _("Handsfree");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_MICROPHONE:
            name_prefix = "microphone";
            input_description = _("Microphone");
            output_description = _("Bluetooth Output");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_SPEAKER:
            name_prefix = "speaker";
            input_description = _("Bluetooth Input");
            output_description = _("Speaker");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_HEADPHONE:
            name_prefix = "headphone";
            input_description = _("Bluetooth Input");
            output_description = _("Headphone");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_PORTABLE:
            name_prefix = "portable";
            input_description = output_description = _("Portable");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_CAR:
            name_prefix = "car";
            input_description = output_description = _("Car");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_HIFI:
            name_prefix = "hifi";
            input_description = output_description = _("HiFi");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_PHONE:
            name_prefix = "phone";
            input_description = output_description = _("Phone");
            break;

        case PA_BLUETOOTH_FORM_FACTOR_UNKNOWN:
            name_prefix = "unknown";
            input_description = _("Bluetooth Input");
            output_description = _("Bluetooth Output");
            break;
    }

    u->output_port_name = pa_sprintf_malloc("%s-output", name_prefix);
    pa_device_port_new_data_init(&port_data);
    pa_device_port_new_data_set_name(&port_data, u->output_port_name);
    pa_device_port_new_data_set_description(&port_data, output_description);
    pa_device_port_new_data_set_direction(&port_data, PA_DIRECTION_OUTPUT);
    pa_device_port_new_data_set_available(&port_data, get_port_availability(u, PA_DIRECTION_OUTPUT));
    pa_assert_se(port = pa_device_port_new(u->core, &port_data, 0));
    pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);
    pa_device_port_new_data_done(&port_data);

    u->input_port_name = pa_sprintf_malloc("%s-input", name_prefix);
    pa_device_port_new_data_init(&port_data);
    pa_device_port_new_data_set_name(&port_data, u->input_port_name);
    pa_device_port_new_data_set_description(&port_data, input_description);
    pa_device_port_new_data_set_direction(&port_data, PA_DIRECTION_INPUT);
    pa_device_port_new_data_set_available(&port_data, get_port_availability(u, PA_DIRECTION_INPUT));
    pa_assert_se(port = pa_device_port_new(u->core, &port_data, 0));
    pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);
    pa_device_port_new_data_done(&port_data);
}

/* Run from main thread */
static int add_card(struct userdata *u) {
    const pa_bluetooth_device *d;
    pa_card_new_data data;
    char *alias;
    pa_bluetooth_form_factor_t ff;
    pa_card_profile *cp;
    pa_bluetooth_profile_t *p;

    pa_assert(u);
    pa_assert(u->device);

    d = u->device;

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;

    alias = cleanup_name(d->alias);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, alias);
    pa_xfree(alias);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, d->address);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "bluez");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_BUS, "bluetooth");

    if ((ff = form_factor_from_class(d->class_of_device)) != PA_BLUETOOTH_FORM_FACTOR_UNKNOWN)
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_FORM_FACTOR, form_factor_to_string(ff));

    pa_proplist_sets(data.proplist, "bluez.path", d->path);
    pa_proplist_setf(data.proplist, "bluez.class", "0x%06x", d->class_of_device);
    pa_proplist_sets(data.proplist, "bluez.alias", d->alias);
    data.name = pa_sprintf_malloc("bluez_card.%s", d->address);
    data.namereg_fail = false;

    create_card_ports(u, data.ports);

    cp = pa_card_profile_new("off", _("Off"), sizeof(pa_bluetooth_profile_t));
    cp->available = PA_AVAILABLE_YES;
    p = PA_CARD_PROFILE_DATA(cp);
    *p = PA_BLUETOOTH_PROFILE_OFF;
    pa_hashmap_put(data.profiles, cp->name, cp);

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);
    if (!u->card) {
        pa_log("Failed to allocate card.");
        return -1;
    }

    u->card->userdata = u;

    p = PA_CARD_PROFILE_DATA(u->card->active_profile);
    u->profile = *p;

    return 0;
}

/* Run from main thread */
static pa_hook_result_t device_connection_changed_cb(pa_bluetooth_discovery *y, const pa_bluetooth_device *d, struct userdata *u) {
    pa_assert(d);
    pa_assert(u);

    if (d != u->device || pa_bluetooth_device_any_transport_connected(d))
        return PA_HOOK_OK;

    pa_log_debug("Unloading module for device %s", d->path);
    pa_module_unload(u->core, u->module, true);

    return PA_HOOK_OK;
}

int pa__init(pa_module* m) {
    struct userdata *u;
    const char *path;
    pa_modargs *ma;

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        goto fail;
    }

    if (!(path = pa_modargs_get_value(ma, "path", NULL))) {
        pa_log_error("Failed to get device path from module arguments");
        goto fail;
    }

    if (!(u->discovery = pa_bluetooth_discovery_get(m->core)))
        goto fail;

    if (!(u->device = pa_bluetooth_discovery_get_device_by_path(u->discovery, path))) {
        pa_log_error("%s is unknown", path);
        goto fail;
    }

    pa_modargs_free(ma);

    u->device_connection_changed_slot =
        pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery, PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED),
                        PA_HOOK_NORMAL, (pa_hook_cb_t) device_connection_changed_cb, u);

    if (add_card(u) < 0)
        goto fail;

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

    if (u->card)
        pa_card_free(u->card);

    if (u->discovery)
        pa_bluetooth_discovery_unref(u->discovery);

    pa_xfree(u->output_port_name);
    pa_xfree(u->input_port_name);

    pa_xfree(u);
}
