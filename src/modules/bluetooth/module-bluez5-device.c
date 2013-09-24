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

#include <sbc/sbc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>

#include "a2dp-codecs.h"
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

typedef struct sbc_info {
    sbc_t sbc;                           /* Codec data */
    bool sbc_initialized;                /* Keep track if the encoder is initialized */
    size_t codesize, frame_length;       /* SBC Codesize, frame_length. We simply cache those values here */
    uint16_t seq_num;                    /* Cumulative packet sequence */
    uint8_t min_bitpool;
    uint8_t max_bitpool;

    void* buffer;                        /* Codec transfer buffer */
    size_t buffer_size;                  /* Size of the buffer */
} sbc_info_t;

struct userdata {
    pa_module *module;
    pa_core *core;

    pa_hook_slot *device_connection_changed_slot;

    pa_bluetooth_discovery *discovery;
    pa_bluetooth_device *device;
    pa_bluetooth_transport *transport;
    bool transport_acquired;

    pa_card *card;
    pa_sink *sink;
    pa_bluetooth_profile_t profile;
    char *output_port_name;
    char *input_port_name;

    int stream_fd;
    size_t read_link_mtu;
    size_t write_link_mtu;
    size_t read_block_size;
    size_t write_block_size;
    pa_sample_spec sample_spec;
    struct sbc_info sbc_info;
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
static void connect_ports(struct userdata *u, void *new_data, pa_direction_t direction) {
    pa_device_port *port;

    if (direction == PA_DIRECTION_OUTPUT) {
        pa_sink_new_data *sink_new_data = new_data;

        pa_assert_se(port = pa_hashmap_get(u->card->ports, u->output_port_name));
        pa_assert_se(pa_hashmap_put(sink_new_data->ports, port->name, port) >= 0);
        pa_device_port_ref(port);
    } else {
        pa_source_new_data *source_new_data = new_data;

        pa_assert_se(port = pa_hashmap_get(u->card->ports, u->input_port_name));
        pa_assert_se(pa_hashmap_put(source_new_data->ports, port->name, port) >= 0);
        pa_device_port_ref(port);
    }
}

static int transport_acquire(struct userdata *u, bool optional) {
    pa_assert(u->transport);

    if (u->transport_acquired)
        return 0;

    pa_log_debug("Acquiring transport %s", u->transport->path);

    u->stream_fd = u->transport->acquire(u->transport, optional, &u->read_link_mtu, &u->write_link_mtu);
    if (u->stream_fd < 0)
        return -1;

    u->transport_acquired = true;
    pa_log_info("Transport %s acquired: fd %d", u->transport->path, u->stream_fd);

    return 0;
}

/* Run from main thread */
static int add_sink(struct userdata *u) {
    pa_sink_new_data data;

    pa_assert(u->transport);

    pa_sink_new_data_init(&data);
    data.module = u->module;
    data.card = u->card;
    data.driver = __FILE__;
    data.name = pa_sprintf_malloc("bluez_sink.%s", u->device->address);
    data.namereg_fail = false;
    pa_proplist_sets(data.proplist, "bluetooth.protocol", pa_bluetooth_profile_to_string(u->profile));
    pa_sink_new_data_set_sample_spec(&data, &u->sample_spec);

    connect_ports(u, &data, PA_DIRECTION_OUTPUT);

    if (!u->transport_acquired)
        switch (u->profile) {
            case PA_BLUETOOTH_PROFILE_A2DP_SINK:
                /* Profile switch should have failed */
            case PA_BLUETOOTH_PROFILE_A2DP_SOURCE:
            case PA_BLUETOOTH_PROFILE_OFF:
                pa_assert_not_reached();
                break;
        }

    u->sink = pa_sink_new(u->core, &data, PA_SINK_HARDWARE|PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);
    if (!u->sink) {
        pa_log_error("Failed to create sink");
        return -1;
    }

    u->sink->userdata = u;

    return 0;
}

/* Run from main thread */
static void transport_config(struct userdata *u) {
    sbc_info_t *sbc_info = &u->sbc_info;
    a2dp_sbc_t *config;

    pa_assert(u->transport);

    u->sample_spec.format = PA_SAMPLE_S16LE;
    config = (a2dp_sbc_t *) u->transport->config;

    if (sbc_info->sbc_initialized)
        sbc_reinit(&sbc_info->sbc, 0);
    else
        sbc_init(&sbc_info->sbc, 0);
    sbc_info->sbc_initialized = true;

    switch (config->frequency) {
        case SBC_SAMPLING_FREQ_16000:
            sbc_info->sbc.frequency = SBC_FREQ_16000;
            u->sample_spec.rate = 16000U;
            break;
        case SBC_SAMPLING_FREQ_32000:
            sbc_info->sbc.frequency = SBC_FREQ_32000;
            u->sample_spec.rate = 32000U;
            break;
        case SBC_SAMPLING_FREQ_44100:
            sbc_info->sbc.frequency = SBC_FREQ_44100;
            u->sample_spec.rate = 44100U;
            break;
        case SBC_SAMPLING_FREQ_48000:
            sbc_info->sbc.frequency = SBC_FREQ_48000;
            u->sample_spec.rate = 48000U;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->channel_mode) {
        case SBC_CHANNEL_MODE_MONO:
            sbc_info->sbc.mode = SBC_MODE_MONO;
            u->sample_spec.channels = 1;
            break;
        case SBC_CHANNEL_MODE_DUAL_CHANNEL:
            sbc_info->sbc.mode = SBC_MODE_DUAL_CHANNEL;
            u->sample_spec.channels = 2;
            break;
        case SBC_CHANNEL_MODE_STEREO:
            sbc_info->sbc.mode = SBC_MODE_STEREO;
            u->sample_spec.channels = 2;
            break;
        case SBC_CHANNEL_MODE_JOINT_STEREO:
            sbc_info->sbc.mode = SBC_MODE_JOINT_STEREO;
            u->sample_spec.channels = 2;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->allocation_method) {
        case SBC_ALLOCATION_SNR:
            sbc_info->sbc.allocation = SBC_AM_SNR;
            break;
        case SBC_ALLOCATION_LOUDNESS:
            sbc_info->sbc.allocation = SBC_AM_LOUDNESS;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->subbands) {
        case SBC_SUBBANDS_4:
            sbc_info->sbc.subbands = SBC_SB_4;
            break;
        case SBC_SUBBANDS_8:
            sbc_info->sbc.subbands = SBC_SB_8;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->block_length) {
        case SBC_BLOCK_LENGTH_4:
            sbc_info->sbc.blocks = SBC_BLK_4;
            break;
        case SBC_BLOCK_LENGTH_8:
            sbc_info->sbc.blocks = SBC_BLK_8;
            break;
        case SBC_BLOCK_LENGTH_12:
            sbc_info->sbc.blocks = SBC_BLK_12;
            break;
        case SBC_BLOCK_LENGTH_16:
            sbc_info->sbc.blocks = SBC_BLK_16;
            break;
        default:
            pa_assert_not_reached();
    }

    sbc_info->min_bitpool = config->min_bitpool;
    sbc_info->max_bitpool = config->max_bitpool;

    /* Set minimum bitpool for source to get the maximum possible block_size */
    sbc_info->sbc.bitpool = u->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK ? sbc_info->max_bitpool : sbc_info->min_bitpool;
    sbc_info->codesize = sbc_get_codesize(&sbc_info->sbc);
    sbc_info->frame_length = sbc_get_frame_length(&sbc_info->sbc);

    pa_log_info("SBC parameters: allocation=%u, subbands=%u, blocks=%u, bitpool=%u",
                sbc_info->sbc.allocation, sbc_info->sbc.subbands, sbc_info->sbc.blocks, sbc_info->sbc.bitpool);
}

/* Run from main thread */
static int setup_transport(struct userdata *u) {
    pa_bluetooth_transport *t;

    pa_assert(u);
    pa_assert(!u->transport);
    pa_assert(u->profile != PA_BLUETOOTH_PROFILE_OFF);

    /* check if profile has a transport */
    t = u->device->transports[u->profile];
    if (!t || t->state <= PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED) {
        pa_log_warn("Profile has no transport");
        return -1;
    }

    u->transport = t;

    if (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE)
        transport_acquire(u, true); /* In case of error, the sink/sources will be created suspended */
    else if (transport_acquire(u, false) < 0)
        return -1; /* We need to fail here until the interactions with module-suspend-on-idle and alike get improved */

    transport_config(u);

    return 0;
}

/* Run from main thread */
static int init_profile(struct userdata *u) {
    int r = 0;
    pa_assert(u);
    pa_assert(u->profile != PA_BLUETOOTH_PROFILE_OFF);

    if (setup_transport(u) < 0)
        return -1;

    pa_assert(u->transport);

    if (u->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK)
        if (add_sink(u) < 0)
            r = -1;

    /* TODO: add source */

    return r;
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
static pa_available_t transport_state_to_availability(pa_bluetooth_transport_state_t state) {
    switch (state) {
        case PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED:
            return PA_AVAILABLE_NO;
        case PA_BLUETOOTH_TRANSPORT_STATE_PLAYING:
            return PA_AVAILABLE_YES;
        default:
            return PA_AVAILABLE_UNKNOWN;
    }
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
static pa_card_profile *create_card_profile(struct userdata *u, const char *uuid, pa_hashmap *ports) {
    pa_device_port *input_port, *output_port;
    pa_card_profile *cp = NULL;
    pa_bluetooth_profile_t *p;

    pa_assert(u->input_port_name);
    pa_assert(u->output_port_name);
    pa_assert_se(input_port = pa_hashmap_get(ports, u->input_port_name));
    pa_assert_se(output_port = pa_hashmap_get(ports, u->output_port_name));

    if (pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SINK)) {
	/* TODO: Change this profile's name to a2dp_sink, to reflect the remote
         * device's role and be consistent with the a2dp source profile */
        cp = pa_card_profile_new("a2dp", _("High Fidelity Playback (A2DP Sink)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 10;
        cp->n_sinks = 1;
        cp->n_sources = 0;
        cp->max_sink_channels = 2;
        cp->max_source_channels = 0;
        pa_hashmap_put(output_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        *p = PA_BLUETOOTH_PROFILE_A2DP_SINK;
    } else if (pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SOURCE)) {
        cp = pa_card_profile_new("a2dp_source", _("High Fidelity Capture (A2DP Source)"), sizeof(pa_bluetooth_profile_t));
        cp->priority = 10;
        cp->n_sinks = 0;
        cp->n_sources = 1;
        cp->max_sink_channels = 0;
        cp->max_source_channels = 2;
        pa_hashmap_put(input_port->profiles, cp->name, cp);

        p = PA_CARD_PROFILE_DATA(cp);
        *p = PA_BLUETOOTH_PROFILE_A2DP_SOURCE;
    }

    if (cp && u->device->transports[*p])
        cp->available = transport_state_to_availability(u->device->transports[*p]->state);

    return cp;
}

/* Run from main thread */
static int add_card(struct userdata *u) {
    const pa_bluetooth_device *d;
    pa_card_new_data data;
    char *alias;
    pa_bluetooth_form_factor_t ff;
    pa_card_profile *cp;
    pa_bluetooth_profile_t *p;
    const char *uuid;
    void *state;

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

    PA_HASHMAP_FOREACH(uuid, d->uuids, state) {
        cp = create_card_profile(u, uuid, data.ports);

        if (!cp)
            continue;

        if (pa_hashmap_get(data.profiles, cp->name)) {
            pa_card_profile_free(cp);
            continue;
        }

        pa_hashmap_put(data.profiles, cp->name, cp);
    }

    pa_assert(!pa_hashmap_isempty(data.profiles));

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

    if (u->profile != PA_BLUETOOTH_PROFILE_OFF)
        if (init_profile(u) < 0)
            goto off;

    return 0;

off:

    pa_assert_se(pa_card_set_profile(u->card, "off", false) >= 0);

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

    if (u->sbc_info.sbc_initialized)
        sbc_finish(&u->sbc_info.sbc);

    if (u->card)
        pa_card_free(u->card);

    if (u->discovery)
        pa_bluetooth_discovery_unref(u->discovery);

    pa_xfree(u->output_port_name);
    pa_xfree(u->input_port_name);

    pa_xfree(u);
}
