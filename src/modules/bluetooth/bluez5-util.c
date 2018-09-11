/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita
  Copyrigth 2018-2019 Pali Rohár <pali.rohar@gmail.com>

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

#include <errno.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>

#include "a2dp-codec-api.h"
#include "a2dp-codec-util.h"
#include "a2dp-codecs.h"

#include "bluez5-util.h"

#define WAIT_FOR_PROFILES_TIMEOUT_USEC (3 * PA_USEC_PER_SEC)

#define DBUS_INTERFACE_OBJECT_MANAGER DBUS_INTERFACE_DBUS ".ObjectManager"

#define A2DP_OBJECT_MANAGER_PATH "/MediaEndpoint"
#define A2DP_SOURCE_ENDPOINT A2DP_OBJECT_MANAGER_PATH "/A2DPSource"
#define A2DP_SINK_ENDPOINT A2DP_OBJECT_MANAGER_PATH "/A2DPSink"

#define OBJECT_MANAGER_INTROSPECT_XML                                          \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                                  \
    "<node>\n"                                                                 \
    " <interface name=\"" DBUS_INTERFACE_OBJECT_MANAGER "\">\n"                \
    "  <method name=\"GetManagedObjects\">\n"                                  \
    "   <arg name=\"objects\" direction=\"out\" type=\"a{oa{sa{sv}}}\"/>\n"    \
    "  </method>\n"                                                            \
    "  <signal name=\"InterfacesAdded\">\n"                                    \
    "   <arg name=\"object\" type=\"o\"/>\n"                                   \
    "   <arg name=\"interfaces\" type=\"a{sa{sv}}\"/>\n"                       \
    "  </signal>\n"                                                            \
    "  <signal name=\"InterfacesRemoved\">\n"                                  \
    "   <arg name=\"object\" type=\"o\"/>\n"                                   \
    "   <arg name=\"interfaces\" type=\"as\"/>\n"                              \
    "  </signal>\n"                                                            \
    " </interface>\n"                                                          \
    " <interface name=\"" DBUS_INTERFACE_INTROSPECTABLE "\">\n"                \
    "  <method name=\"Introspect\">\n"                                         \
    "   <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"                   \
    "  </method>\n"                                                            \
    " </interface>\n"                                                          \
    " <node name=\"A2DPSink\"/>\n"                                             \
    " <node name=\"A2DPSource\"/>\n"                                           \
    "</node>\n"

#define ENDPOINT_INTROSPECT_XML                                         \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
    "<node>"                                                            \
    " <interface name=\"" BLUEZ_MEDIA_ENDPOINT_INTERFACE "\">"          \
    "  <method name=\"SetConfiguration\">"                              \
    "   <arg name=\"transport\" direction=\"in\" type=\"o\"/>"          \
    "   <arg name=\"properties\" direction=\"in\" type=\"ay\"/>"        \
    "  </method>"                                                       \
    "  <method name=\"SelectConfiguration\">"                           \
    "   <arg name=\"capabilities\" direction=\"in\" type=\"ay\"/>"      \
    "   <arg name=\"configuration\" direction=\"out\" type=\"ay\"/>"    \
    "  </method>"                                                       \
    "  <method name=\"ClearConfiguration\">"                            \
    "   <arg name=\"transport\" direction=\"in\" type=\"o\"/>"          \
    "  </method>"                                                       \
    "  <method name=\"Release\">"                                       \
    "  </method>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"" DBUS_INTERFACE_INTROSPECTABLE "\">"           \
    "  <method name=\"Introspect\">"                                    \
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
    "  </method>"                                                       \
    " </interface>"                                                     \
    "</node>"

static pa_volume_t a2dp_gain_to_volume(uint16_t gain) {
    pa_volume_t volume = (pa_volume_t) ((
        gain * PA_VOLUME_NORM
        /* Round to closest by adding half the denominator */
        + A2DP_MAX_GAIN / 2
    ) / A2DP_MAX_GAIN);

    if (volume > PA_VOLUME_NORM)
        volume = PA_VOLUME_NORM;

    return volume;
}

static uint16_t volume_to_a2dp_gain(pa_volume_t volume) {
    uint16_t gain = (uint16_t) ((
        volume * A2DP_MAX_GAIN
        /* Round to closest by adding half the denominator */
        + PA_VOLUME_NORM / 2
    ) / PA_VOLUME_NORM);

    if (gain > A2DP_MAX_GAIN)
        gain = A2DP_MAX_GAIN;

    return gain;
}

struct pa_bluetooth_discovery {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_dbus_connection *connection;
    bool filter_added;
    bool matches_added;
    bool objects_listed;
    pa_hook hooks[PA_BLUETOOTH_HOOK_MAX];
    pa_hashmap *adapters;
    pa_hashmap *devices;
    pa_hashmap *transports;

    int headset_backend;
    pa_bluetooth_backend *ofono_backend, *native_backend;
    PA_LLIST_HEAD(pa_dbus_pending, pending);
    bool enable_native_hsp_hs;
    bool enable_native_hfp_hf;
    bool enable_msbc;
};

static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_discovery *y, DBusMessage *m,
                                                                  DBusPendingCallNotifyFunction func, void *call_data) {
    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(y);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(pa_dbus_connection_get(y->connection), m, &call, -1));

    p = pa_dbus_pending_new(pa_dbus_connection_get(y->connection), m, call, y, call_data);
    PA_LLIST_PREPEND(pa_dbus_pending, y->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static const char *check_variant_property(DBusMessageIter *i) {
    const char *key;

    pa_assert(i);

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_STRING) {
        pa_log_error("Property name not a string.");
        return NULL;
    }

    dbus_message_iter_get_basic(i, &key);

    if (!dbus_message_iter_next(i)) {
        pa_log_error("Property value missing");
        return NULL;
    }

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_VARIANT) {
        pa_log_error("Property value not a variant.");
        return NULL;
    }

    return key;
}

pa_bluetooth_transport *pa_bluetooth_transport_new(pa_bluetooth_device *d, const char *owner, const char *path,
                                                   pa_bluetooth_profile_t p, const uint8_t *config, size_t size) {
    pa_bluetooth_transport *t;

    t = pa_xnew0(pa_bluetooth_transport, 1);
    t->device = d;
    t->owner = pa_xstrdup(owner);
    t->path = pa_xstrdup(path);
    t->profile = p;
    t->config_size = size;
    /* Always force initial volume to be set/propagated correctly */
    t->sink_volume = PA_VOLUME_INVALID;
    t->source_volume = PA_VOLUME_INVALID;

    if (size > 0) {
        t->config = pa_xnew(uint8_t, size);
        if (config)
            memcpy(t->config, config, size);
        else
            memset(t->config, 0, size);
    }

    return t;
}

void pa_bluetooth_transport_reconfigure(pa_bluetooth_transport *t, const pa_bt_codec *bt_codec,
                                        pa_bluetooth_transport_write_cb write_cb, pa_bluetooth_transport_setsockopt_cb setsockopt_cb) {
    pa_assert(t);

    t->bt_codec = bt_codec;

    t->write = write_cb;
    t->setsockopt = setsockopt_cb;

    /* reset stream write type hint */
    t->stream_write_type = 0;

    /* reset SCO MTU adjustment hint */
    t->last_read_size = 0;
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

static bool device_supports_profile(pa_bluetooth_device *device, pa_bluetooth_profile_t profile) {
    bool show_hfp, show_hsp;

    if (device->enable_hfp_hf) {
        show_hfp = pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_HFP_HF);
        show_hsp = !show_hfp;
    } else {
        show_hfp = false;
        show_hsp = true;
    }

    switch (profile) {
        case PA_BLUETOOTH_PROFILE_A2DP_SINK:
            return !!pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_A2DP_SINK);
        case PA_BLUETOOTH_PROFILE_A2DP_SOURCE:
            return !!pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_A2DP_SOURCE);
        case PA_BLUETOOTH_PROFILE_HSP_HS:
            return show_hsp
                && ( !!pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_HSP_HS)
                  || !!pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_HSP_HS_ALT));
        case PA_BLUETOOTH_PROFILE_HSP_AG:
            return !!pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_HSP_AG);
        case PA_BLUETOOTH_PROFILE_HFP_HF:
            return show_hfp && !!pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_HFP_HF);
        case PA_BLUETOOTH_PROFILE_HFP_AG:
            return !!pa_hashmap_get(device->uuids, PA_BLUETOOTH_UUID_HFP_AG);
        case PA_BLUETOOTH_PROFILE_OFF:
            pa_assert_not_reached();
    }

    pa_assert_not_reached();
}

static bool device_is_profile_connected(pa_bluetooth_device *device, pa_bluetooth_profile_t profile) {
    if (device->transports[profile] && device->transports[profile]->state != PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED)
        return true;
    else
        return false;
}

static unsigned device_count_disconnected_profiles(pa_bluetooth_device *device) {
    pa_bluetooth_profile_t profile;
    unsigned count = 0;

    for (profile = 0; profile < PA_BLUETOOTH_PROFILE_COUNT; profile++) {
        if (!device_supports_profile(device, profile))
            continue;

        if (!device_is_profile_connected(device, profile))
            count++;
    }

    return count;
}

static void device_stop_waiting_for_profiles(pa_bluetooth_device *device) {
    if (!device->wait_for_profiles_timer)
        return;

    device->discovery->core->mainloop->time_free(device->wait_for_profiles_timer);
    device->wait_for_profiles_timer = NULL;
}

static void wait_for_profiles_cb(pa_mainloop_api *api, pa_time_event* event, const struct timeval *tv, void *userdata) {
    pa_bluetooth_device *device = userdata;
    pa_strbuf *buf;
    pa_bluetooth_profile_t profile;
    bool first = true;
    char *profiles_str;

    device_stop_waiting_for_profiles(device);

    buf = pa_strbuf_new();

    for (profile = 0; profile < PA_BLUETOOTH_PROFILE_COUNT; profile++) {
        if (device_is_profile_connected(device, profile))
            continue;

        if (!device_supports_profile(device, profile))
            continue;

        if (first)
            first = false;
        else
            pa_strbuf_puts(buf, ", ");

        pa_strbuf_puts(buf, pa_bluetooth_profile_to_string(profile));
    }

    profiles_str = pa_strbuf_to_string_free(buf);
    pa_log_debug("Timeout expired, and device %s still has disconnected profiles: %s",
                 device->path, profiles_str);
    pa_xfree(profiles_str);
    pa_hook_fire(&device->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], device);
}

static void device_start_waiting_for_profiles(pa_bluetooth_device *device) {
    pa_assert(!device->wait_for_profiles_timer);
    device->wait_for_profiles_timer = pa_core_rttime_new(device->discovery->core,
                                                         pa_rtclock_now() + WAIT_FOR_PROFILES_TIMEOUT_USEC,
                                                         wait_for_profiles_cb, device);
}

struct switch_codec_data {
    char *pa_endpoint;
    char *device_path;
    pa_bluetooth_profile_t profile;
    void (*cb)(bool, pa_bluetooth_profile_t profile, void *);
    void *userdata;
};

static void pa_bluetooth_device_switch_codec_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;
    pa_bluetooth_device *device;
    struct switch_codec_data *data;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(data = p->call_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);

    device = pa_hashmap_get(y->devices, data->device_path);
    if (!device) {
        pa_log_error("Changing codec for device %s with profile %s failed. Device is not connected anymore",
                data->device_path, pa_bluetooth_profile_to_string(data->profile));
        data->cb(false, data->profile, data->userdata);
    } else if (dbus_message_get_type(r) != DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_info("Changing codec for device %s with profile %s succeeded",
                data->device_path, pa_bluetooth_profile_to_string(data->profile));
        data->cb(true, data->profile, data->userdata);
    } else if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error("Changing codec for device %s with profile %s failed. Error: %s",
                data->device_path, pa_bluetooth_profile_to_string(data->profile),
                dbus_message_get_error_name(r));
    }

    dbus_message_unref(r);

    pa_xfree(data->pa_endpoint);
    pa_xfree(data->device_path);
    pa_xfree(data);

    device->codec_switching_in_progress = false;
}

bool pa_bluetooth_device_switch_codec(pa_bluetooth_device *device, pa_bluetooth_profile_t profile,
        pa_hashmap *capabilities_hashmap, const pa_a2dp_endpoint_conf *endpoint_conf,
        void (*codec_switch_cb)(bool, pa_bluetooth_profile_t profile, void *), void *userdata) {
    DBusMessageIter iter, dict;
    DBusMessage *m;
    struct switch_codec_data *data;
    pa_a2dp_codec_capabilities *capabilities;
    uint8_t config[MAX_A2DP_CAPS_SIZE];
    uint8_t config_size;
    bool is_a2dp_sink;
    pa_hashmap *all_endpoints;
    char *pa_endpoint;
    const char *endpoint;

    pa_assert(device);
    pa_assert(capabilities_hashmap);
    pa_assert(endpoint_conf);

    if (device->codec_switching_in_progress) {
        pa_log_error("Codec switching operation already in progress");
        return false;
    }

    is_a2dp_sink = profile == PA_BLUETOOTH_PROFILE_A2DP_SINK;

    all_endpoints = NULL;
    all_endpoints = pa_hashmap_get(is_a2dp_sink ? device->a2dp_sink_endpoints : device->a2dp_source_endpoints,
            &endpoint_conf->id);
    pa_assert(all_endpoints);

    pa_assert_se(endpoint = endpoint_conf->choose_remote_endpoint(capabilities_hashmap, &device->discovery->core->default_sample_spec, is_a2dp_sink));
    pa_assert_se(capabilities = pa_hashmap_get(all_endpoints, endpoint));

    config_size = endpoint_conf->fill_preferred_configuration(&device->discovery->core->default_sample_spec,
            capabilities->buffer, capabilities->size, config);
    if (config_size == 0)
        return false;

    pa_endpoint = pa_sprintf_malloc("%s/%s", is_a2dp_sink ? A2DP_SOURCE_ENDPOINT : A2DP_SINK_ENDPOINT,
            endpoint_conf->bt_codec.name);

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, endpoint,
                BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SetConfiguration"));

    dbus_message_iter_init_append(m, &iter);
    pa_assert_se(dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &pa_endpoint));
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
                                     DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING
                                     DBUS_TYPE_VARIANT_AS_STRING
                                     DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                     &dict);
    pa_dbus_append_basic_array_variant_dict_entry(&dict, "Capabilities", DBUS_TYPE_BYTE, &config, config_size);
    dbus_message_iter_close_container(&iter, &dict);

    device->codec_switching_in_progress = true;

    data = pa_xnew0(struct switch_codec_data, 1);
    data->pa_endpoint = pa_endpoint;
    data->device_path = pa_xstrdup(device->path);
    data->profile = profile;
    data->cb = codec_switch_cb;
    data->userdata = userdata;

    send_and_add_to_pending(device->discovery, m, pa_bluetooth_device_switch_codec_reply, data);

    return true;
}

void pa_bluetooth_transport_set_state(pa_bluetooth_transport *t, pa_bluetooth_transport_state_t state) {
    bool old_any_connected;
    unsigned n_disconnected_profiles;
    bool new_device_appeared;
    bool device_disconnected;

    pa_assert(t);

    if (t->state == state)
        return;

    old_any_connected = pa_bluetooth_device_any_transport_connected(t->device);

    pa_log_debug("Transport %s state: %s -> %s",
                 t->path, transport_state_to_string(t->state), transport_state_to_string(state));

    t->state = state;

    pa_hook_fire(&t->device->discovery->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_STATE_CHANGED], t);

    /* If there are profiles that are expected to get connected soon (based
     * on the UUID list), we wait for a bit before announcing the new
     * device, so that all profiles have time to get connected before the
     * card object is created. If we didn't wait, the card would always
     * have only one profile marked as available in the initial state,
     * which would prevent module-card-restore from restoring the initial
     * profile properly. */

    n_disconnected_profiles = device_count_disconnected_profiles(t->device);

    new_device_appeared = !old_any_connected && pa_bluetooth_device_any_transport_connected(t->device);
    device_disconnected = old_any_connected && !pa_bluetooth_device_any_transport_connected(t->device);

    if (new_device_appeared) {
        if (n_disconnected_profiles > 0)
            device_start_waiting_for_profiles(t->device);
        else
            pa_hook_fire(&t->device->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], t->device);
        return;
    }

    if (device_disconnected) {
        if (t->device->wait_for_profiles_timer) {
            /* If the timer is still running when the device disconnects, we
             * never sent the notification of the device getting connected, so
             * we don't need to send a notification about the disconnection
             * either. Let's just stop the timer. */
            device_stop_waiting_for_profiles(t->device);
        } else
            pa_hook_fire(&t->device->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], t->device);
        return;
    }

    if (n_disconnected_profiles == 0 && t->device->wait_for_profiles_timer) {
        /* All profiles are now connected, so we can stop the wait timer and
         * send a notification of the new device. */
        device_stop_waiting_for_profiles(t->device);
        pa_hook_fire(&t->device->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], t->device);
    }
}

static pa_volume_t pa_bluetooth_transport_set_volume(pa_bluetooth_transport *t, pa_volume_t volume) {
    static const char *volume_str = "Volume";
    static const char *mediatransport_str = BLUEZ_MEDIA_TRANSPORT_INTERFACE;
    DBusMessage *m;
    DBusMessageIter iter;
    uint16_t gain;

    pa_assert(t);
    pa_assert(t->device);
    pa_assert(pa_bluetooth_profile_is_a2dp(t->profile));
    pa_assert(t->device->discovery);

    gain = volume_to_a2dp_gain(volume);
    /* Propagate rounding and bound checks */
    volume = a2dp_gain_to_volume(gain);

    if (t->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE && t->source_volume == volume)
        return volume;
    else if (t->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK && t->sink_volume == volume)
        return volume;

    if (t->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE)
        t->source_volume = volume;
    else if (t->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK)
        t->sink_volume = volume;

    pa_log_debug("Sending A2DP volume %d/127 to peer", gain);

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, t->path, DBUS_INTERFACE_PROPERTIES, "Set"));

    dbus_message_iter_init_append(m, &iter);
    pa_assert_se(dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &mediatransport_str));
    pa_assert_se(dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &volume_str));
    pa_dbus_append_basic_variant(&iter, DBUS_TYPE_UINT16, &gain);

    /* Ignore replies, wait for the Volume property to change (generally arrives
     * before this function replies).
     *
     * In an ideal world BlueZ exposes a function to change volume, that returns
     * with the actual volume set by the peer as returned by the SetAbsoluteVolume
     * AVRCP command.  That is required later to perform software volume compensation
     * based on actual playback volume.
     */
    dbus_message_set_no_reply(m, true);
    pa_assert_se(dbus_connection_send(pa_dbus_connection_get(t->device->discovery->connection), m, NULL));
    dbus_message_unref(m);

    return volume;
}

static pa_volume_t pa_bluetooth_transport_set_sink_volume(pa_bluetooth_transport *t, pa_volume_t volume) {
    pa_assert(t);
    pa_assert(t->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK);
    return pa_bluetooth_transport_set_volume(t, volume);
}

static pa_volume_t pa_bluetooth_transport_set_source_volume(pa_bluetooth_transport *t, pa_volume_t volume) {
    pa_assert(t);
    pa_assert(t->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE);
    return pa_bluetooth_transport_set_volume(t, volume);
}

static void pa_bluetooth_transport_remote_volume_changed(pa_bluetooth_transport *t, pa_volume_t volume) {
    pa_bluetooth_hook_t hook;
    bool is_source;
    char volume_str[PA_VOLUME_SNPRINT_MAX];

    pa_assert(t);
    pa_assert(t->device);

    if (!t->device->avrcp_absolute_volume)
        return;

    is_source = t->profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE;

    if (is_source) {
        if (t->source_volume == volume)
            return;
        t->source_volume = volume;
        hook = PA_BLUETOOTH_HOOK_TRANSPORT_SOURCE_VOLUME_CHANGED;
    } else if (t->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK) {
        if (t->sink_volume == volume)
            return;
        t->sink_volume = volume;
        hook = PA_BLUETOOTH_HOOK_TRANSPORT_SINK_VOLUME_CHANGED;

        /* A2DP Absolute Volume is optional.  This callback is only
         * attached when the peer supports it, and the hook handler
         * further attaches the necessary hardware callback to the
         * pa_sink and disables software attenuation.
         */
        if (!t->set_sink_volume) {
            pa_log_debug("A2DP sink supports volume control");
            t->set_sink_volume = pa_bluetooth_transport_set_sink_volume;
        }
    } else {
        pa_assert_not_reached();
    }

    pa_log_debug("Reporting volume change %s for %s",
                 pa_volume_snprint(volume_str, sizeof(volume_str), volume),
                 is_source ? "source" : "sink");

    pa_hook_fire(pa_bluetooth_discovery_hook(t->device->discovery, hook), t);
}

void pa_bluetooth_transport_put(pa_bluetooth_transport *t) {
    pa_assert(t);

    t->device->transports[t->profile] = t;
    pa_assert_se(pa_hashmap_put(t->device->discovery->transports, t->path, t) >= 0);
    pa_bluetooth_transport_set_state(t, PA_BLUETOOTH_TRANSPORT_STATE_IDLE);
}

void pa_bluetooth_transport_unlink(pa_bluetooth_transport *t) {
    pa_assert(t);

    pa_bluetooth_transport_set_state(t, PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED);
    pa_hashmap_remove(t->device->discovery->transports, t->path);
    t->device->transports[t->profile] = NULL;
}

void pa_bluetooth_transport_free(pa_bluetooth_transport *t) {
    pa_assert(t);

    if (t->destroy)
        t->destroy(t);
    pa_bluetooth_transport_unlink(t);

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
    dbus_message_unref(m);
    m = NULL;
    if (!r) {
        if (optional && pa_streq(err.name, BLUEZ_ERROR_NOT_AVAILABLE))
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
    DBusMessage *m, *r;
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
    r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(t->device->discovery->connection), m, -1, &err);
    dbus_message_unref(m);
    m = NULL;
    if (r) {
        dbus_message_unref(r);
        r = NULL;
    }

    if (dbus_error_is_set(&err)) {
        pa_log_error("Failed to release transport %s: %s", t->path, err.message);
        dbus_error_free(&err);
    } else
        pa_log_info("Transport %s released", t->path);
}

static void get_volume_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    DBusMessageIter iter, variant;
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;
    pa_bluetooth_transport *t;
    uint16_t gain;
    pa_volume_t volume;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(t = p->call_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error(DBUS_INTERFACE_PROPERTIES ".Get %s Volume failed: %s: %s",
                     dbus_message_get_path(p->message),
                     dbus_message_get_error_name(r),
                     pa_dbus_get_error_message(r));
        goto finish;
    }
    dbus_message_iter_init(r, &iter);
    pa_assert(dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT);
    dbus_message_iter_recurse(&iter, &variant);
    pa_assert(dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT16);
    dbus_message_iter_get_basic(&variant, &gain);

    if (gain > A2DP_MAX_GAIN)
        gain = A2DP_MAX_GAIN;

    pa_log_debug("Received A2DP Absolute Volume %d", gain);

    volume = a2dp_gain_to_volume(gain);

    pa_bluetooth_transport_remote_volume_changed(t, volume);

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static void bluez5_transport_get_volume(pa_bluetooth_transport *t) {
    static const char *volume_str = "Volume";
    static const char *mediatransport_str = BLUEZ_MEDIA_TRANSPORT_INTERFACE;
    DBusMessage *m;

    pa_assert(t);
    pa_assert(t->device);
    pa_assert(t->device->discovery);

    pa_assert(pa_bluetooth_profile_is_a2dp(t->profile));

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, t->path, DBUS_INTERFACE_PROPERTIES, "Get"));
    pa_assert_se(dbus_message_append_args(m,
        DBUS_TYPE_STRING, &mediatransport_str,
        DBUS_TYPE_STRING, &volume_str,
        DBUS_TYPE_INVALID));

    send_and_add_to_pending(t->device->discovery, m, get_volume_reply, t);
}

void pa_bluetooth_transport_load_a2dp_sink_volume(pa_bluetooth_transport *t) {
    pa_assert(t);
    pa_assert(t->device);

    if (!t->device->avrcp_absolute_volume)
        return;

    if (t->profile == PA_BLUETOOTH_PROFILE_A2DP_SINK)
        /* A2DP Absolute Volume control (AVRCP 1.4) is optional */
        bluez5_transport_get_volume(t);
}

static ssize_t a2dp_transport_write(pa_bluetooth_transport *t, int fd, const void* buffer, size_t size, size_t write_mtu) {
    ssize_t l = 0;
    size_t written = 0;
    size_t write_size;

    pa_assert(t);

    while (written < size) {
        write_size = PA_MIN(size - written, write_mtu);
        l = pa_write(fd, buffer + written, write_size, &t->stream_write_type);
        if (l < 0)
            break;
        written += l;
    }

    if (l < 0) {
        if (errno == EAGAIN) {
            /* Hmm, apparently the socket was not writable, give up for now */
            pa_log_debug("Got EAGAIN on write() after POLLOUT, probably there is a temporary connection loss.");
            /* Drain write buffer */
            written = size;
        } else {
            pa_log_error("Failed to write data to socket: %s", pa_cstrerror(errno));
            /* Report error from write call */
            return -1;
        }
    }

    /* if too much data left discard it all */
    if (size - written >= write_mtu) {
        pa_log_warn("Wrote memory block to socket only partially! %lu written, discarding pending write size %lu larger than write_mtu %lu",
                    written, size, write_mtu);
        /* Drain write buffer */
        written = size;
    }

    return written;
}

bool pa_bluetooth_device_any_transport_connected(const pa_bluetooth_device *d) {
    unsigned i;

    pa_assert(d);

    if (!d->valid)
        return false;

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++)
        if (d->transports[i] && d->transports[i]->state != PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED)
            return true;

    return false;
}

static int transport_state_from_string(const char* value, pa_bluetooth_transport_state_t *state) {
    pa_assert(value);
    pa_assert(state);

    if (pa_streq(value, "idle"))
        *state = PA_BLUETOOTH_TRANSPORT_STATE_IDLE;
    else if (pa_streq(value, "pending") || pa_streq(value, "active"))
        *state = PA_BLUETOOTH_TRANSPORT_STATE_PLAYING;
    else
        return -1;

    return 0;
}

static void parse_transport_property(pa_bluetooth_transport *t, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    key = check_variant_property(i);
    if (key == NULL)
        return;

    pa_log_debug("Transport property %s changed", key);

    dbus_message_iter_recurse(i, &variant_i);

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_STRING: {

            const char *value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "State")) {
                pa_bluetooth_transport_state_t state;

                if (transport_state_from_string(value, &state) < 0) {
                    pa_log_error("Invalid state received: %s", value);
                    return;
                }

                pa_bluetooth_transport_set_state(t, state);
            }

            break;
        }

        case DBUS_TYPE_UINT16: {
            uint16_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Volume")) {
                pa_volume_t volume = a2dp_gain_to_volume(value);
                pa_bluetooth_transport_remote_volume_changed(t, volume);
            }
            break;
        }
    }

    return;
}

static int parse_transport_properties(pa_bluetooth_transport *t, DBusMessageIter *i) {
    DBusMessageIter element_i;

    dbus_message_iter_recurse(i, &element_i);

    while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_i;

        dbus_message_iter_recurse(&element_i, &dict_i);

        parse_transport_property(t, &dict_i);

        dbus_message_iter_next(&element_i);
    }

    return 0;
}

static unsigned pa_a2dp_codec_id_hash_func(const void *_p) {
    unsigned hash;
    const pa_a2dp_codec_id *p = _p;

    hash = p->codec_id;
    hash = 31 * hash + ((p->vendor_id >>  0) & 0xFF);
    hash = 31 * hash + ((p->vendor_id >>  8) & 0xFF);
    hash = 31 * hash + ((p->vendor_id >> 16) & 0xFF);
    hash = 31 * hash + ((p->vendor_id >> 24) & 0xFF);
    hash = 31 * hash + ((p->vendor_codec_id >> 0) & 0xFF);
    hash = 31 * hash + ((p->vendor_codec_id >> 8) & 0xFF);
    return hash;
}

static int pa_a2dp_codec_id_compare_func(const void *_a, const void *_b) {
    const pa_a2dp_codec_id *a = _a;
    const pa_a2dp_codec_id *b = _b;

    if (a->codec_id < b->codec_id)
        return -1;
    if (a->codec_id > b->codec_id)
        return 1;

    if (a->vendor_id < b->vendor_id)
        return -1;
    if (a->vendor_id > b->vendor_id)
        return 1;

    if (a->vendor_codec_id < b->vendor_codec_id)
        return -1;
    if (a->vendor_codec_id > b->vendor_codec_id)
        return 1;

    return 0;
}

static void remote_endpoint_remove(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_device *device;
    pa_hashmap *endpoints;
    void *devices_state;
    void *state;

    PA_HASHMAP_FOREACH(device, y->devices, devices_state) {
        PA_HASHMAP_FOREACH(endpoints, device->a2dp_sink_endpoints, state)
            pa_hashmap_remove_and_free(endpoints, path);

        PA_HASHMAP_FOREACH(endpoints, device->a2dp_source_endpoints, state)
            pa_hashmap_remove_and_free(endpoints, path);
    }

    pa_log_debug("Remote endpoint %s was removed", path);
}

static pa_bluetooth_device* device_create(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(path);

    d = pa_xnew0(pa_bluetooth_device, 1);
    d->discovery = y;
    d->enable_hfp_hf = pa_bluetooth_discovery_get_enable_native_hfp_hf(y);
    d->path = pa_xstrdup(path);
    d->uuids = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, pa_xfree);
    d->a2dp_sink_endpoints = pa_hashmap_new_full(pa_a2dp_codec_id_hash_func, pa_a2dp_codec_id_compare_func, pa_xfree, (pa_free_cb_t)pa_hashmap_free);
    d->a2dp_source_endpoints = pa_hashmap_new_full(pa_a2dp_codec_id_hash_func, pa_a2dp_codec_id_compare_func, pa_xfree, (pa_free_cb_t)pa_hashmap_free);

    pa_hashmap_put(y->devices, d->path, d);

    return d;
}

pa_bluetooth_device* pa_bluetooth_discovery_get_device_by_path(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(path);

    if ((d = pa_hashmap_get(y->devices, path)) && d->valid)
        return d;

    return NULL;
}

bool pa_bluetooth_discovery_get_enable_native_hsp_hs(pa_bluetooth_discovery *y)
{
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    return y->enable_native_hsp_hs;
}

bool pa_bluetooth_discovery_get_enable_native_hfp_hf(pa_bluetooth_discovery *y)
{
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    return y->enable_native_hfp_hf;
}

bool pa_bluetooth_discovery_get_enable_msbc(pa_bluetooth_discovery *y)
{
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    return y->enable_msbc;
}

pa_bluetooth_device* pa_bluetooth_discovery_get_device_by_address(pa_bluetooth_discovery *y, const char *remote, const char *local) {
    pa_bluetooth_device *d;
    void *state = NULL;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(remote);
    pa_assert(local);

    while ((d = pa_hashmap_iterate(y->devices, &state, NULL)))
        if (d->valid && pa_streq(d->address, remote) && pa_streq(d->adapter->address, local))
            return d;

    return NULL;
}

static void device_free(pa_bluetooth_device *d) {
    unsigned i;

    pa_assert(d);

    device_stop_waiting_for_profiles(d);

    pa_hook_fire(&d->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_UNLINK], d);

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++) {
        pa_bluetooth_transport *t;

        if (!(t = d->transports[i]))
            continue;

        pa_bluetooth_transport_free(t);
    }

    if (d->uuids)
        pa_hashmap_free(d->uuids);
    if (d->a2dp_sink_endpoints)
        pa_hashmap_free(d->a2dp_sink_endpoints);
    if (d->a2dp_source_endpoints)
        pa_hashmap_free(d->a2dp_source_endpoints);

    pa_xfree(d->path);
    pa_xfree(d->alias);
    pa_xfree(d->address);
    pa_xfree(d->adapter_path);
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

static void device_set_valid(pa_bluetooth_device *device, bool valid) {
    bool old_any_connected;

    pa_assert(device);

    if (valid == device->valid)
        return;

    old_any_connected = pa_bluetooth_device_any_transport_connected(device);
    device->valid = valid;

    if (pa_bluetooth_device_any_transport_connected(device) != old_any_connected)
        pa_hook_fire(&device->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], device);
}

static void device_update_valid(pa_bluetooth_device *d) {
    pa_assert(d);

    if (!d->properties_received) {
        pa_assert(!d->valid);
        return;
    }

    /* Check if mandatory properties are set. */
    if (!d->address || !d->adapter_path || !d->alias) {
        device_set_valid(d, false);
        return;
    }

    if (!d->adapter || !d->adapter->valid) {
        device_set_valid(d, false);
        return;
    }

    device_set_valid(d, true);
}

static void device_set_adapter(pa_bluetooth_device *device, pa_bluetooth_adapter *adapter) {
    pa_assert(device);

    if (adapter == device->adapter)
        return;

    device->adapter = adapter;

    device_update_valid(device);
}

static pa_bluetooth_adapter* adapter_create(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_adapter *a;

    pa_assert(y);
    pa_assert(path);

    a = pa_xnew0(pa_bluetooth_adapter, 1);
    a->discovery = y;
    a->path = pa_xstrdup(path);
    a->uuids = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, pa_xfree);

    pa_hashmap_put(y->adapters, a->path, a);

    return a;
}

static void adapter_free(pa_bluetooth_adapter *a) {
    pa_bluetooth_device *d;
    void *state;

    pa_assert(a);
    pa_assert(a->discovery);

    PA_HASHMAP_FOREACH(d, a->discovery->devices, state)
        if (d->adapter == a)
            device_set_adapter(d, NULL);

    pa_xfree(a->path);
    pa_xfree(a->address);
    pa_xfree(a);
}

static void adapter_remove(pa_bluetooth_discovery *y, const char *path) {
    pa_bluetooth_adapter *a;

    if (!(a = pa_hashmap_remove(y->adapters, path)))
        pa_log_warn("Unknown adapter removed %s", path);
    else {
        pa_log_debug("Adapter %s removed", path);
        adapter_free(a);
    }
}

static void parse_device_property(pa_bluetooth_device *d, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(d);

    key = check_variant_property(i);
    if (key == NULL) {
        pa_log_error("Received invalid property for device %s", d->path);
        return;
    }

    dbus_message_iter_recurse(i, &variant_i);

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_STRING: {
            const char *value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Alias")) {
                pa_xfree(d->alias);
                d->alias = pa_xstrdup(value);
                pa_log_debug("%s: %s", key, value);
            } else if (pa_streq(key, "Address")) {
                if (d->properties_received) {
                    pa_log_warn("Device property 'Address' expected to be constant but changed for %s, ignoring", d->path);
                    return;
                }

                if (d->address) {
                    pa_log_warn("Device %s: Received a duplicate 'Address' property, ignoring", d->path);
                    return;
                }

                d->address = pa_xstrdup(value);
                pa_log_debug("%s: %s", key, value);
            }

            break;
        }

        case DBUS_TYPE_OBJECT_PATH: {
            const char *value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Adapter")) {

                if (d->properties_received) {
                    pa_log_warn("Device property 'Adapter' expected to be constant but changed for %s, ignoring", d->path);
                    return;
                }

                if (d->adapter_path) {
                    pa_log_warn("Device %s: Received a duplicate 'Adapter' property, ignoring", d->path);
                    return;
                }

                d->adapter_path = pa_xstrdup(value);
                pa_log_debug("%s: %s", key, value);
            }

            break;
        }

        case DBUS_TYPE_UINT32: {
            uint32_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Class")) {
                d->class_of_device = value;
                pa_log_debug("%s: %d", key, value);
            }

            break;
        }

        case DBUS_TYPE_ARRAY: {
            DBusMessageIter ai;
            dbus_message_iter_recurse(&variant_i, &ai);

            if (dbus_message_iter_get_arg_type(&ai) == DBUS_TYPE_STRING && pa_streq(key, "UUIDs")) {
                /* bluetoothd never removes UUIDs from a device object so we
                 * don't need to check for disappeared UUIDs here. */
                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    const char *value;
                    char *uuid;

                    dbus_message_iter_get_basic(&ai, &value);

                    if (pa_hashmap_get(d->uuids, value)) {
                        dbus_message_iter_next(&ai);
                        continue;
                    }

                    uuid = pa_xstrdup(value);
                    pa_hashmap_put(d->uuids, uuid, uuid);

                    pa_log_debug("%s: %s", key, value);
                    dbus_message_iter_next(&ai);
                }
            }

            break;
        }
    }
}

static void parse_device_properties(pa_bluetooth_device *d, DBusMessageIter *i) {
    DBusMessageIter element_i;

    dbus_message_iter_recurse(i, &element_i);

    while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_i;

        dbus_message_iter_recurse(&element_i, &dict_i);
        parse_device_property(d, &dict_i);
        dbus_message_iter_next(&element_i);
    }

    if (!d->properties_received) {
        d->properties_received = true;
        device_update_valid(d);

        if (!d->address || !d->adapter_path || !d->alias)
            pa_log_error("Non-optional information missing for device %s", d->path);
    }
}

static void parse_adapter_properties(pa_bluetooth_adapter *a, DBusMessageIter *i, bool is_property_change) {
    DBusMessageIter element_i;

    pa_assert(a);

    dbus_message_iter_recurse(i, &element_i);

    while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_i, variant_i;
        const char *key;

        dbus_message_iter_recurse(&element_i, &dict_i);

        key = check_variant_property(&dict_i);
        if (key == NULL) {
            pa_log_error("Received invalid property for adapter %s", a->path);
            return;
        }

        dbus_message_iter_recurse(&dict_i, &variant_i);

        if (dbus_message_iter_get_arg_type(&variant_i) == DBUS_TYPE_STRING && pa_streq(key, "Address")) {
            const char *value;

            if (is_property_change) {
                pa_log_warn("Adapter property 'Address' expected to be constant but changed for %s, ignoring", a->path);
                return;
            }

            if (a->address) {
                pa_log_warn("Adapter %s received a duplicate 'Address' property, ignoring", a->path);
                return;
            }

            dbus_message_iter_get_basic(&variant_i, &value);
            a->address = pa_xstrdup(value);
            a->valid = true;
        } else if (dbus_message_iter_get_arg_type(&variant_i) == DBUS_TYPE_ARRAY) {
            DBusMessageIter ai;
            dbus_message_iter_recurse(&variant_i, &ai);

            if (dbus_message_iter_get_arg_type(&ai) == DBUS_TYPE_STRING && pa_streq(key, "UUIDs")) {
                pa_hashmap_remove_all(a->uuids);
                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    const char *value;
                    char *uuid;

                    dbus_message_iter_get_basic(&ai, &value);

                    if (pa_hashmap_get(a->uuids, value)) {
                        dbus_message_iter_next(&ai);
                        continue;
                    }

                    uuid = pa_xstrdup(value);
                    pa_hashmap_put(a->uuids, uuid, uuid);

                    pa_log_debug("%s: %s", key, value);
                    dbus_message_iter_next(&ai);
                }
            }
        }

        dbus_message_iter_next(&element_i);
    }
}

static void register_legacy_sbc_endpoint_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;
    char *endpoint;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(endpoint = p->call_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
        pa_log_info("Couldn't register endpoint %s because it is disabled in BlueZ", endpoint);
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error(BLUEZ_MEDIA_INTERFACE ".RegisterEndpoint() failed: %s: %s", dbus_message_get_error_name(r),
                     pa_dbus_get_error_message(r));
        goto finish;
    }

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);

    pa_xfree(endpoint);
}

static void register_legacy_sbc_endpoint(pa_bluetooth_discovery *y, const pa_a2dp_endpoint_conf *endpoint_conf, const char *path, const char *endpoint, const char *uuid) {
    DBusMessage *m;
    DBusMessageIter i, d;
    uint8_t capabilities[MAX_A2DP_CAPS_SIZE];
    size_t capabilities_size;
    uint8_t codec_id;

    pa_log_debug("Registering %s on adapter %s", endpoint, path);

    codec_id = endpoint_conf->id.codec_id;
    capabilities_size = endpoint_conf->fill_capabilities(capabilities);
    pa_assert(capabilities_size != 0);

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, path, BLUEZ_MEDIA_INTERFACE, "RegisterEndpoint"));

    dbus_message_iter_init_append(m, &i);
    pa_assert_se(dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &endpoint));
    dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
                                     DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING
                                     DBUS_TYPE_VARIANT_AS_STRING
                                     DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                     &d);
    pa_dbus_append_basic_variant_dict_entry(&d, "UUID", DBUS_TYPE_STRING, &uuid);
    pa_dbus_append_basic_variant_dict_entry(&d, "Codec", DBUS_TYPE_BYTE, &codec_id);
    pa_dbus_append_basic_array_variant_dict_entry(&d, "Capabilities", DBUS_TYPE_BYTE, &capabilities, capabilities_size);

    dbus_message_iter_close_container(&i, &d);

    send_and_add_to_pending(y, m, register_legacy_sbc_endpoint_reply, pa_xstrdup(endpoint));
}

static void register_application_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_adapter *a;
    pa_bluetooth_discovery *y;
    char *path;
    bool fallback = true;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(path = p->call_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
        pa_log_info("Couldn't register media application for adapter %s because it is disabled in BlueZ", path);
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_warn(BLUEZ_MEDIA_INTERFACE ".RegisterApplication() failed: %s: %s",
                dbus_message_get_error_name(r), pa_dbus_get_error_message(r));
        pa_log_warn("Couldn't register media application for adapter %s", path);
        goto finish;
    }

    a = pa_hashmap_get(y->adapters, path);
    if (!a) {
        pa_log_error("Couldn't register media application for adapter %s because it does not exist anymore", path);
        goto finish;
    }

    fallback = false;
    a->application_registered = true;
    pa_log_debug("Media application for adapter %s was successfully registered", path);

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);

    if (fallback) {
        /* If bluez does not support RegisterApplication, fallback to old legacy API with just one SBC codec */
        const pa_a2dp_endpoint_conf *endpoint_conf;
        endpoint_conf = pa_bluetooth_get_a2dp_endpoint_conf("sbc");
        pa_assert(endpoint_conf);
        register_legacy_sbc_endpoint(y, endpoint_conf, path, A2DP_SINK_ENDPOINT "/sbc",
                PA_BLUETOOTH_UUID_A2DP_SINK);
        register_legacy_sbc_endpoint(y, endpoint_conf, path, A2DP_SOURCE_ENDPOINT "/sbc",
                PA_BLUETOOTH_UUID_A2DP_SOURCE);
        pa_log_warn("Only SBC codec is available for A2DP profiles");
    }

    pa_xfree(path);
}

static void register_application(pa_bluetooth_adapter *a) {
    DBusMessage *m;
    DBusMessageIter i, d;
    const char *object_manager_path = A2DP_OBJECT_MANAGER_PATH;

    if (a->application_registered) {
        pa_log_info("Media application is already registered for adapter %s", a->path);
        return;
    }

    pa_log_debug("Registering media application for adapter %s", a->path);

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, a->path,
                BLUEZ_MEDIA_INTERFACE, "RegisterApplication"));

    dbus_message_iter_init_append(m, &i);
    pa_assert_se(dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &object_manager_path));
    dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
                                     DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING
                                     DBUS_TYPE_VARIANT_AS_STRING
                                     DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                     &d);
    dbus_message_iter_close_container(&i, &d);

    send_and_add_to_pending(a->discovery, m, register_application_reply, pa_xstrdup(a->path));
}

static void parse_remote_endpoint_properties(pa_bluetooth_discovery *y, const char *endpoint, DBusMessageIter *i) {
    DBusMessageIter element_i;
    pa_bluetooth_device *device;
    pa_hashmap *codec_endpoints;
    pa_hashmap *endpoints;
    pa_a2dp_codec_id *a2dp_codec_id;
    pa_a2dp_codec_capabilities *a2dp_codec_capabilities;
    const char *uuid = NULL;
    const char *device_path = NULL;
    uint8_t codec_id = 0;
    bool have_codec_id = false;
    const uint8_t *capabilities = NULL;
    int capabilities_size = 0;

    pa_log_debug("Parsing remote endpoint %s", endpoint);

    dbus_message_iter_recurse(i, &element_i);

    while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_i, variant_i;
        const char *key;

        dbus_message_iter_recurse(&element_i, &dict_i);

        key = check_variant_property(&dict_i);
        if (key == NULL) {
            pa_log_error("Received invalid property for remote endpoint %s", endpoint);
            return;
        }

        dbus_message_iter_recurse(&dict_i, &variant_i);

        if (pa_streq(key, "UUID")) {
            if (dbus_message_iter_get_arg_type(&variant_i) != DBUS_TYPE_STRING) {
                pa_log_warn("Remote endpoint %s property 'UUID' is not string, ignoring", endpoint);
                return;
            }

            dbus_message_iter_get_basic(&variant_i, &uuid);
        } else if (pa_streq(key, "Codec")) {
            if (dbus_message_iter_get_arg_type(&variant_i) != DBUS_TYPE_BYTE) {
                pa_log_warn("Remote endpoint %s property 'Codec' is not byte, ignoring", endpoint);
                return;
            }

            dbus_message_iter_get_basic(&variant_i, &codec_id);
            have_codec_id = true;
        } else if (pa_streq(key, "Capabilities")) {
            DBusMessageIter array;

            if (dbus_message_iter_get_arg_type(&variant_i) != DBUS_TYPE_ARRAY) {
                pa_log_warn("Remote endpoint %s property 'Capabilities' is not array, ignoring", endpoint);
                return;
            }

            dbus_message_iter_recurse(&variant_i, &array);
            if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_BYTE) {
                pa_log_warn("Remote endpoint %s property 'Capabilities' is not array of bytes, ignoring", endpoint);
                return;
            }

            dbus_message_iter_get_fixed_array(&array, &capabilities, &capabilities_size);
        } else if (pa_streq(key, "Device")) {
            if (dbus_message_iter_get_arg_type(&variant_i) != DBUS_TYPE_OBJECT_PATH) {
                pa_log_warn("Remote endpoint %s property 'Device' is not path, ignoring", endpoint);
                return;
            }

            dbus_message_iter_get_basic(&variant_i, &device_path);
        }

        dbus_message_iter_next(&element_i);
    }

    if (!uuid) {
        pa_log_warn("Remote endpoint %s does not have property 'UUID', ignoring", endpoint);
        return;
    }

    if (!have_codec_id) {
        pa_log_warn("Remote endpoint %s does not have property 'Codec', ignoring", endpoint);
        return;
    }

    if (!capabilities || !capabilities_size) {
        pa_log_warn("Remote endpoint %s does not have property 'Capabilities', ignoring", endpoint);
        return;
    }

    if (!device_path) {
        pa_log_warn("Remote endpoint %s does not have property 'Device', ignoring", endpoint);
        return;
    }

    device = pa_hashmap_get(y->devices, device_path);
    if (!device) {
        pa_log_warn("Device for remote endpoint %s was not found", endpoint);
        return;
    }

    if (pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SINK)) {
        codec_endpoints = device->a2dp_sink_endpoints;
    } else if (pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SOURCE)) {
        codec_endpoints = device->a2dp_source_endpoints;
    } else {
        pa_log_warn("Remote endpoint %s does not have valid property 'UUID', ignoring", endpoint);
        return;
    }

    if (capabilities_size < 0 || capabilities_size > MAX_A2DP_CAPS_SIZE) {
        pa_log_warn("Remote endpoint %s does not have valid property 'Capabilities', ignoring", endpoint);
        return;
    }

    a2dp_codec_id = pa_xmalloc0(sizeof(*a2dp_codec_id));
    a2dp_codec_id->codec_id = codec_id;
    if (codec_id == A2DP_CODEC_VENDOR) {
        if ((size_t)capabilities_size < sizeof(a2dp_vendor_codec_t)) {
            pa_log_warn("Remote endpoint %s does not have valid property 'Capabilities', ignoring", endpoint);
            pa_xfree(a2dp_codec_id);
            return;
        }
        a2dp_codec_id->vendor_id = A2DP_GET_VENDOR_ID(*(a2dp_vendor_codec_t *)capabilities);
        a2dp_codec_id->vendor_codec_id = A2DP_GET_CODEC_ID(*(a2dp_vendor_codec_t *)capabilities);
    } else {
        a2dp_codec_id->vendor_id = 0;
        a2dp_codec_id->vendor_codec_id = 0;
    }

    if (!pa_bluetooth_a2dp_codec_is_available(a2dp_codec_id, pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SINK))) {
        pa_xfree(a2dp_codec_id);
        return;
    }

    a2dp_codec_capabilities = pa_xmalloc0(sizeof(*a2dp_codec_capabilities) + capabilities_size);
    a2dp_codec_capabilities->size = capabilities_size;
    memcpy(a2dp_codec_capabilities->buffer, capabilities, capabilities_size);

    endpoints = pa_hashmap_get(codec_endpoints, a2dp_codec_id);
    if (!endpoints) {
        endpoints = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, pa_xfree, pa_xfree);
        pa_hashmap_put(codec_endpoints, a2dp_codec_id, endpoints);
    }

    if (pa_hashmap_remove_and_free(endpoints, endpoint) >= 0)
        pa_log_debug("Replacing existing remote endpoint %s", endpoint);
    pa_hashmap_put(endpoints, pa_xstrdup(endpoint), a2dp_codec_capabilities);
}

static void parse_interfaces_and_properties(pa_bluetooth_discovery *y, DBusMessageIter *dict_i) {
    DBusMessageIter element_i;
    const char *path;
    void *state;
    pa_bluetooth_device *d;

    pa_assert(dbus_message_iter_get_arg_type(dict_i) == DBUS_TYPE_OBJECT_PATH);
    dbus_message_iter_get_basic(dict_i, &path);

    pa_assert_se(dbus_message_iter_next(dict_i));
    pa_assert(dbus_message_iter_get_arg_type(dict_i) == DBUS_TYPE_ARRAY);

    dbus_message_iter_recurse(dict_i, &element_i);

    while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter iface_i;
        const char *interface;

        dbus_message_iter_recurse(&element_i, &iface_i);

        pa_assert(dbus_message_iter_get_arg_type(&iface_i) == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(&iface_i, &interface);

        pa_assert_se(dbus_message_iter_next(&iface_i));
        pa_assert(dbus_message_iter_get_arg_type(&iface_i) == DBUS_TYPE_ARRAY);

        if (pa_streq(interface, BLUEZ_ADAPTER_INTERFACE)) {
            pa_bluetooth_adapter *a;

            if ((a = pa_hashmap_get(y->adapters, path))) {
                pa_log_error("Found duplicated D-Bus path for adapter %s", path);
                return;
            } else
                a = adapter_create(y, path);

            pa_log_debug("Adapter %s found", path);

            parse_adapter_properties(a, &iface_i, false);

            if (!a->valid)
                return;

            register_application(a);
        } else if (pa_streq(interface, BLUEZ_DEVICE_INTERFACE)) {

            if ((d = pa_hashmap_get(y->devices, path))) {
                if (d->properties_received) {
                    pa_log_error("Found duplicated D-Bus path for device %s", path);
                    return;
                }
            } else
                d = device_create(y, path);

            pa_log_debug("Device %s found", d->path);

            parse_device_properties(d, &iface_i);
        } else if (pa_streq(interface, BLUEZ_MEDIA_ENDPOINT_INTERFACE)) {
            parse_remote_endpoint_properties(y, path, &iface_i);
        } else
            pa_log_debug("Unknown interface %s found, skipping", interface);

        dbus_message_iter_next(&element_i);
    }

    PA_HASHMAP_FOREACH(d, y->devices, state) {
        if (d->properties_received && !d->tried_to_link_with_adapter) {
            if (d->adapter_path) {
                device_set_adapter(d, pa_hashmap_get(d->discovery->adapters, d->adapter_path));

                if (!d->adapter)
                    pa_log("Device %s points to a nonexistent adapter %s.", d->path, d->adapter_path);
                else if (!d->adapter->valid)
                    pa_log("Device %s points to an invalid adapter %s.", d->path, d->adapter_path);
            }

            d->tried_to_link_with_adapter = true;
        }
    }

    return;
}

void pa_bluetooth_discovery_set_ofono_running(pa_bluetooth_discovery *y, bool is_running) {
    pa_assert(y);

    pa_log_debug("oFono is running: %s", pa_yes_no(is_running));
    if (y->headset_backend != HEADSET_BACKEND_AUTO)
        return;

    pa_bluetooth_native_backend_enable_shared_profiles(y->native_backend, !is_running);

    /* If ofono starts running, all devices that might be connected to the HS roles or HFP AG role
     * need to be disconnected, so that the devices can be handled by ofono */
    if (is_running) {
        void *state;
        pa_bluetooth_device *d;

        PA_HASHMAP_FOREACH(d, y->devices, state) {
            if (device_supports_profile(d, PA_BLUETOOTH_PROFILE_HFP_AG) || device_supports_profile(d, PA_BLUETOOTH_PROFILE_HFP_HF)) {
                DBusMessage *m;

                pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, d->path, BLUEZ_DEVICE_INTERFACE, "Disconnect"));
                dbus_message_set_no_reply(m, true);
                pa_assert_se(dbus_connection_send(pa_dbus_connection_get(y->connection), m, NULL));
                dbus_message_unref(m);
            }
        }
    }
}

static void get_managed_objects_reply(DBusPendingCall *pending, void *userdata) {
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;
    DBusMessage *r;
    DBusMessageIter arg_i, element_i;

    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
        pa_log_warn("BlueZ D-Bus ObjectManager not available");
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error("GetManagedObjects() failed: %s: %s", dbus_message_get_error_name(r), pa_dbus_get_error_message(r));
        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i) || !pa_streq(dbus_message_get_signature(r), "a{oa{sa{sv}}}")) {
        pa_log_error("Invalid reply signature for GetManagedObjects()");
        goto finish;
    }

    dbus_message_iter_recurse(&arg_i, &element_i);
    while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_i;

        dbus_message_iter_recurse(&element_i, &dict_i);

        parse_interfaces_and_properties(y, &dict_i);

        dbus_message_iter_next(&element_i);
    }

    y->objects_listed = true;

    if (!y->native_backend && y->headset_backend != HEADSET_BACKEND_OFONO)
        y->native_backend = pa_bluetooth_native_backend_new(y->core, y, (y->headset_backend == HEADSET_BACKEND_NATIVE));
    if (!y->ofono_backend && y->headset_backend != HEADSET_BACKEND_NATIVE)
        y->ofono_backend = pa_bluetooth_ofono_backend_new(y->core, y);

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static void get_managed_objects(pa_bluetooth_discovery *y) {
    DBusMessage *m;

    pa_assert(y);

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, "/", DBUS_INTERFACE_OBJECT_MANAGER,
                                                  "GetManagedObjects"));
    send_and_add_to_pending(y, m, get_managed_objects_reply, NULL);
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

    if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;

        if (!dbus_message_get_args(m, &err,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            pa_log_error("Failed to parse " DBUS_INTERFACE_DBUS ".NameOwnerChanged: %s", err.message);
            goto fail;
        }

        if (pa_streq(name, BLUEZ_SERVICE)) {
            if (old_owner && *old_owner) {
                pa_log_debug("Bluetooth daemon disappeared");
                pa_hashmap_remove_all(y->devices);
                pa_hashmap_remove_all(y->adapters);
                y->objects_listed = false;
                if (y->ofono_backend) {
                    pa_bluetooth_ofono_backend_free(y->ofono_backend);
                    y->ofono_backend = NULL;
                }
                if (y->native_backend) {
                    pa_bluetooth_native_backend_free(y->native_backend);
                    y->native_backend = NULL;
                }
            }

            if (new_owner && *new_owner) {
                pa_log_debug("Bluetooth daemon appeared");
                get_managed_objects(y);
            }
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    } else if (dbus_message_is_signal(m, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded")) {
        DBusMessageIter arg_i;

        if (!y->objects_listed)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED; /* No reply received yet from GetManagedObjects */

        if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "oa{sa{sv}}")) {
            pa_log_error("Invalid signature found in InterfacesAdded");
            goto fail;
        }

        parse_interfaces_and_properties(y, &arg_i);

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    } else if (dbus_message_is_signal(m, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved")) {
        const char *p;
        DBusMessageIter arg_i;
        DBusMessageIter element_i;

        if (!y->objects_listed)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED; /* No reply received yet from GetManagedObjects */

        if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "oas")) {
            pa_log_error("Invalid signature found in InterfacesRemoved");
            goto fail;
        }

        dbus_message_iter_get_basic(&arg_i, &p);

        pa_assert_se(dbus_message_iter_next(&arg_i));
        pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_ARRAY);

        dbus_message_iter_recurse(&arg_i, &element_i);

        while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_STRING) {
            const char *iface;

            dbus_message_iter_get_basic(&element_i, &iface);

            if (pa_streq(iface, BLUEZ_DEVICE_INTERFACE))
                device_remove(y, p);
            else if (pa_streq(iface, BLUEZ_ADAPTER_INTERFACE))
                adapter_remove(y, p);
            else if (pa_streq(iface, BLUEZ_MEDIA_ENDPOINT_INTERFACE))
                remote_endpoint_remove(y, p);

            dbus_message_iter_next(&element_i);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged")) {
        DBusMessageIter arg_i;
        const char *iface;

        if (!y->objects_listed)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED; /* No reply received yet from GetManagedObjects */

        if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "sa{sv}as")) {
            pa_log_error("Invalid signature found in PropertiesChanged");
            goto fail;
        }

        dbus_message_iter_get_basic(&arg_i, &iface);

        pa_assert_se(dbus_message_iter_next(&arg_i));
        pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_ARRAY);

        if (pa_streq(iface, BLUEZ_ADAPTER_INTERFACE)) {
            pa_bluetooth_adapter *a;

            pa_log_debug("Properties changed in adapter %s", dbus_message_get_path(m));

            if (!(a = pa_hashmap_get(y->adapters, dbus_message_get_path(m)))) {
                pa_log_warn("Properties changed in unknown adapter");
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }

            parse_adapter_properties(a, &arg_i, true);

        } else if (pa_streq(iface, BLUEZ_DEVICE_INTERFACE)) {
            pa_bluetooth_device *d;

            pa_log_debug("Properties changed in device %s", dbus_message_get_path(m));

            if (!(d = pa_hashmap_get(y->devices, dbus_message_get_path(m)))) {
                pa_log_warn("Properties changed in unknown device");
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }

            if (!d->properties_received)
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

            parse_device_properties(d, &arg_i);
        } else if (pa_streq(iface, BLUEZ_MEDIA_TRANSPORT_INTERFACE)) {
            pa_bluetooth_transport *t;

            pa_log_debug("Properties changed in transport %s", dbus_message_get_path(m));

            if (!(t = pa_hashmap_get(y->transports, dbus_message_get_path(m))))
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

            parse_transport_properties(t, &arg_i);
        } else if (pa_streq(iface, BLUEZ_MEDIA_ENDPOINT_INTERFACE)) {
            pa_log_info("Properties changed in remote endpoint %s", dbus_message_get_path(m));

            parse_remote_endpoint_properties(y, dbus_message_get_path(m), &arg_i);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

fail:
    dbus_error_free(&err);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

const char *pa_bluetooth_profile_to_string(pa_bluetooth_profile_t profile) {
    switch(profile) {
        case PA_BLUETOOTH_PROFILE_A2DP_SINK:
            return "a2dp_sink";
        case PA_BLUETOOTH_PROFILE_A2DP_SOURCE:
            return "a2dp_source";
        case PA_BLUETOOTH_PROFILE_HSP_HS:
            return "headset_head_unit";
        case PA_BLUETOOTH_PROFILE_HSP_AG:
            return "headset_audio_gateway";
        case PA_BLUETOOTH_PROFILE_HFP_HF:
            return "handsfree_head_unit";
        case PA_BLUETOOTH_PROFILE_HFP_AG:
            return "handsfree_audio_gateway";
        case PA_BLUETOOTH_PROFILE_OFF:
            return "off";
    }

    return NULL;
}

/* Returns true when PA has to perform attenuation, false if this is the
 * responsibility of the peer.
 *
 * `peer_profile` is the profile of the peer.
 *
 * When the peer is in the HFP/HSP Audio Gateway role (PA is in headset role) PA
 * has to perform attenuation on both the incoming and outgoing stream. In the
 * HandsFree/HeadSet role both are attenuated on the peer.
 */
bool pa_bluetooth_profile_should_attenuate_volume(pa_bluetooth_profile_t peer_profile) {
    switch(peer_profile) {
        case PA_BLUETOOTH_PROFILE_A2DP_SINK:
            return false;
        case PA_BLUETOOTH_PROFILE_A2DP_SOURCE:
            return true;
        case PA_BLUETOOTH_PROFILE_HFP_HF:
        case PA_BLUETOOTH_PROFILE_HSP_HS:
            return false;
        case PA_BLUETOOTH_PROFILE_HFP_AG:
        case PA_BLUETOOTH_PROFILE_HSP_AG:
            return true;
        case PA_BLUETOOTH_PROFILE_OFF:
            pa_assert_not_reached();
    }
    pa_assert_not_reached();
}

bool pa_bluetooth_profile_is_a2dp(pa_bluetooth_profile_t profile) {
    return profile == PA_BLUETOOTH_PROFILE_A2DP_SINK || profile == PA_BLUETOOTH_PROFILE_A2DP_SOURCE;
}

static const pa_a2dp_endpoint_conf *a2dp_sep_to_a2dp_endpoint_conf(const char *endpoint) {
    const char *codec_name;

    if (pa_startswith(endpoint, A2DP_SINK_ENDPOINT "/"))
        codec_name = endpoint + strlen(A2DP_SINK_ENDPOINT "/");
    else if (pa_startswith(endpoint, A2DP_SOURCE_ENDPOINT "/"))
        codec_name = endpoint + strlen(A2DP_SOURCE_ENDPOINT "/");
    else
        return NULL;

    return pa_bluetooth_get_a2dp_endpoint_conf(codec_name);
}

static DBusMessage *endpoint_set_configuration(DBusConnection *conn, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y = userdata;
    pa_bluetooth_device *d;
    pa_bluetooth_transport *t;
    const pa_a2dp_endpoint_conf *endpoint_conf = NULL;
    const char *sender, *path, *endpoint_path, *dev_path = NULL, *uuid = NULL;
    const uint8_t *config = NULL;
    int size = 0;
    pa_bluetooth_profile_t p = PA_BLUETOOTH_PROFILE_OFF;
    DBusMessageIter args, props;
    DBusMessage *r;

    if (!dbus_message_iter_init(m, &args) || !pa_streq(dbus_message_get_signature(m), "oa{sv}")) {
        pa_log_error("Invalid signature for method SetConfiguration()");
        goto fail2;
    }

    dbus_message_iter_get_basic(&args, &path);

    if (pa_hashmap_get(y->transports, path)) {
        pa_log_error("Endpoint SetConfiguration(): Transport %s is already configured.", path);
        goto fail2;
    }

    pa_assert_se(dbus_message_iter_next(&args));

    dbus_message_iter_recurse(&args, &props);
    if (dbus_message_iter_get_arg_type(&props) != DBUS_TYPE_DICT_ENTRY)
        goto fail;

    endpoint_path = dbus_message_get_path(m);

    /* Read transport properties */
    while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
        const char *key;
        DBusMessageIter value, entry;
        int var;

        dbus_message_iter_recurse(&props, &entry);
        dbus_message_iter_get_basic(&entry, &key);

        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &value);

        var = dbus_message_iter_get_arg_type(&value);

        if (pa_streq(key, "UUID")) {
            if (var != DBUS_TYPE_STRING) {
                pa_log_error("Property %s of wrong type %c", key, (char)var);
                goto fail;
            }

            dbus_message_iter_get_basic(&value, &uuid);

            if (pa_startswith(endpoint_path, A2DP_SINK_ENDPOINT "/"))
                p = PA_BLUETOOTH_PROFILE_A2DP_SOURCE;
            else if (pa_startswith(endpoint_path, A2DP_SOURCE_ENDPOINT "/"))
                p = PA_BLUETOOTH_PROFILE_A2DP_SINK;

            if ((pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SOURCE) && p != PA_BLUETOOTH_PROFILE_A2DP_SINK) ||
                (pa_streq(uuid, PA_BLUETOOTH_UUID_A2DP_SINK) && p != PA_BLUETOOTH_PROFILE_A2DP_SOURCE)) {
                pa_log_error("UUID %s of transport %s incompatible with endpoint %s", uuid, path, endpoint_path);
                goto fail;
            }
        } else if (pa_streq(key, "Device")) {
            if (var != DBUS_TYPE_OBJECT_PATH) {
                pa_log_error("Property %s of wrong type %c", key, (char)var);
                goto fail;
            }

            dbus_message_iter_get_basic(&value, &dev_path);
        } else if (pa_streq(key, "Configuration")) {
            DBusMessageIter array;

            if (var != DBUS_TYPE_ARRAY) {
                pa_log_error("Property %s of wrong type %c", key, (char)var);
                goto fail;
            }

            dbus_message_iter_recurse(&value, &array);
            var = dbus_message_iter_get_arg_type(&array);
            if (var != DBUS_TYPE_BYTE) {
                pa_log_error("%s is an array of wrong type %c", key, (char)var);
                goto fail;
            }

            dbus_message_iter_get_fixed_array(&array, &config, &size);

            endpoint_conf = a2dp_sep_to_a2dp_endpoint_conf(endpoint_path);
            pa_assert(endpoint_conf);

            if (!endpoint_conf->is_configuration_valid(config, size))
                goto fail;
        }

        dbus_message_iter_next(&props);
    }

    if (!endpoint_conf)
        goto fail2;

    if ((d = pa_hashmap_get(y->devices, dev_path))) {
        if (!d->valid) {
            pa_log_error("Information about device %s is invalid", dev_path);
            goto fail2;
        }
    } else {
        /* InterfacesAdded signal is probably on its way, device_info_valid is kept as 0. */
        pa_log_warn("SetConfiguration() received for unknown device %s", dev_path);
        d = device_create(y, dev_path);
    }

    if (d->transports[p] != NULL) {
        pa_log_error("Cannot configure transport %s because profile %s is already used", path, pa_bluetooth_profile_to_string(p));
        goto fail2;
    }

    sender = dbus_message_get_sender(m);

    pa_assert_se(r = dbus_message_new_method_return(m));
    pa_assert_se(dbus_connection_send(pa_dbus_connection_get(y->connection), r, NULL));
    dbus_message_unref(r);

    t = pa_bluetooth_transport_new(d, sender, path, p, config, size);
    t->acquire = bluez5_transport_acquire_cb;
    t->release = bluez5_transport_release_cb;
    /* A2DP Absolute Volume is optional but BlueZ unconditionally reports
     * feature category 2, meaning supporting it is mandatory.
     * PulseAudio can and should perform the attenuation anyway in
     * the source role as it is the audio rendering device.
     */
    t->set_source_volume = pa_bluetooth_transport_set_source_volume;

    pa_bluetooth_transport_reconfigure(t, &endpoint_conf->bt_codec, a2dp_transport_write, NULL);
    pa_bluetooth_transport_put(t);

    pa_log_debug("Transport %s available for profile %s", t->path, pa_bluetooth_profile_to_string(t->profile));
    pa_log_info("Selected codec: %s", endpoint_conf->bt_codec.name);

    return NULL;

fail:
    pa_log_error("Endpoint SetConfiguration(): invalid arguments");

fail2:
    pa_assert_se(r = dbus_message_new_error(m, BLUEZ_ERROR_INVALID_ARGUMENTS, "Unable to set configuration"));
    return r;
}

static DBusMessage *endpoint_select_configuration(DBusConnection *conn, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y = userdata;
    const char *endpoint_path;
    uint8_t *cap;
    int size;
    const pa_a2dp_endpoint_conf *endpoint_conf;
    uint8_t config[MAX_A2DP_CAPS_SIZE];
    uint8_t *config_ptr = config;
    size_t config_size;
    DBusMessage *r;
    DBusError err;

    endpoint_path = dbus_message_get_path(m);

    dbus_error_init(&err);

    if (!dbus_message_get_args(m, &err, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
        pa_log_error("Endpoint SelectConfiguration(): %s", err.message);
        dbus_error_free(&err);
        goto fail;
    }

    endpoint_conf = a2dp_sep_to_a2dp_endpoint_conf(endpoint_path);
    pa_assert(endpoint_conf);

    config_size = endpoint_conf->fill_preferred_configuration(&y->core->default_sample_spec, cap, size, config);
    if (config_size == 0)
        goto fail;

    pa_assert_se(r = dbus_message_new_method_return(m));
    pa_assert_se(dbus_message_append_args(r, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &config_ptr, config_size, DBUS_TYPE_INVALID));

    return r;

fail:
    pa_assert_se(r = dbus_message_new_error(m, BLUEZ_ERROR_INVALID_ARGUMENTS, "Unable to select configuration"));
    return r;
}

static DBusMessage *endpoint_clear_configuration(DBusConnection *conn, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y = userdata;
    pa_bluetooth_transport *t;
    DBusMessage *r = NULL;
    DBusError err;
    const char *path;

    dbus_error_init(&err);

    if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
        pa_log_error("Endpoint ClearConfiguration(): %s", err.message);
        dbus_error_free(&err);
        goto fail;
    }

    if ((t = pa_hashmap_get(y->transports, path))) {
        pa_log_debug("Clearing transport %s profile %s", t->path, pa_bluetooth_profile_to_string(t->profile));
        pa_bluetooth_transport_free(t);
    }

    if (!dbus_message_get_no_reply(m))
        pa_assert_se(r = dbus_message_new_method_return(m));

    return r;

fail:
    if (!dbus_message_get_no_reply(m))
        pa_assert_se(r = dbus_message_new_error(m, BLUEZ_ERROR_INVALID_ARGUMENTS, "Unable to clear configuration"));
    return r;
}

static DBusMessage *endpoint_release(DBusConnection *conn, DBusMessage *m, void *userdata) {
    DBusMessage *r = NULL;

    /* From doc/media-api.txt in bluez:
     *
     *    This method gets called when the service daemon
     *    unregisters the endpoint. An endpoint can use it to do
     *    cleanup tasks. There is no need to unregister the
     *    endpoint, because when this method gets called it has
     *    already been unregistered.
     *
     * We don't have any cleanup to do. */

    /* Reply only if requested. Generally bluetoothd doesn't request a reply
     * to the Release() call. Sending replies when not requested on the system
     * bus tends to cause errors in syslog from dbus-daemon, because it
     * doesn't let unexpected replies through, so it's important to have this
     * check here. */
    if (!dbus_message_get_no_reply(m))
        pa_assert_se(r = dbus_message_new_method_return(m));

    return r;
}

static DBusHandlerResult endpoint_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    struct pa_bluetooth_discovery *y = userdata;
    DBusMessage *r = NULL;
    const char *path, *interface, *member;

    pa_assert(y);

    path = dbus_message_get_path(m);
    interface = dbus_message_get_interface(m);
    member = dbus_message_get_member(m);

    pa_log_debug("dbus: path=%s, interface=%s, member=%s", path, interface, member);

    if (!a2dp_sep_to_a2dp_endpoint_conf(path))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
        const char *xml = ENDPOINT_INTROSPECT_XML;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID));

    } else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SetConfiguration"))
        r = endpoint_set_configuration(c, m, userdata);
    else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SelectConfiguration"))
        r = endpoint_select_configuration(c, m, userdata);
    else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "ClearConfiguration"))
        r = endpoint_clear_configuration(c, m, userdata);
    else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "Release"))
        r = endpoint_release(c, m, userdata);
    else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (r) {
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(y->connection), r, NULL));
        dbus_message_unref(r);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static void endpoint_init(pa_bluetooth_discovery *y, const char *endpoint) {
    static const DBusObjectPathVTable vtable_endpoint = {
        .message_function = endpoint_handler,
    };

    pa_assert(y);
    pa_assert(endpoint);

    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(y->connection), endpoint,
                                                      &vtable_endpoint, y));
}

static void endpoint_done(pa_bluetooth_discovery *y, const char *endpoint) {
    pa_assert(y);
    pa_assert(endpoint);

    dbus_connection_unregister_object_path(pa_dbus_connection_get(y->connection), endpoint);
}

static void append_a2dp_object(DBusMessageIter *iter, const char *endpoint, const char *uuid, uint8_t codec_id, uint8_t *capabilities, uint8_t capabilities_size) {
    const char *interface_name = BLUEZ_MEDIA_ENDPOINT_INTERFACE;
    DBusMessageIter object, array, entry, dict;

    dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &object);
    pa_assert_se(dbus_message_iter_append_basic(&object, DBUS_TYPE_OBJECT_PATH, &endpoint));

    dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY,
                                     DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING
                                     DBUS_TYPE_ARRAY_AS_STRING
                                     DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING
                                     DBUS_TYPE_VARIANT_AS_STRING
                                     DBUS_DICT_ENTRY_END_CHAR_AS_STRING
                                     DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                     &array);

    dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    pa_assert_se(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface_name));

    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
                                     DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING
                                     DBUS_TYPE_VARIANT_AS_STRING
                                     DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                     &dict);

    pa_dbus_append_basic_variant_dict_entry(&dict, "UUID", DBUS_TYPE_STRING, &uuid);
    pa_dbus_append_basic_variant_dict_entry(&dict, "Codec", DBUS_TYPE_BYTE, &codec_id);
    pa_dbus_append_basic_array_variant_dict_entry(&dict, "Capabilities", DBUS_TYPE_BYTE,
            capabilities, capabilities_size);

    dbus_message_iter_close_container(&entry, &dict);
    dbus_message_iter_close_container(&array, &entry);
    dbus_message_iter_close_container(&object, &array);
    dbus_message_iter_close_container(iter, &object);
}

static DBusHandlerResult object_manager_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    struct pa_bluetooth_discovery *y = userdata;
    DBusMessage *r;
    const char *path, *interface, *member;

    pa_assert(y);

    path = dbus_message_get_path(m);
    interface = dbus_message_get_interface(m);
    member = dbus_message_get_member(m);

    pa_log_debug("dbus: path=%s, interface=%s, member=%s", path, interface, member);

    if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
        const char *xml = OBJECT_MANAGER_INTROSPECT_XML;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID));
    } else if (dbus_message_is_method_call(m, DBUS_INTERFACE_OBJECT_MANAGER, "GetManagedObjects")) {
        DBusMessageIter iter, array;
        int i;

        pa_assert_se(r = dbus_message_new_method_return(m));

        dbus_message_iter_init_append(r, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
                                         DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                         DBUS_TYPE_OBJECT_PATH_AS_STRING
                                         DBUS_TYPE_ARRAY_AS_STRING
                                         DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                         DBUS_TYPE_STRING_AS_STRING
                                         DBUS_TYPE_ARRAY_AS_STRING
                                         DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                         DBUS_TYPE_STRING_AS_STRING
                                         DBUS_TYPE_VARIANT_AS_STRING
                                         DBUS_DICT_ENTRY_END_CHAR_AS_STRING
                                         DBUS_DICT_ENTRY_END_CHAR_AS_STRING
                                         DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                         &array);

        for (i = 0; i < pa_bluetooth_a2dp_endpoint_conf_count(); i++) {
            const pa_a2dp_endpoint_conf *endpoint_conf;
            uint8_t capabilities[MAX_A2DP_CAPS_SIZE];
            uint8_t capabilities_size;
            uint8_t codec_id;
            char *endpoint;

            endpoint_conf = pa_bluetooth_a2dp_endpoint_conf_iter(i);

            codec_id = endpoint_conf->id.codec_id;

            if (endpoint_conf->can_be_supported(false)) {
                capabilities_size = endpoint_conf->fill_capabilities(capabilities);
                pa_assert(capabilities_size != 0);
                endpoint = pa_sprintf_malloc("%s/%s", A2DP_SINK_ENDPOINT, endpoint_conf->bt_codec.name);
                append_a2dp_object(&array, endpoint, PA_BLUETOOTH_UUID_A2DP_SINK, codec_id,
                        capabilities, capabilities_size);
                pa_xfree(endpoint);
            }

            if (endpoint_conf->can_be_supported(true)) {
                capabilities_size = endpoint_conf->fill_capabilities(capabilities);
                pa_assert(capabilities_size != 0);
                endpoint = pa_sprintf_malloc("%s/%s", A2DP_SOURCE_ENDPOINT, endpoint_conf->bt_codec.name);
                append_a2dp_object(&array, endpoint, PA_BLUETOOTH_UUID_A2DP_SOURCE, codec_id,
                        capabilities, capabilities_size);
                pa_xfree(endpoint);
            }
        }

        dbus_message_iter_close_container(&iter, &array);
    } else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    pa_assert_se(dbus_connection_send(pa_dbus_connection_get(y->connection), r, NULL));
    dbus_message_unref(r);

    return DBUS_HANDLER_RESULT_HANDLED;
}

static void object_manager_init(pa_bluetooth_discovery *y) {
    static const DBusObjectPathVTable vtable = {
        .message_function = object_manager_handler,
    };

    pa_assert(y);
    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(y->connection),
                A2DP_OBJECT_MANAGER_PATH, &vtable, y));
}

static void object_manager_done(pa_bluetooth_discovery *y) {
    pa_assert(y);
    dbus_connection_unregister_object_path(pa_dbus_connection_get(y->connection),
            A2DP_OBJECT_MANAGER_PATH);
}

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *c, int headset_backend, bool enable_native_hsp_hs, bool enable_native_hfp_hf, bool enable_msbc) {
    pa_bluetooth_discovery *y;
    DBusError err;
    DBusConnection *conn;
    unsigned i, count;
    const pa_a2dp_endpoint_conf *endpoint_conf;
    char *endpoint;

    pa_bluetooth_a2dp_codec_gst_init();
    y = pa_xnew0(pa_bluetooth_discovery, 1);
    PA_REFCNT_INIT(y);
    y->core = c;
    y->headset_backend = headset_backend;
    y->enable_native_hsp_hs = enable_native_hsp_hs;
    y->enable_native_hfp_hf = enable_native_hfp_hf;
    y->enable_msbc = enable_msbc;
    y->adapters = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL,
                                      (pa_free_cb_t) adapter_free);
    y->devices = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL,
                                     (pa_free_cb_t) device_free);
    y->transports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    PA_LLIST_HEAD_INIT(pa_dbus_pending, y->pending);

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
            "type='signal',sender='" DBUS_SERVICE_DBUS "',interface='" DBUS_INTERFACE_DBUS "',member='NameOwnerChanged'"
            ",arg0='" BLUEZ_SERVICE "'",
            "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_OBJECT_MANAGER "',member='InterfacesAdded'",
            "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_OBJECT_MANAGER "',"
            "member='InterfacesRemoved'",
            "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',member='PropertiesChanged'"
            ",arg0='" BLUEZ_ADAPTER_INTERFACE "'",
            "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',member='PropertiesChanged'"
            ",arg0='" BLUEZ_DEVICE_INTERFACE "'",
            "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',member='PropertiesChanged'"
            ",arg0='" BLUEZ_MEDIA_ENDPOINT_INTERFACE "'",
            "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',member='PropertiesChanged'"
            ",arg0='" BLUEZ_MEDIA_TRANSPORT_INTERFACE "'",
            NULL) < 0) {
        pa_log_error("Failed to add D-Bus matches: %s", err.message);
        goto fail;
    }
    y->matches_added = true;

    object_manager_init(y);

    count = pa_bluetooth_a2dp_endpoint_conf_count();
    for (i = 0; i < count; i++) {
        endpoint_conf = pa_bluetooth_a2dp_endpoint_conf_iter(i);
        if (endpoint_conf->can_be_supported(false)) {
            endpoint = pa_sprintf_malloc("%s/%s", A2DP_SINK_ENDPOINT, endpoint_conf->bt_codec.name);
            endpoint_init(y, endpoint);
            pa_xfree(endpoint);
        }

        if (endpoint_conf->can_be_supported(true)) {
            endpoint = pa_sprintf_malloc("%s/%s", A2DP_SOURCE_ENDPOINT, endpoint_conf->bt_codec.name);
            endpoint_init(y, endpoint);
            pa_xfree(endpoint);
        }
    }

    get_managed_objects(y);

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
    unsigned i, count;
    const pa_a2dp_endpoint_conf *endpoint_conf;
    char *endpoint;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    if (PA_REFCNT_DEC(y) > 0)
        return;

    pa_dbus_free_pending_list(&y->pending);

    if (y->ofono_backend)
        pa_bluetooth_ofono_backend_free(y->ofono_backend);
    if (y->native_backend)
        pa_bluetooth_native_backend_free(y->native_backend);

    if (y->adapters)
        pa_hashmap_free(y->adapters);

    if (y->devices)
        pa_hashmap_free(y->devices);

    if (y->transports) {
        pa_assert(pa_hashmap_isempty(y->transports));
        pa_hashmap_free(y->transports);
    }

    if (y->connection) {

        if (y->matches_added)
            pa_dbus_remove_matches(pa_dbus_connection_get(y->connection),
                "type='signal',sender='" DBUS_SERVICE_DBUS "',interface='" DBUS_INTERFACE_DBUS "',member='NameOwnerChanged',"
                "arg0='" BLUEZ_SERVICE "'",
                "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_OBJECT_MANAGER "',"
                "member='InterfacesAdded'",
                "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_OBJECT_MANAGER "',"
                "member='InterfacesRemoved'",
                "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',"
                "member='PropertiesChanged',arg0='" BLUEZ_ADAPTER_INTERFACE "'",
                "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',"
                "member='PropertiesChanged',arg0='" BLUEZ_DEVICE_INTERFACE "'",
                "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',"
                "member='PropertiesChanged',arg0='" BLUEZ_MEDIA_ENDPOINT_INTERFACE "'",
                "type='signal',sender='" BLUEZ_SERVICE "',interface='" DBUS_INTERFACE_PROPERTIES "',"
                "member='PropertiesChanged',arg0='" BLUEZ_MEDIA_TRANSPORT_INTERFACE "'",
                NULL);

        if (y->filter_added)
            dbus_connection_remove_filter(pa_dbus_connection_get(y->connection), filter_cb, y);

        object_manager_done(y);

        count = pa_bluetooth_a2dp_endpoint_conf_count();
        for (i = 0; i < count; i++) {
            endpoint_conf = pa_bluetooth_a2dp_endpoint_conf_iter(i);

            if (endpoint_conf->can_be_supported(false)) {
                endpoint = pa_sprintf_malloc("%s/%s", A2DP_SINK_ENDPOINT, endpoint_conf->bt_codec.name);
                endpoint_done(y, endpoint);
                pa_xfree(endpoint);
            }

            if (endpoint_conf->can_be_supported(true)) {
                endpoint = pa_sprintf_malloc("%s/%s", A2DP_SOURCE_ENDPOINT, endpoint_conf->bt_codec.name);
                endpoint_done(y, endpoint);
                pa_xfree(endpoint);
            }
        }

        pa_dbus_connection_unref(y->connection);
    }

    pa_shared_remove(y->core, "bluetooth-discovery");
    pa_xfree(y);
}
