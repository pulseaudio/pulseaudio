/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <locale.h>
#include <ctype.h>

#include <sndfile.h>

#include <pulse/pulseaudio.h>
#include <pulse/ext-device-restore.h>
#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/json.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/sndfile-util.h>

static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static char
    *list_type = NULL,
    *sample_name = NULL,
    *sink_name = NULL,
    *source_name = NULL,
    *module_name = NULL,
    *module_args = NULL,
    *card_name = NULL,
    *profile_name = NULL,
    *port_name = NULL,
    *formats = NULL,
    *object_path = NULL,
    *message = NULL,
    *message_args = NULL;

static uint32_t
    sink_input_idx = PA_INVALID_INDEX,
    source_output_idx = PA_INVALID_INDEX,
    sink_idx = PA_INVALID_INDEX;

static bool short_list_format = false;
static uint32_t module_index;
static int32_t latency_offset;
static bool suspend;
static pa_cvolume volume;
static enum volume_flags {
    VOL_UINT     = 0,
    VOL_PERCENT  = 1,
    VOL_LINEAR   = 2,
    VOL_DECIBEL  = 3,
    VOL_ABSOLUTE = 0 << 4,
    VOL_RELATIVE = 1 << 4,
} volume_flags;

static enum mute_flags {
    INVALID_MUTE = -1,
    UNMUTE = 0,
    MUTE = 1,
    TOGGLE_MUTE = 2
} mute = INVALID_MUTE;

static pa_proplist *proplist = NULL;

static SNDFILE *sndfile = NULL;
static pa_stream *sample_stream = NULL;
static pa_sample_spec sample_spec;
static pa_channel_map channel_map;
static size_t sample_length = 0;

/* This variable tracks the number of ongoing asynchronous operations. When a
 * new operation begins, this is incremented simply with actions++, and when
 * an operation finishes, this is decremented with the complete_action()
 * function, which shuts down the program if actions reaches zero. */
static int actions = 0;

static bool nl = false;
static pa_json_encoder *list_encoder = NULL;
static pa_json_encoder *json_encoder = NULL;

static enum {
    NONE,
    EXIT,
    STAT,
    INFO,
    UPLOAD_SAMPLE,
    PLAY_SAMPLE,
    REMOVE_SAMPLE,
    LIST,
    MOVE_SINK_INPUT,
    MOVE_SOURCE_OUTPUT,
    LOAD_MODULE,
    UNLOAD_MODULE,
    SUSPEND_SINK,
    SUSPEND_SOURCE,
    SET_CARD_PROFILE,
    SET_SINK_PORT,
    GET_DEFAULT_SINK,
    SET_DEFAULT_SINK,
    SET_SOURCE_PORT,
    GET_DEFAULT_SOURCE,
    SET_DEFAULT_SOURCE,
    GET_SINK_VOLUME,
    SET_SINK_VOLUME,
    GET_SOURCE_VOLUME,
    SET_SOURCE_VOLUME,
    SET_SINK_INPUT_VOLUME,
    SET_SOURCE_OUTPUT_VOLUME,
    GET_SINK_MUTE,
    SET_SINK_MUTE,
    GET_SOURCE_MUTE,
    SET_SOURCE_MUTE,
    SET_SINK_INPUT_MUTE,
    SET_SOURCE_OUTPUT_MUTE,
    SET_SINK_FORMATS,
    SET_PORT_LATENCY_OFFSET,
    SEND_MESSAGE,
    SUBSCRIBE
} action = NONE;

static enum {
    TEXT,
    JSON
} format = TEXT;

static void quit(int ret) {
    pa_assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}

static void context_drain_complete(pa_context *c, void *userdata) {
    pa_context_disconnect(c);
}

static void drain(void) {
    pa_operation *o;

    if (!(o = pa_context_drain(context, context_drain_complete, NULL)))
        pa_context_disconnect(context);
    else
        pa_operation_unref(o);
}

static void complete_action(void) {
    pa_assert(actions > 0);

    if (!(--actions))
        drain();
}

static void stat_callback(pa_context *c, const pa_stat_info *i, void *userdata) {
    char s[PA_BYTES_SNPRINT_MAX];
    if (!i) {
        pa_log(_("Failed to get statistics: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (format == JSON) {
        printf("{\"current\":{\"blocks\":%u,\"size\":%u},"
               "\"lifetime\":{\"blocks\":%u,\"size\":%u},"
               "\"sample_cache_size\":%u}",
               i->memblock_total,
               i->memblock_total_size,
               i->memblock_allocated,
               i->memblock_allocated_size,
               i->scache_size);
    } else {
        pa_bytes_snprint(s, sizeof(s), i->memblock_total_size);
        printf(ngettext("Currently in use: %u block containing %s bytes total.\n",
                        "Currently in use: %u blocks containing %s bytes total.\n",
                        i->memblock_total),
            i->memblock_total, s);

        pa_bytes_snprint(s, sizeof(s), i->memblock_allocated_size);
        printf(ngettext("Allocated during whole lifetime: %u block containing %s bytes total.\n",
                        "Allocated during whole lifetime: %u blocks containing %s bytes total.\n",
                        i->memblock_allocated),
            i->memblock_allocated, s);

        pa_bytes_snprint(s, sizeof(s), i->scache_size);
        printf(_("Sample cache size: %s\n"), s);
    }

    complete_action();
}

static void get_default_sink(pa_context *c, const pa_server_info *i, void *userdata) {
    if (!i) {
        pa_log(_("Failed to get server information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    printf(_("%s\n"), i->default_sink_name);

    complete_action();
}

static void get_default_source(pa_context *c, const pa_server_info *i, void *userdata) {
    if (!i) {
        pa_log(_("Failed to get server information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    printf(_("%s\n"), i->default_source_name);

    complete_action();
}

static void get_server_info_callback(pa_context *c, const pa_server_info *i, void *useerdata) {
    char ss[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    if (!i) {
        pa_log(_("Failed to get server information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    pa_sample_spec_snprint(ss, sizeof(ss), &i->sample_spec);
    pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map);

    if (format == JSON) {
        char* tile_size = pa_sprintf_malloc("%zu", pa_context_get_tile_size(c, NULL));
        char* cookie = pa_sprintf_malloc("%04x:%04x", i->cookie >> 16, i->cookie & 0xFFFFU);
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_string(encoder, "server_string", pa_context_get_server(c));
        pa_json_encoder_add_member_int(encoder, "library_protocol_version", pa_context_get_protocol_version(c));
        pa_json_encoder_add_member_int(encoder, "server_protocol_version", pa_context_get_server_protocol_version(c));
        pa_json_encoder_add_member_string(encoder, "is_local", pa_yes_no_localised(pa_context_is_local(c)));
        pa_json_encoder_add_member_int(encoder, "client_index", pa_context_get_index(c));
        pa_json_encoder_add_member_string(encoder, "tile_size", tile_size);
        pa_json_encoder_add_member_string(encoder, "user_name", i->user_name);
        pa_json_encoder_add_member_string(encoder, "host_name", i->host_name);
        pa_json_encoder_add_member_string(encoder, "server_name", i->server_name);
        pa_json_encoder_add_member_string(encoder, "server_version", i->server_version);
        pa_json_encoder_add_member_string(encoder, "default_sample_specification", ss);
        pa_json_encoder_add_member_string(encoder, "default_channel_map", cm);
        pa_json_encoder_add_member_string(encoder, "default_sink_name", i->default_sink_name);
        pa_json_encoder_add_member_string(encoder, "default_source_name", i->default_source_name);
        pa_json_encoder_add_member_string(encoder, "cookie", cookie);
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        printf("%s", json_str);
        pa_xfree(json_str);
        pa_xfree(tile_size);
        pa_xfree(cookie);
    } else {
        printf(_("Server String: %s\n"
             "Library Protocol Version: %u\n"
             "Server Protocol Version: %u\n"
             "Is Local: %s\n"
             "Client Index: %u\n"
             "Tile Size: %zu\n"),
             pa_context_get_server(c),
             pa_context_get_protocol_version(c),
             pa_context_get_server_protocol_version(c),
             pa_yes_no_localised(pa_context_is_local(c)),
             pa_context_get_index(c),
             pa_context_get_tile_size(c, NULL));

        printf(_("User Name: %s\n"
                "Host Name: %s\n"
                "Server Name: %s\n"
                "Server Version: %s\n"
                "Default Sample Specification: %s\n"
                "Default Channel Map: %s\n"
                "Default Sink: %s\n"
                "Default Source: %s\n"
                "Cookie: %04x:%04x\n"),
            i->user_name,
            i->host_name,
            i->server_name,
            i->server_version,
            ss,
            cm,
            i->default_sink_name,
            i->default_source_name,
            i->cookie >> 16,
            i->cookie & 0xFFFFU);
    }

    complete_action();
}

static const char* get_available_str(int available) {
    switch (available) {
        case PA_PORT_AVAILABLE_UNKNOWN: return _("availability unknown");
        case PA_PORT_AVAILABLE_YES: return _("available");
        case PA_PORT_AVAILABLE_NO: return _("not available");
    }

    pa_assert_not_reached();
}

static const char* get_device_port_type(unsigned int type) {
    static char buf[32];
    switch (type) {
    case PA_DEVICE_PORT_TYPE_UNKNOWN: return _("Unknown");
    case PA_DEVICE_PORT_TYPE_AUX: return _("Aux");
    case PA_DEVICE_PORT_TYPE_SPEAKER: return _("Speaker");
    case PA_DEVICE_PORT_TYPE_HEADPHONES: return _("Headphones");
    case PA_DEVICE_PORT_TYPE_LINE: return _("Line");
    case PA_DEVICE_PORT_TYPE_MIC: return _("Mic");
    case PA_DEVICE_PORT_TYPE_HEADSET: return _("Headset");
    case PA_DEVICE_PORT_TYPE_HANDSET: return _("Handset");
    case PA_DEVICE_PORT_TYPE_EARPIECE: return _("Earpiece");
    case PA_DEVICE_PORT_TYPE_SPDIF: return _("SPDIF");
    case PA_DEVICE_PORT_TYPE_HDMI: return _("HDMI");
    case PA_DEVICE_PORT_TYPE_TV: return _("TV");
    case PA_DEVICE_PORT_TYPE_RADIO: return _("Radio");
    case PA_DEVICE_PORT_TYPE_VIDEO: return _("Video");
    case PA_DEVICE_PORT_TYPE_USB: return _("USB");
    case PA_DEVICE_PORT_TYPE_BLUETOOTH: return _("Bluetooth");
    case PA_DEVICE_PORT_TYPE_PORTABLE: return _("Portable");
    case PA_DEVICE_PORT_TYPE_HANDSFREE: return _("Handsfree");
    case PA_DEVICE_PORT_TYPE_CAR: return _("Car");
    case PA_DEVICE_PORT_TYPE_HIFI: return _("HiFi");
    case PA_DEVICE_PORT_TYPE_PHONE: return _("Phone");
    case PA_DEVICE_PORT_TYPE_NETWORK: return _("Network");
    case PA_DEVICE_PORT_TYPE_ANALOG: return _("Analog");
    }
    snprintf(buf, sizeof(buf), "%s-%u", _("Unknown"), type);
    return buf;
}

char* pa_proplist_to_json_object(const pa_proplist *p) {
    const char *key;
    void *state = NULL;
    pa_json_encoder *encoder;

    pa_assert(p);

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);
    while (true) {
        key = pa_proplist_iterate(p, &state);
        if (!key) break;

        const char *v;

        if ((v = pa_proplist_gets(p, key))) {
            pa_json_encoder_add_member_string(encoder, key, v);
        } else {
            const void *value;
            size_t nbytes;
            char *c;
            char* hex_str;

            pa_assert_se(pa_proplist_get(p, key, &value, &nbytes) == 0);
            c = pa_xmalloc(nbytes*2+1);
            pa_hexstr((const uint8_t*) value, nbytes, c, nbytes*2+1);

            hex_str = pa_sprintf_malloc("hex:%s", c);
            pa_json_encoder_add_member_string(encoder, key, hex_str);
            pa_xfree(c);
            pa_xfree(hex_str);
        }
    }
    pa_json_encoder_end_object(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

static const char* pa_sink_ports_to_json_array(pa_sink_port_info **ports) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!ports) {
        pa_json_encoder_begin_element_array(encoder);
        pa_json_encoder_end_array(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    pa_sink_port_info **p;

    pa_json_encoder_begin_element_array(encoder);
    for (p = ports; *p; p++) {
        pa_json_encoder *sink_port_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(sink_port_encoder);
        pa_json_encoder_add_member_string(sink_port_encoder, "name", (*p)->name);
        pa_json_encoder_add_member_string(sink_port_encoder, "description", (*p)->description);
        pa_json_encoder_add_member_string(sink_port_encoder, "type", get_device_port_type((*p)->type));
        pa_json_encoder_add_member_int(sink_port_encoder, "priority", (*p)->priority);
        pa_json_encoder_add_member_string(sink_port_encoder, "availability_group", (*p)->availability_group);
        pa_json_encoder_add_member_string(sink_port_encoder, "availability", get_available_str((*p)->available));
        pa_json_encoder_end_object(sink_port_encoder);

        char* sink_port_str = pa_json_encoder_to_string_free(sink_port_encoder);
        pa_json_encoder_add_element_raw_json(encoder, sink_port_str);
        pa_xfree(sink_port_str);
    }
    pa_json_encoder_end_array(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

static const char* pa_source_ports_to_json_array(pa_source_port_info **ports) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!ports) {
        pa_json_encoder_begin_element_array(encoder);
        pa_json_encoder_end_array(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    pa_source_port_info **p;

    pa_json_encoder_begin_element_array(encoder);
    for (p = ports; *p; p++) {
        pa_json_encoder *source_port_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(source_port_encoder);
        pa_json_encoder_add_member_string(source_port_encoder, "name", (*p)->name);
        pa_json_encoder_add_member_string(source_port_encoder, "description", (*p)->description);
        pa_json_encoder_add_member_string(source_port_encoder, "type", get_device_port_type((*p)->type));
        pa_json_encoder_add_member_int(source_port_encoder, "priority", (*p)->priority);
        pa_json_encoder_add_member_string(source_port_encoder, "availability_group", (*p)->availability_group);
        pa_json_encoder_add_member_string(source_port_encoder, "availability", get_available_str((*p)->available));
        pa_json_encoder_end_object(source_port_encoder);

        char* source_port_str = pa_json_encoder_to_string_free(source_port_encoder);
        pa_json_encoder_add_element_raw_json(encoder, source_port_str);
        pa_xfree(source_port_str);
    }
    pa_json_encoder_end_array(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

static const char* pa_format_infos_to_json_array(pa_format_info **formats, uint8_t n_formats) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!formats) {
        pa_json_encoder_begin_element_array(encoder);
        pa_json_encoder_end_array(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    char f[PA_FORMAT_INFO_SNPRINT_MAX];
    uint8_t i;

    pa_json_encoder_begin_element_array(encoder);
    for (i = 0; i < n_formats; i++) {
        pa_json_encoder_add_element_string(encoder, pa_format_info_snprint(f, sizeof(f), formats[i]));
    }
    pa_json_encoder_end_array(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

const char* pa_volume_to_json_object(pa_volume_t v, int print_dB) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!PA_VOLUME_IS_VALID(v)) {
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_string(encoder, "error", _("(invalid)"));
        pa_json_encoder_end_object(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    char dB[PA_SW_VOLUME_SNPRINT_DB_MAX];
    char* value_percent = pa_sprintf_malloc("%u%%", (unsigned)(((uint64_t)v * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM));
    pa_json_encoder_begin_element_object(encoder);
    pa_json_encoder_add_member_int(encoder, "value", v);
    pa_json_encoder_add_member_string(encoder, "value_percent", value_percent);
    pa_json_encoder_add_member_string(encoder, "db", print_dB ? pa_sw_volume_snprint_dB(dB, sizeof(dB), v) : NULL);
    pa_json_encoder_end_object(encoder);
    pa_xfree(value_percent);

    return pa_json_encoder_to_string_free(encoder);
}

const char* pa_cvolume_to_json_object(const pa_cvolume *c, const pa_channel_map *map, int print_dB) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!pa_cvolume_valid(c)) {
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_string(encoder, "error", _("(invalid)"));
        pa_json_encoder_end_object(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    pa_assert(!map || (map->channels == c->channels));
    pa_assert(!map || pa_channel_map_valid(map));

    pa_json_encoder_begin_element_object(encoder);
    for (unsigned channel = 0; channel < c->channels; channel++) {
        char channel_position[32];
        if (map)
            pa_snprintf(channel_position, sizeof(channel_position), "%s", pa_channel_position_to_string(map->map[channel]));
        else
            pa_snprintf(channel_position, sizeof(channel_position), "%u", channel);

        pa_json_encoder_add_member_raw_json(encoder,
            channel_position,
            pa_volume_to_json_object(c->values[channel], print_dB));
    }
    pa_json_encoder_end_object(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

static void pa_json_encoder_end_array_handler(const char *name) {
    pa_assert(json_encoder != NULL);

    pa_json_encoder_end_array(json_encoder);
    char* json_str = pa_json_encoder_to_string_free(json_encoder);
    if (list_encoder != NULL) {
        pa_json_encoder_add_member_raw_json(list_encoder, name, json_str);
    } else {
        printf("%s", json_str);
    }
    pa_xfree(json_str);

    json_encoder = NULL;
}

static void get_sink_info_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata) {

    static const char *state_table[] = {
        [1+PA_SINK_INVALID_STATE] = "n/a",
        [1+PA_SINK_RUNNING] = "RUNNING",
        [1+PA_SINK_IDLE] = "IDLE",
        [1+PA_SINK_SUSPENDED] = "SUSPENDED"
    };

    char
        s[PA_SAMPLE_SPEC_SNPRINT_MAX],
        cv[PA_CVOLUME_SNPRINT_VERBOSE_MAX],
        v[PA_VOLUME_SNPRINT_VERBOSE_MAX],
        cm[PA_CHANNEL_MAP_SNPRINT_MAX],
        f[PA_FORMAT_INFO_SNPRINT_MAX];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get sink information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("sinks");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    char *sample_spec = pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec);
    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            pa_json_encoder_add_member_string(encoder, "name", i->name);
            pa_json_encoder_add_member_string(encoder, "driver", i->driver);
            pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
            pa_json_encoder_add_member_string(encoder, "state", state_table[1+i->state]);
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%s\t%s\t%s\t%s\n",
               i->index,
               i->name,
               pa_strnull(i->driver),
               sample_spec,
               state_table[1+i->state]);
        }
        return;
    }

    char *channel_map = pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map);
    float volume_balance = pa_cvolume_get_balance(&i->volume, &i->channel_map);

    if (format == JSON) {
        pa_json_encoder *latency_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(latency_encoder);
        pa_json_encoder_add_member_double(latency_encoder, "actual", (double) i->latency, 2);
        pa_json_encoder_add_member_double(latency_encoder, "configured", (double) i->configured_latency, 2);
        pa_json_encoder_end_object(latency_encoder);
        char* latency_json_str = pa_json_encoder_to_string_free(latency_encoder);

        pa_json_encoder *flags_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(flags_encoder);
        if (i->flags & PA_SINK_HARDWARE) pa_json_encoder_add_element_string(flags_encoder, "HARDWARE");
        if (i->flags & PA_SINK_NETWORK) pa_json_encoder_add_element_string(flags_encoder, "NETWORK");
        if (i->flags & PA_SINK_HW_MUTE_CTRL) pa_json_encoder_add_element_string(flags_encoder, "HW_MUTE_CTRL");
        if (i->flags & PA_SINK_HW_VOLUME_CTRL) pa_json_encoder_add_element_string(flags_encoder, "HW_VOLUME_CTRL");
        if (i->flags & PA_SINK_DECIBEL_VOLUME) pa_json_encoder_add_element_string(flags_encoder, "DECIBEL_VOLUME");
        if (i->flags & PA_SINK_LATENCY) pa_json_encoder_add_element_string(flags_encoder, "LATENCY");
        if (i->flags & PA_SINK_SET_FORMATS) pa_json_encoder_add_element_string(flags_encoder, "SET_FORMATS");
        pa_json_encoder_end_array(flags_encoder);
        char* flags_json_str = pa_json_encoder_to_string_free(flags_encoder);

        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", i->index);
        pa_json_encoder_add_member_string(encoder, "state", state_table[1+i->state]);
        pa_json_encoder_add_member_string(encoder, "name", i->name);
        pa_json_encoder_add_member_string(encoder, "description", i->description);
        pa_json_encoder_add_member_string(encoder, "driver", i->driver);
        pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
        pa_json_encoder_add_member_string(encoder, "channel_map", channel_map);
        pa_json_encoder_add_member_int(encoder, "owner_module", i->owner_module);
        pa_json_encoder_add_member_bool(encoder, "mute", i->mute);
        pa_json_encoder_add_member_raw_json(encoder, "volume", pa_cvolume_to_json_object(&i->volume, &i->channel_map, i->flags & PA_SINK_DECIBEL_VOLUME));
        pa_json_encoder_add_member_double(encoder, "balance", volume_balance, 2);
        pa_json_encoder_add_member_raw_json(encoder, "base_volume", pa_volume_to_json_object(i->base_volume, i->flags & PA_SINK_DECIBEL_VOLUME));
        pa_json_encoder_add_member_string(encoder, "monitor_source", i->monitor_source_name);
        pa_json_encoder_add_member_raw_json(encoder, "latency", latency_json_str);
        pa_json_encoder_add_member_raw_json(encoder, "flags", flags_json_str);
        pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
        pa_json_encoder_add_member_raw_json(encoder, "ports", pa_sink_ports_to_json_array(i->ports));
        i->active_port ? pa_json_encoder_add_member_string(encoder, "active_port", i->active_port->name): pa_json_encoder_add_member_null(encoder, "active_port");
        pa_json_encoder_add_member_raw_json(encoder, "formats", pa_format_infos_to_json_array(i->formats, i->n_formats));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        pa_json_encoder_add_element_raw_json(json_encoder, json_str);
        pa_xfree(json_str);
        pa_xfree(latency_json_str);
        pa_xfree(flags_json_str);
    } else {
        printf(_("Sink #%u\n"
             "\tState: %s\n"
             "\tName: %s\n"
             "\tDescription: %s\n"
             "\tDriver: %s\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tOwner Module: %u\n"
             "\tMute: %s\n"
             "\tVolume: %s\n"
             "\t        balance %0.2f\n"
             "\tBase Volume: %s\n"
             "\tMonitor Source: %s\n"
             "\tLatency: %0.0f usec, configured %0.0f usec\n"
             "\tFlags: %s%s%s%s%s%s%s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           state_table[1+i->state],
           i->name,
           pa_strnull(i->description),
           pa_strnull(i->driver),
           sample_spec,
           channel_map,
           i->owner_module,
           pa_yes_no_localised(i->mute),
           pa_cvolume_snprint_verbose(cv, sizeof(cv), &i->volume, &i->channel_map, i->flags & PA_SINK_DECIBEL_VOLUME),
           volume_balance,
           pa_volume_snprint_verbose(v, sizeof(v), i->base_volume, i->flags & PA_SINK_DECIBEL_VOLUME),
           pa_strnull(i->monitor_source_name),
           (double) i->latency, (double) i->configured_latency,
           i->flags & PA_SINK_HARDWARE ? "HARDWARE " : "",
           i->flags & PA_SINK_NETWORK ? "NETWORK " : "",
           i->flags & PA_SINK_HW_MUTE_CTRL ? "HW_MUTE_CTRL " : "",
           i->flags & PA_SINK_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
           i->flags & PA_SINK_DECIBEL_VOLUME ? "DECIBEL_VOLUME " : "",
           i->flags & PA_SINK_LATENCY ? "LATENCY " : "",
           i->flags & PA_SINK_SET_FORMATS ? "SET_FORMATS " : "",
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

        if (i->ports) {
            pa_sink_port_info **p;

            printf(_("\tPorts:\n"));
            for (p = i->ports; *p; p++)
                printf(_("\t\t%s: %s (type: %s, priority: %u%s%s, %s)\n"),
                        (*p)->name, (*p)->description, get_device_port_type((*p)->type),
                        (*p)->priority, (*p)->availability_group ? _(", availability group: ") : "",
                        (*p)->availability_group ?: "", get_available_str((*p)->available));
        }

        if (i->active_port)
            printf(_("\tActive Port: %s\n"),
                i->active_port->name);

        if (i->formats) {
            uint8_t j;

            printf(_("\tFormats:\n"));
            for (j = 0; j < i->n_formats; j++)
                printf("\t\t%s\n", pa_format_info_snprint(f, sizeof(f), i->formats[j]));
        }
    }

    pa_xfree(pl);
}

static void get_source_info_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata) {

    static const char *state_table[] = {
        [1+PA_SOURCE_INVALID_STATE] = "n/a",
        [1+PA_SOURCE_RUNNING] = "RUNNING",
        [1+PA_SOURCE_IDLE] = "IDLE",
        [1+PA_SOURCE_SUSPENDED] = "SUSPENDED"
    };

    char
        s[PA_SAMPLE_SPEC_SNPRINT_MAX],
        cv[PA_CVOLUME_SNPRINT_VERBOSE_MAX],
        v[PA_VOLUME_SNPRINT_VERBOSE_MAX],
        cm[PA_CHANNEL_MAP_SNPRINT_MAX],
        f[PA_FORMAT_INFO_SNPRINT_MAX];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get source information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("sources");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    char *sample_spec = pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec);
    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            pa_json_encoder_add_member_string(encoder, "name", i->name);
            pa_json_encoder_add_member_string(encoder, "driver", i->driver);
            pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
            pa_json_encoder_add_member_string(encoder, "state", state_table[1+i->state]);
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%s\t%s\t%s\t%s\n",
               i->index,
               i->name,
               pa_strnull(i->driver),
               sample_spec,
               state_table[1+i->state]);
        }
        return;
    }

    char *channel_map = pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map);
    float volume_balance = pa_cvolume_get_balance(&i->volume, &i->channel_map);

    if (format == JSON) {
        pa_json_encoder *latency_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(latency_encoder);
        pa_json_encoder_add_member_double(latency_encoder, "actual", (double) i->latency, 2);
        pa_json_encoder_add_member_double(latency_encoder, "configured", (double) i->configured_latency, 2);
        pa_json_encoder_end_object(latency_encoder);
        char* latency_json_str = pa_json_encoder_to_string_free(latency_encoder);

        pa_json_encoder *flags_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(flags_encoder);
        if (i->flags & PA_SOURCE_HARDWARE) pa_json_encoder_add_element_string(flags_encoder, "HARDWARE");
        if (i->flags & PA_SOURCE_NETWORK) pa_json_encoder_add_element_string(flags_encoder, "NETWORK");
        if (i->flags & PA_SOURCE_HW_MUTE_CTRL) pa_json_encoder_add_element_string(flags_encoder, "HW_MUTE_CTRL");
        if (i->flags & PA_SOURCE_HW_VOLUME_CTRL) pa_json_encoder_add_element_string(flags_encoder, "HW_VOLUME_CTRL");
        if (i->flags & PA_SOURCE_DECIBEL_VOLUME) pa_json_encoder_add_element_string(flags_encoder, "DECIBEL_VOLUME");
        if (i->flags & PA_SOURCE_LATENCY) pa_json_encoder_add_element_string(flags_encoder, "LATENCY");
        pa_json_encoder_end_array(flags_encoder);
        char* flags_json_str = pa_json_encoder_to_string_free(flags_encoder);

        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", i->index);
        pa_json_encoder_add_member_string(encoder, "state", state_table[1+i->state]);
        pa_json_encoder_add_member_string(encoder, "name", i->name);
        pa_json_encoder_add_member_string(encoder, "description", i->description);
        pa_json_encoder_add_member_string(encoder, "driver", i->driver);
        pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
        pa_json_encoder_add_member_string(encoder, "channel_map", channel_map);
        pa_json_encoder_add_member_int(encoder, "owner_module", i->owner_module);
        pa_json_encoder_add_member_bool(encoder, "mute", i->mute);
        pa_json_encoder_add_member_raw_json(encoder, "volume", pa_cvolume_to_json_object(&i->volume, &i->channel_map, i->flags & PA_SINK_DECIBEL_VOLUME));
        pa_json_encoder_add_member_double(encoder, "balance", volume_balance, 2);
        pa_json_encoder_add_member_raw_json(encoder, "base_volume", pa_volume_to_json_object(i->base_volume, i->flags & PA_SINK_DECIBEL_VOLUME));
        pa_json_encoder_add_member_string(encoder, "monitor_source", i->monitor_of_sink_name);
        pa_json_encoder_add_member_raw_json(encoder, "latency", latency_json_str);
        pa_json_encoder_add_member_raw_json(encoder, "flags", flags_json_str);
        pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
        pa_json_encoder_add_member_raw_json(encoder, "ports", pa_source_ports_to_json_array(i->ports));
        i->active_port ? pa_json_encoder_add_member_string(encoder, "active_port", i->active_port->name) : pa_json_encoder_add_member_null(encoder, "active_port");
        pa_json_encoder_add_member_raw_json(encoder, "formats", pa_format_infos_to_json_array(i->formats, i->n_formats));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        pa_json_encoder_add_element_raw_json(json_encoder, json_str);
        pa_xfree(json_str);
        pa_xfree(latency_json_str);
        pa_xfree(flags_json_str);
    } else {
        printf(_("Source #%u\n"
                "\tState: %s\n"
                "\tName: %s\n"
                "\tDescription: %s\n"
                "\tDriver: %s\n"
                "\tSample Specification: %s\n"
                "\tChannel Map: %s\n"
                "\tOwner Module: %u\n"
                "\tMute: %s\n"
                "\tVolume: %s\n"
                "\t        balance %0.2f\n"
                "\tBase Volume: %s\n"
                "\tMonitor of Sink: %s\n"
                "\tLatency: %0.0f usec, configured %0.0f usec\n"
                "\tFlags: %s%s%s%s%s%s\n"
                "\tProperties:\n\t\t%s\n"),
            i->index,
            state_table[1+i->state],
            i->name,
            pa_strnull(i->description),
            pa_strnull(i->driver),
            sample_spec,
            channel_map,
            i->owner_module,
            pa_yes_no_localised(i->mute),
            pa_cvolume_snprint_verbose(cv, sizeof(cv), &i->volume, &i->channel_map, i->flags & PA_SOURCE_DECIBEL_VOLUME),
            volume_balance,
            pa_volume_snprint_verbose(v, sizeof(v), i->base_volume, i->flags & PA_SOURCE_DECIBEL_VOLUME),
            i->monitor_of_sink_name ? i->monitor_of_sink_name : _("n/a"),
            (double) i->latency, (double) i->configured_latency,
            i->flags & PA_SOURCE_HARDWARE ? "HARDWARE " : "",
            i->flags & PA_SOURCE_NETWORK ? "NETWORK " : "",
            i->flags & PA_SOURCE_HW_MUTE_CTRL ? "HW_MUTE_CTRL " : "",
            i->flags & PA_SOURCE_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
            i->flags & PA_SOURCE_DECIBEL_VOLUME ? "DECIBEL_VOLUME " : "",
            i->flags & PA_SOURCE_LATENCY ? "LATENCY " : "",
            pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

        if (i->ports) {
            pa_source_port_info **p;

            printf(_("\tPorts:\n"));
            for (p = i->ports; *p; p++)
                printf(_("\t\t%s: %s (type: %s, priority: %u%s%s, %s)\n"),
                        (*p)->name, (*p)->description, get_device_port_type((*p)->type),
                        (*p)->priority, (*p)->availability_group ? _(", availability group: ") : "",
                        (*p)->availability_group ?: "", get_available_str((*p)->available));
        }

        if (i->active_port)
            printf(_("\tActive Port: %s\n"),
                i->active_port->name);

        if (i->formats) {
            uint8_t j;

            printf(_("\tFormats:\n"));
            for (j = 0; j < i->n_formats; j++)
                printf("\t\t%s\n", pa_format_info_snprint(f, sizeof(f), i->formats[j]));
        }
    }

    pa_xfree(pl);
}

static void get_module_info_callback(pa_context *c, const pa_module_info *i, int is_last, void *userdata) {
    char t[32];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get module information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("modules");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    pa_snprintf(t, sizeof(t), "%u", i->n_used);

    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_string(encoder, "name", i->name);
            pa_json_encoder_add_member_string(encoder, "argument", i->argument);
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%s\t%s\t\n", i->index, i->name, i->argument ? i->argument : "");
        }
        return;
    }

    char *n_used = i->n_used != PA_INVALID_INDEX ? t : _("n/a");
    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_string(encoder, "name", i->name);
        pa_json_encoder_add_member_string(encoder, "argument", i->argument);
        pa_json_encoder_add_member_string(encoder, "usage_counter", n_used);
        pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        pa_json_encoder_add_element_raw_json(json_encoder, json_str);
        pa_xfree(json_str);
    } else {
        printf(_("Module #%u\n"
                "\tName: %s\n"
                "\tArgument: %s\n"
                "\tUsage counter: %s\n"
                "\tProperties:\n\t\t%s\n"),
            i->index,
            i->name,
            i->argument ? i->argument : "",
            n_used,
            pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

    }

    pa_xfree(pl);
}

static void get_client_info_callback(pa_context *c, const pa_client_info *i, int is_last, void *userdata) {
    char t[32];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get client information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("clients");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    pa_snprintf(t, sizeof(t), "%u", i->owner_module);

    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            pa_json_encoder_add_member_string(encoder, "driver", i->driver);
            pa_json_encoder_add_member_string(encoder, PA_PROP_APPLICATION_PROCESS_BINARY, pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY));
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%s\t%s\n",
               i->index,
               pa_strnull(i->driver),
               pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY)));
        }
        return;
    } else {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            i->driver ? pa_json_encoder_add_member_string(encoder, "driver", i->driver) : pa_json_encoder_add_member_null(encoder, "driver");
            i->owner_module != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "owner_module", t) : pa_json_encoder_add_member_null(encoder, "owner_module");
            pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf(_("Client #%u\n"
                "\tDriver: %s\n"
                "\tOwner Module: %s\n"
                "\tProperties:\n\t\t%s\n"),
                i->index,
                pa_strnull(i->driver),
                i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
                pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));
        }
    }

    pa_xfree(pl);
}

const char* pa_card_profile_info_2_to_json_object(pa_card_profile_info2 **profiles2) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!profiles2) {
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_end_object(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    pa_card_profile_info2 **p;

    pa_json_encoder_begin_element_object(encoder);
    for (p = profiles2; *p; p++) {
        pa_json_encoder *info_json_2_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(info_json_2_encoder);
        pa_json_encoder_add_member_string(info_json_2_encoder, "description", (*p)->description);
        pa_json_encoder_add_member_int(info_json_2_encoder, "sinks", (*p)->n_sinks);
        pa_json_encoder_add_member_int(info_json_2_encoder, "sources", (*p)->n_sources);
        pa_json_encoder_add_member_int(info_json_2_encoder, "priority", (*p)->priority);
        pa_json_encoder_add_member_bool(info_json_2_encoder, "available", (*p)->available);
        pa_json_encoder_end_object(info_json_2_encoder);

        char *info_json_2_str = pa_json_encoder_to_string_free(info_json_2_encoder);
        pa_json_encoder_add_member_raw_json(encoder, (*p)->name, info_json_2_str);
        pa_xfree(info_json_2_str);
    }
    pa_json_encoder_end_object(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

const char* pa_card_profile_info_to_json_array(pa_card_profile_info **info) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!info) {
        pa_json_encoder_begin_element_array(encoder);
        pa_json_encoder_end_array(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    pa_card_profile_info **p;

    pa_json_encoder_begin_element_array(encoder);
    for (p = info; *p; p++) {
        pa_json_encoder_add_element_string(encoder, (*p)->name);
    }
    pa_json_encoder_end_array(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

const char* pa_card_port_info_to_json_object(pa_card_port_info **info) {
    pa_json_encoder *encoder = pa_json_encoder_new();
    if (!info) {
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_end_object(encoder);
        return pa_json_encoder_to_string_free(encoder);
    }

    pa_card_port_info **p;
    char *pl;

    pa_json_encoder_begin_element_object(encoder);
    for (p = info; *p; p++) {
        pa_card_profile_info **pr = (*p)->profiles;

        char* latency_offset_str = pa_sprintf_malloc("%"PRId64" usec", (*p)->latency_offset);
        pa_json_encoder *port_info_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(port_info_encoder);
        pa_json_encoder_add_member_string(port_info_encoder, "description", (*p)->description);
        pa_json_encoder_add_member_string(port_info_encoder, "type", get_device_port_type((*p)->type));
        pa_json_encoder_add_member_int(port_info_encoder, "priority", (*p)->priority);
        pa_json_encoder_add_member_string(port_info_encoder, "latency_offset", latency_offset_str);
        pa_json_encoder_add_member_string(port_info_encoder, "availability_group", (*p)->availability_group);
        pa_json_encoder_add_member_string(port_info_encoder, "availability", get_available_str((*p)->available));
        pa_json_encoder_add_member_raw_json(port_info_encoder, "properties", pl = pa_proplist_to_json_object((*p)->proplist));
        pa_json_encoder_add_member_raw_json(port_info_encoder, "profiles", pa_card_profile_info_to_json_array(pr));
        pa_json_encoder_end_object(port_info_encoder);

        char *port_info_str = pa_json_encoder_to_string_free(port_info_encoder);
        pa_json_encoder_add_member_raw_json(encoder, (*p)->name, port_info_str);
        pa_xfree(port_info_str);
        pa_xfree(latency_offset_str);
        pa_xfree(pl);
    }
    pa_json_encoder_end_object(encoder);

    return pa_json_encoder_to_string_free(encoder);
}

static void get_card_info_callback(pa_context *c, const pa_card_info *i, int is_last, void *userdata) {
    char t[32];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get card information: %s"), pa_strerror(pa_context_errno(c)));
        complete_action();
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("cards");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    pa_snprintf(t, sizeof(t), "%u", i->owner_module);

    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            pa_json_encoder_add_member_string(encoder, "name", i->name);
            pa_json_encoder_add_member_string(encoder, "driver", i->driver);
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%s\t%s\n", i->index, i->name, pa_strnull(i->driver));
        }
        return;
    }

    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", i->index);
        pa_json_encoder_add_member_string(encoder, "name", i->name);
        pa_json_encoder_add_member_string(encoder, "driver", i->driver);
        i->owner_module != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "owner_module", t) : pa_json_encoder_add_member_null(encoder, "owner_module");
        pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
        pa_json_encoder_add_member_raw_json(encoder, "profiles", i->n_profiles > 0 ? pa_card_profile_info_2_to_json_object(i->profiles2) : "{}");
        i->active_profile ? pa_json_encoder_add_member_string(encoder, "active_profile", i->active_profile->name) : pa_json_encoder_add_member_null(encoder, "active_profile");
        pa_json_encoder_add_member_raw_json(encoder, "ports", pa_card_port_info_to_json_object(i->ports));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        pa_json_encoder_add_element_raw_json(json_encoder, json_str);
        pa_xfree(json_str);
    } else {
        printf(_("Card #%u\n"
             "\tName: %s\n"
             "\tDriver: %s\n"
             "\tOwner Module: %s\n"
             "\tProperties:\n\t\t%s\n"),
            i->index,
            i->name,
            pa_strnull(i->driver),
            i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
            pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

        if (i->n_profiles > 0) {
            pa_card_profile_info2 **p;

            printf(_("\tProfiles:\n"));
            for (p = i->profiles2; *p; p++)
                printf(_("\t\t%s: %s (sinks: %u, sources: %u, priority: %u, available: %s)\n"), (*p)->name,
                    (*p)->description, (*p)->n_sinks, (*p)->n_sources, (*p)->priority, pa_yes_no_localised((*p)->available));
        }

        if (i->active_profile)
            printf(_("\tActive Profile: %s\n"),
                i->active_profile->name);

        if (i->ports) {
            pa_card_port_info **p;

            printf(_("\tPorts:\n"));
            for (p = i->ports; *p; p++) {
                pa_card_profile_info **pr = (*p)->profiles;
                printf(_("\t\t%s: %s (type: %s, priority: %u, latency offset: %" PRId64 " usec%s%s, %s)\n"), (*p)->name,
                    (*p)->description, get_device_port_type((*p)->type), (*p)->priority, (*p)->latency_offset,
                    (*p)->availability_group ? _(", availability group: ") : "", (*p)->availability_group ?: "",
                    get_available_str((*p)->available));

                if (!pa_proplist_isempty((*p)->proplist)) {
                    pa_xfree(pl);
                    printf(_("\t\t\tProperties:\n\t\t\t\t%s\n"), pl = pa_proplist_to_string_sep((*p)->proplist, "\n\t\t\t\t"));
                }

                if (pr) {
                    printf(_("\t\t\tPart of profile(s): %s"), pa_strnull((*pr)->name));
                    pr++;
                    while (*pr) {
                        printf(", %s", pa_strnull((*pr)->name));
                        pr++;
                    }
                    printf("\n");
                }
            }
        }
    }

    pa_xfree(pl);
}

static void get_sink_input_info_callback(pa_context *c, const pa_sink_input_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_VERBOSE_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX], f[PA_FORMAT_INFO_SNPRINT_MAX];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get sink input information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("sink_inputs");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    pa_snprintf(t, sizeof(t), "%u", i->owner_module);
    pa_snprintf(k, sizeof(k), "%u", i->client);

    char *sample_spec = pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec);
    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            pa_json_encoder_add_member_int(encoder, "sink", i->sink);
            i->client != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "client", k) : pa_json_encoder_add_member_null(encoder, "client");
            pa_json_encoder_add_member_string(encoder, "driver", i->driver);
            pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%u\t%s\t%s\t%s\n",
               i->index,
               i->sink,
               i->client != PA_INVALID_INDEX ? k : "-",
               pa_strnull(i->driver),
               sample_spec);
        }
        return;
    }

    char *channel_map = pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map);
    char *format_info = pa_format_info_snprint(f, sizeof(f), i->format);
    float balance = pa_cvolume_get_balance(&i->volume, &i->channel_map);
    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", i->index);
        pa_json_encoder_add_member_string(encoder, "driver", i->driver);
        i->owner_module != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "owner_module", t) : pa_json_encoder_add_member_null(encoder, "owner_module");
        i->client != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "client", k) : pa_json_encoder_add_member_null(encoder, "client");
        pa_json_encoder_add_member_int(encoder, "sink", i->sink);
        pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
        pa_json_encoder_add_member_string(encoder, "channel_map", channel_map);
        pa_json_encoder_add_member_string(encoder, "format", format_info);
        pa_json_encoder_add_member_bool(encoder, "corked", i->corked);
        pa_json_encoder_add_member_bool(encoder, "mute", i->mute);
        pa_json_encoder_add_member_raw_json(encoder, "volume", pa_cvolume_to_json_object(&i->volume, &i->channel_map, true));
        pa_json_encoder_add_member_double(encoder, "balance", balance, 2);
        pa_json_encoder_add_member_double(encoder, "buffer_latency_usec", (double) i->buffer_usec, 2);
        pa_json_encoder_add_member_double(encoder, "sink_latency_usec", (double) i->sink_usec, 2);
        pa_json_encoder_add_member_string(encoder, "resample_method", i->resample_method);
        pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        pa_json_encoder_add_element_raw_json(json_encoder, json_str);
        pa_xfree(json_str);
    } else {
        printf(_("Sink Input #%u\n"
             "\tDriver: %s\n"
             "\tOwner Module: %s\n"
             "\tClient: %s\n"
             "\tSink: %u\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tFormat: %s\n"
             "\tCorked: %s\n"
             "\tMute: %s\n"
             "\tVolume: %s\n"
             "\t        balance %0.2f\n"
             "\tBuffer Latency: %0.0f usec\n"
             "\tSink Latency: %0.0f usec\n"
             "\tResample method: %s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           pa_strnull(i->driver),
           i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
           i->client != PA_INVALID_INDEX ? k : _("n/a"),
           i->sink,
           sample_spec,
           channel_map,
           format_info,
           pa_yes_no_localised(i->corked),
           pa_yes_no_localised(i->mute),
           pa_cvolume_snprint_verbose(cv, sizeof(cv), &i->volume, &i->channel_map, true),
           balance,
           (double) i->buffer_usec,
           (double) i->sink_usec,
           i->resample_method ? i->resample_method : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));
    }

    pa_xfree(pl);
}

static void get_source_output_info_callback(pa_context *c, const pa_source_output_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_VERBOSE_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX], f[PA_FORMAT_INFO_SNPRINT_MAX];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get source output information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("source_outputs");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    pa_snprintf(t, sizeof(t), "%u", i->owner_module);
    pa_snprintf(k, sizeof(k), "%u", i->client);

    char *sample_spec = pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec);
    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            pa_json_encoder_add_member_int(encoder, "source", i->source);
            i->client != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "client", k) : pa_json_encoder_add_member_null(encoder, "client");
            pa_json_encoder_add_member_string(encoder, "driver", i->driver);
            pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%u\t%s\t%s\t%s\n",
               i->index,
               i->source,
               i->client != PA_INVALID_INDEX ? k : "-",
               pa_strnull(i->driver),
               sample_spec);
        }
        return;
    }

    char *channel_map = pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map);
    char *format_info = pa_format_info_snprint(f, sizeof(f), i->format);
    float balance = pa_cvolume_get_balance(&i->volume, &i->channel_map);
    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", i->index);
        pa_json_encoder_add_member_string(encoder, "driver", i->driver);
        i->owner_module != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "owner_module", t) : pa_json_encoder_add_member_null(encoder, "owner_module");
        i->client != PA_INVALID_INDEX ? pa_json_encoder_add_member_string(encoder, "client", k) : pa_json_encoder_add_member_null(encoder, "client");
        pa_json_encoder_add_member_int(encoder, "source", i->source);
        pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
        pa_json_encoder_add_member_string(encoder, "channel_map", channel_map);
        pa_json_encoder_add_member_string(encoder, "format", format_info);
        pa_json_encoder_add_member_bool(encoder, "corked", i->corked);
        pa_json_encoder_add_member_bool(encoder, "mute", i->mute);
        pa_json_encoder_add_member_raw_json(encoder, "volume", pa_cvolume_to_json_object(&i->volume, &i->channel_map, true));
        pa_json_encoder_add_member_double(encoder, "balance", balance, 2);
        pa_json_encoder_add_member_double(encoder, "buffer_latency_usec", (double) i->buffer_usec, 2);
        pa_json_encoder_add_member_double(encoder, "source_latency_usec", (double) i->source_usec, 2);
        pa_json_encoder_add_member_string(encoder, "resample_method", i->resample_method);
        pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        pa_json_encoder_add_element_raw_json(json_encoder, json_str);
        pa_xfree(json_str);
    } else {
        printf(_("Source Output #%u\n"
             "\tDriver: %s\n"
             "\tOwner Module: %s\n"
             "\tClient: %s\n"
             "\tSource: %u\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tFormat: %s\n"
             "\tCorked: %s\n"
             "\tMute: %s\n"
             "\tVolume: %s\n"
             "\t        balance %0.2f\n"
             "\tBuffer Latency: %0.0f usec\n"
             "\tSource Latency: %0.0f usec\n"
             "\tResample method: %s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           pa_strnull(i->driver),
           i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
           i->client != PA_INVALID_INDEX ? k : _("n/a"),
           i->source,
           sample_spec,
           channel_map,
           format_info,
           pa_yes_no_localised(i->corked),
           pa_yes_no_localised(i->mute),
           pa_cvolume_snprint_verbose(cv, sizeof(cv), &i->volume, &i->channel_map, true),
           balance,
           (double) i->buffer_usec,
           (double) i->source_usec,
           i->resample_method ? i->resample_method : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));
    }

    pa_xfree(pl);
}

static void get_sample_info_callback(pa_context *c, const pa_sample_info *i, int is_last, void *userdata) {
    char t[PA_BYTES_SNPRINT_MAX], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_VERBOSE_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (format == JSON && json_encoder == NULL) {
        json_encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_array(json_encoder);
    }

    if (is_last < 0) {
        pa_log(_("Failed to get sample information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (format == JSON) {
            pa_json_encoder_end_array_handler("samples");
        }
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl && !short_list_format && format == TEXT)
        printf("\n");
    nl = true;

    pa_bytes_snprint(t, sizeof(t), i->bytes);

    char *sample_spec = pa_sample_spec_valid(&i->sample_spec) ? pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec) : short_list_format ? "-" : _("n/a");
    double duration = (double) i->duration/1000000.0;
    if (short_list_format) {
        if (format == JSON) {
            pa_json_encoder *encoder = pa_json_encoder_new();
            pa_json_encoder_begin_element_object(encoder);
            pa_json_encoder_add_member_int(encoder, "index", i->index);
            pa_json_encoder_add_member_string(encoder, "name", i->name);
            pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
            pa_json_encoder_add_member_double(encoder, "duration", duration, 3);
            pa_json_encoder_end_object(encoder);

            char* json_str = pa_json_encoder_to_string_free(encoder);
            pa_json_encoder_add_element_raw_json(json_encoder, json_str);
            pa_xfree(json_str);
        } else {
            printf("%u\t%s\t%s\t%0.3f\n",
               i->index,
               i->name,
               sample_spec,
               duration);
        }
        return;
    }

    char *channel_map = pa_sample_spec_valid(&i->sample_spec) ? pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map) : _("n/a");
    float balance = pa_cvolume_get_balance(&i->volume, &i->channel_map);
    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", i->index);
        pa_json_encoder_add_member_string(encoder, "name", i->name);
        pa_json_encoder_add_member_string(encoder, "sample_specification", sample_spec);
        pa_json_encoder_add_member_string(encoder, "channel_map", channel_map);
        pa_json_encoder_add_member_raw_json(encoder, "volume", pa_cvolume_to_json_object(&i->volume, &i->channel_map, true));
        pa_json_encoder_add_member_double(encoder, "balance", balance, 2);
        pa_json_encoder_add_member_double(encoder, "duration", duration, 3);
        pa_json_encoder_add_member_string(encoder, "size", t);
        pa_json_encoder_add_member_bool(encoder, "lazy", i->lazy);
        pa_json_encoder_add_member_string(encoder, "filename", i->filename);
        pa_json_encoder_add_member_raw_json(encoder, "properties", pl = pa_proplist_to_json_object(i->proplist));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        pa_json_encoder_add_element_raw_json(json_encoder, json_str);
        pa_xfree(json_str);
    } else {
        printf(_("Sample #%u\n"
             "\tName: %s\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tVolume: %s\n"
             "\t        balance %0.2f\n"
             "\tDuration: %0.1fs\n"
             "\tSize: %s\n"
             "\tLazy: %s\n"
             "\tFilename: %s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           i->name,
           sample_spec,
           channel_map,
           pa_cvolume_snprint_verbose(cv, sizeof(cv), &i->volume, &i->channel_map, true),
           balance,
           duration,
           t,
           pa_yes_no_localised(i->lazy),
           i->filename ? i->filename : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));
    }

    pa_xfree(pl);
}

static void simple_callback(pa_context *c, int success, void *userdata) {
    if (!success) {
        pa_log(_("Failure: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    complete_action();
}

static void index_callback(pa_context *c, uint32_t idx, void *userdata) {
    if (idx == PA_INVALID_INDEX) {
        pa_log(_("Failure: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", idx);
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        printf("%s", json_str);
        pa_xfree(json_str);
    } else {
        printf("%u\n", idx);
    }

    complete_action();
}

static void send_message_callback(pa_context *c, int success, char *response, void *userdata) {

    if (!success) {
        pa_log(_("Send message failed: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_string(encoder, "response", response);
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        printf("%s", json_str);
        pa_xfree(json_str);
    } else {
        printf("%s\n", response);
    }

    complete_action();
}

static void list_handlers_callback(pa_context *c, int success, char *response, void *userdata) {
    int err;
    pa_json_object *o;
    int i;
    const pa_json_object *v, *path, *description;

    if (!success) {
        pa_log(_("list-handlers message failed: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    // The response is already JSON encoded
    if (format == JSON) {
        printf("%s\n", response);
        fflush(stdout);
        complete_action();
        return;
    }

    o = pa_json_parse(response);

    if (!o) {
        pa_log(_("list-handlers message response could not be parsed correctly"));
        pa_json_object_free(o);
        quit(1);
        return;
    }

    if (pa_json_object_get_type(o) != PA_JSON_TYPE_ARRAY) {
        pa_log(_("list-handlers message response is not a JSON array"));
        pa_json_object_free(o);
        quit(1);
        return;
    }

    err = 0;

    for (i = 0; i < pa_json_object_get_array_length(o); ++i) {
        v = pa_json_object_get_array_member(o, i);
        if (pa_json_object_get_type(v) != PA_JSON_TYPE_OBJECT) {
            pa_log(_("list-handlers message response array element %d is not a JSON object"), i);
            err = -1;
            break;
        }

        path = pa_json_object_get_object_member(v, "name");
        if (!path || pa_json_object_get_type(path) != PA_JSON_TYPE_STRING) {
            err = -1;
            break;
        }
        description = pa_json_object_get_object_member(v, "description");
        if (!description || pa_json_object_get_type(description) != PA_JSON_TYPE_STRING) {
            err = -1;
            break;
        }

        if (short_list_format) {
            printf("%s\n", pa_json_object_get_string(path));
        } else {
            if (nl)
                printf("\n");
            nl = true;

            printf("Message Handler %s\n"
                   "\tDescription: %s\n",
                   pa_json_object_get_string(path),
                   pa_json_object_get_string(description));
        }
    }

    if (err < 0) {
        pa_log(_("list-handlers message response could not be parsed correctly"));
        pa_json_object_free(o);
        quit(1);
        return;
    }

    pa_json_object_free(o);

    complete_action();
}

static void volume_relative_adjust(pa_cvolume *cv) {
    pa_assert(volume_flags & VOL_RELATIVE);

    /* Relative volume change is additive in case of UINT or PERCENT
     * and multiplicative for LINEAR or DECIBEL */
    if ((volume_flags & 0x0F) == VOL_UINT || (volume_flags & 0x0F) == VOL_PERCENT) {
        unsigned i;
        for (i = 0; i < cv->channels; i++) {
            if (cv->values[i] + volume.values[i] < PA_VOLUME_NORM)
                cv->values[i] = PA_VOLUME_MUTED;
            else
                cv->values[i] = cv->values[i] + volume.values[i] - PA_VOLUME_NORM;
        }
    }
    if ((volume_flags & 0x0F) == VOL_LINEAR || (volume_flags & 0x0F) == VOL_DECIBEL)
        pa_sw_cvolume_multiply(cv, cv, &volume);
}

static void unload_module_by_name_callback(pa_context *c, const pa_module_info *i, int is_last, void *userdata) {
    static bool unloaded = false;

    if (is_last < 0) {
        pa_log(_("Failed to get module information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        if (unloaded == false)
            pa_log(_("Failed to unload module: Module %s not loaded"), module_name);
        complete_action();
        return;
    }

    pa_assert(i);

    if (pa_streq(module_name, i->name)) {
        unloaded = true;
        actions++;
        pa_operation_unref(pa_context_unload_module(c, i->index, simple_callback, NULL));
    }
}

static void fill_volume(pa_cvolume *cv, unsigned supported) {
    if (volume.channels == 1) {
        pa_cvolume_set(&volume, supported, volume.values[0]);
    } else if (volume.channels != supported) {
        pa_log(ngettext("Failed to set volume: You tried to set volumes for %d channel, whereas channel(s) supported = %d\n",
                        "Failed to set volume: You tried to set volumes for %d channels, whereas channel(s) supported = %d\n",
                        volume.channels),
               volume.channels, supported);
        quit(1);
        return;
    }

    if (volume_flags & VOL_RELATIVE)
        volume_relative_adjust(cv);
    else
        *cv = volume;
}

static void get_sink_mute_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get sink information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    printf(("Mute: %s\n"),
           pa_yes_no_localised(i->mute));

    complete_action();
}

static void get_sink_volume_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get sink information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    char cv[PA_CVOLUME_SNPRINT_VERBOSE_MAX];
    printf(("Volume: %s\n"
            "        balance %0.2f\n"),
           pa_cvolume_snprint_verbose(cv, sizeof(cv), &i->volume, &i->channel_map, true),
           pa_cvolume_get_balance(&i->volume, &i->channel_map));

    complete_action();
}

static void set_sink_volume_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata) {
    pa_cvolume cv;
    pa_operation *o;

    if (is_last < 0) {
        pa_log(_("Failed to get sink information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    cv = i->volume;
    fill_volume(&cv, i->channel_map.channels);

    o = pa_context_set_sink_volume_by_name(c, sink_name, &cv, simple_callback, NULL);
    if (o)
        pa_operation_unref(o);
    else {
        pa_log(_("Failed to set sink volume: %s"), pa_strerror(pa_context_errno(c)));
        complete_action();
    }
}

static void get_source_mute_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get source information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    printf(("Mute: %s\n"),
           pa_yes_no_localised(i->mute));

    complete_action();
}

static void get_source_volume_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get source information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    char cv[PA_CVOLUME_SNPRINT_VERBOSE_MAX];
    printf(("Volume: %s\n"
            "        balance %0.2f\n"),
           pa_cvolume_snprint_verbose(cv, sizeof(cv), &i->volume, &i->channel_map, true),
           pa_cvolume_get_balance(&i->volume, &i->channel_map));

    complete_action();
}

static void set_source_volume_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata) {
    pa_cvolume cv;
    pa_operation *o;

    if (is_last < 0) {
        pa_log(_("Failed to get source information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    cv = i->volume;
    fill_volume(&cv, i->channel_map.channels);

    o = pa_context_set_source_volume_by_name(c, source_name, &cv, simple_callback, NULL);
    if (o)
        pa_operation_unref(o);
    else {
        pa_log(_("Failed to set source volume: %s"), pa_strerror(pa_context_errno(c)));
        complete_action();
    }
}

static void get_sink_input_volume_callback(pa_context *c, const pa_sink_input_info *i, int is_last, void *userdata) {
    pa_cvolume cv;

    if (is_last < 0) {
        pa_log(_("Failed to get sink input information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    cv = i->volume;
    fill_volume(&cv, i->channel_map.channels);

    pa_operation_unref(pa_context_set_sink_input_volume(c, sink_input_idx, &cv, simple_callback, NULL));
}

static void get_source_output_volume_callback(pa_context *c, const pa_source_output_info *o, int is_last, void *userdata) {
    pa_cvolume cv;

    if (is_last < 0) {
        pa_log(_("Failed to get source output information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(o);

    cv = o->volume;
    fill_volume(&cv, o->channel_map.channels);

    pa_operation_unref(pa_context_set_source_output_volume(c, source_output_idx, &cv, simple_callback, NULL));
}

static void sink_toggle_mute_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get sink information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    pa_operation_unref(pa_context_set_sink_mute_by_name(c, i->name, !i->mute, simple_callback, NULL));
}

static void source_toggle_mute_callback(pa_context *c, const pa_source_info *o, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get source information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(o);

    pa_operation_unref(pa_context_set_source_mute_by_name(c, o->name, !o->mute, simple_callback, NULL));
}

static void sink_input_toggle_mute_callback(pa_context *c, const pa_sink_input_info *i, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get sink input information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    pa_operation_unref(pa_context_set_sink_input_mute(c, i->index, !i->mute, simple_callback, NULL));
}

static void source_output_toggle_mute_callback(pa_context *c, const pa_source_output_info *o, int is_last, void *userdata) {
    if (is_last < 0) {
        pa_log(_("Failed to get source output information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(o);

    pa_operation_unref(pa_context_set_source_output_mute(c, o->index, !o->mute, simple_callback, NULL));
}

/* PA_MAX_FORMATS is defined in internal.h so we just define a sane value here */
#define MAX_FORMATS 256

static void set_sink_formats(pa_context *c, uint32_t sink, const char *str) {
    pa_format_info *f_arr[MAX_FORMATS] = { 0, };
    char *format = NULL;
    const char *state = NULL;
    int i = 0;
    pa_operation *o = NULL;

    while ((format = pa_split(str, ";", &state))) {
        pa_format_info *f = pa_format_info_from_string(pa_strip(format));

        if (!f) {
            pa_log(_("Failed to set format: invalid format string %s"), format);
            goto error;
        }

        f_arr[i++] = f;
        pa_xfree(format);
    }

    o = pa_ext_device_restore_save_formats(c, PA_DEVICE_TYPE_SINK, sink, i, f_arr, simple_callback, NULL);
    if (o) {
        pa_operation_unref(o);
        actions++;
    }

done:
    if (format)
        pa_xfree(format);
    while (f_arr[i] && i--)
        pa_format_info_free(f_arr[i]);

    return;

error:
    while (f_arr[i] && i--)
        pa_format_info_free(f_arr[i]);
    quit(1);
    goto done;
}

static void stream_state_callback(pa_stream *s, void *userdata) {
    pa_assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_READY:
            break;

        case PA_STREAM_TERMINATED:
            drain();
            break;

        case PA_STREAM_FAILED:
        default:
            pa_log(_("Failed to upload sample: %s"), pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(1);
    }
}

static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    sf_count_t l;
    float *d;
    pa_assert(s && length && sndfile);

    d = pa_xmalloc(length);

    pa_assert(sample_length >= length);
    l = (sf_count_t) (length/pa_frame_size(&sample_spec));

    if ((sf_readf_float(sndfile, d, l)) != l) {
        pa_xfree(d);
        pa_log(_("Premature end of file"));
        quit(1);
        return;
    }

    pa_stream_write(s, d, length, pa_xfree, 0, PA_SEEK_RELATIVE);

    sample_length -= length;

    if (sample_length <= 0) {
        pa_stream_set_write_callback(sample_stream, NULL, NULL);
        pa_stream_finish_upload(sample_stream);
    }
}

static const char *subscription_event_type_to_string(pa_subscription_event_type_t t) {

    switch (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) {

    case PA_SUBSCRIPTION_EVENT_NEW:
        return _("new");

    case PA_SUBSCRIPTION_EVENT_CHANGE:
        return _("change");

    case PA_SUBSCRIPTION_EVENT_REMOVE:
        return _("remove");
    }

    return _("unknown");
}

static const char *subscription_event_facility_to_string(pa_subscription_event_type_t t) {

    switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {

    case PA_SUBSCRIPTION_EVENT_SINK:
        return _("sink");

    case PA_SUBSCRIPTION_EVENT_SOURCE:
        return _("source");

    case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
        return _("sink-input");

    case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
        return _("source-output");

    case PA_SUBSCRIPTION_EVENT_MODULE:
        return _("module");

    case PA_SUBSCRIPTION_EVENT_CLIENT:
        return _("client");

    case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE:
        return _("sample-cache");

    case PA_SUBSCRIPTION_EVENT_SERVER:
        return _("server");

    case PA_SUBSCRIPTION_EVENT_CARD:
        return _("card");
    }

    return _("unknown");
}

static void context_subscribe_callback(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    pa_assert(c);

    if (format == JSON) {
        pa_json_encoder *encoder = pa_json_encoder_new();
        pa_json_encoder_begin_element_object(encoder);
        pa_json_encoder_add_member_int(encoder, "index", idx);
        pa_json_encoder_add_member_string(encoder, "event", subscription_event_type_to_string(t));
        pa_json_encoder_add_member_string(encoder, "on", subscription_event_facility_to_string(t));
        pa_json_encoder_end_object(encoder);

        char* json_str = pa_json_encoder_to_string_free(encoder);
        printf("%s\n", json_str);
        pa_xfree(json_str);
    } else {
        printf(_("Event '%s' on %s #%u\n"),
           subscription_event_type_to_string(t),
           subscription_event_facility_to_string(t),
           idx);
    }
    fflush(stdout);
}

static void context_state_callback(pa_context *c, void *userdata) {
    pa_operation *o = NULL;

    pa_assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            switch (action) {
                case STAT:
                    o = pa_context_stat(c, stat_callback, NULL);
                    break;

                case INFO:
                    o = pa_context_get_server_info(c, get_server_info_callback, NULL);
                    break;

                case PLAY_SAMPLE:
                    o = pa_context_play_sample(c, sample_name, sink_name, PA_VOLUME_NORM, simple_callback, NULL);
                    break;

                case REMOVE_SAMPLE:
                    o = pa_context_remove_sample(c, sample_name, simple_callback, NULL);
                    break;

                case UPLOAD_SAMPLE:
                    sample_stream = pa_stream_new(c, sample_name, &sample_spec, NULL);
                    pa_assert(sample_stream);

                    pa_stream_set_state_callback(sample_stream, stream_state_callback, NULL);
                    pa_stream_set_write_callback(sample_stream, stream_write_callback, NULL);
                    pa_stream_connect_upload(sample_stream, sample_length);
                    actions++;
                    break;

                case EXIT:
                    o = pa_context_exit_daemon(c, simple_callback, NULL);
                    break;

                case LIST:
                    if (list_type) {
                        if (pa_streq(list_type, "modules"))
                            o = pa_context_get_module_info_list(c, get_module_info_callback, NULL);
                        else if (pa_streq(list_type, "sinks"))
                            o = pa_context_get_sink_info_list(c, get_sink_info_callback, NULL);
                        else if (pa_streq(list_type, "sources"))
                            o = pa_context_get_source_info_list(c, get_source_info_callback, NULL);
                        else if (pa_streq(list_type, "sink-inputs"))
                            o = pa_context_get_sink_input_info_list(c, get_sink_input_info_callback, NULL);
                        else if (pa_streq(list_type, "source-outputs"))
                            o = pa_context_get_source_output_info_list(c, get_source_output_info_callback, NULL);
                        else if (pa_streq(list_type, "clients"))
                            o = pa_context_get_client_info_list(c, get_client_info_callback, NULL);
                        else if (pa_streq(list_type, "samples"))
                            o = pa_context_get_sample_info_list(c, get_sample_info_callback, NULL);
                        else if (pa_streq(list_type, "cards"))
                            o = pa_context_get_card_info_list(c, get_card_info_callback, NULL);
                        else if (pa_streq(list_type, "message-handlers"))
                            o = pa_context_send_message_to_object(c, "/core", "list-handlers", NULL, list_handlers_callback, NULL);
                        else
                            pa_assert_not_reached();
                    } else {
                        if (format == JSON) {
                            list_encoder = pa_json_encoder_new();
                            pa_json_encoder_begin_element_object(list_encoder);
                        }

                        o = pa_context_get_module_info_list(c, get_module_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }

                        o = pa_context_get_sink_info_list(c, get_sink_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }

                        o = pa_context_get_source_info_list(c, get_source_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }
                        o = pa_context_get_sink_input_info_list(c, get_sink_input_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }

                        o = pa_context_get_source_output_info_list(c, get_source_output_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }

                        o = pa_context_get_client_info_list(c, get_client_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }

                        o = pa_context_get_sample_info_list(c, get_sample_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }

                        o = pa_context_get_card_info_list(c, get_card_info_callback, NULL);
                        if (o) {
                            pa_operation_unref(o);
                            actions++;
                        }

                        o = NULL;
                    }
                    break;

                case MOVE_SINK_INPUT:
                    o = pa_context_move_sink_input_by_name(c, sink_input_idx, sink_name, simple_callback, NULL);
                    break;

                case MOVE_SOURCE_OUTPUT:
                    o = pa_context_move_source_output_by_name(c, source_output_idx, source_name, simple_callback, NULL);
                    break;

                case LOAD_MODULE:
                    o = pa_context_load_module(c, module_name, module_args, index_callback, NULL);
                    break;

                case UNLOAD_MODULE:
                    if (module_name)
                        o = pa_context_get_module_info_list(c, unload_module_by_name_callback, NULL);
                    else
                        o = pa_context_unload_module(c, module_index, simple_callback, NULL);
                    break;

                case SUSPEND_SINK:
                    if (sink_name)
                        o = pa_context_suspend_sink_by_name(c, sink_name, suspend, simple_callback, NULL);
                    else
                        o = pa_context_suspend_sink_by_index(c, PA_INVALID_INDEX, suspend, simple_callback, NULL);
                    break;

                case SUSPEND_SOURCE:
                    if (source_name)
                        o = pa_context_suspend_source_by_name(c, source_name, suspend, simple_callback, NULL);
                    else
                        o = pa_context_suspend_source_by_index(c, PA_INVALID_INDEX, suspend, simple_callback, NULL);
                    break;

                case SET_CARD_PROFILE:
                    o = pa_context_set_card_profile_by_name(c, card_name, profile_name, simple_callback, NULL);
                    break;

                case SET_SINK_PORT:
                    o = pa_context_set_sink_port_by_name(c, sink_name, port_name, simple_callback, NULL);
                    break;

                case GET_DEFAULT_SINK:
                    o = pa_context_get_server_info(c, get_default_sink, NULL);
                    break;

                case SET_DEFAULT_SINK:
                    o = pa_context_set_default_sink(c, sink_name, simple_callback, NULL);
                    break;

                case SET_SOURCE_PORT:
                    o = pa_context_set_source_port_by_name(c, source_name, port_name, simple_callback, NULL);
                    break;

                case GET_DEFAULT_SOURCE:
                    o = pa_context_get_server_info(c, get_default_source, NULL);
                    break;

                case SET_DEFAULT_SOURCE:
                    o = pa_context_set_default_source(c, source_name, simple_callback, NULL);
                    break;

                case GET_SINK_MUTE:
                    o = pa_context_get_sink_info_by_name(c, sink_name, get_sink_mute_callback, NULL);
                    break;

                case SET_SINK_MUTE:
                    if (mute == TOGGLE_MUTE)
                        o = pa_context_get_sink_info_by_name(c, sink_name, sink_toggle_mute_callback, NULL);
                    else
                        o = pa_context_set_sink_mute_by_name(c, sink_name, mute, simple_callback, NULL);
                    break;

                case GET_SOURCE_MUTE:
                    o = pa_context_get_source_info_by_name(c, source_name, get_source_mute_callback, NULL);
                    break;

                case SET_SOURCE_MUTE:
                    if (mute == TOGGLE_MUTE)
                        o = pa_context_get_source_info_by_name(c, source_name, source_toggle_mute_callback, NULL);
                    else
                        o = pa_context_set_source_mute_by_name(c, source_name, mute, simple_callback, NULL);
                    break;

                case SET_SINK_INPUT_MUTE:
                    if (mute == TOGGLE_MUTE)
                        o = pa_context_get_sink_input_info(c, sink_input_idx, sink_input_toggle_mute_callback, NULL);
                    else
                        o = pa_context_set_sink_input_mute(c, sink_input_idx, mute, simple_callback, NULL);
                    break;

                case SET_SOURCE_OUTPUT_MUTE:
                    if (mute == TOGGLE_MUTE)
                        o = pa_context_get_source_output_info(c, source_output_idx, source_output_toggle_mute_callback, NULL);
                    else
                        o = pa_context_set_source_output_mute(c, source_output_idx, mute, simple_callback, NULL);
                    break;

                case GET_SINK_VOLUME:
                    o = pa_context_get_sink_info_by_name(c, sink_name, get_sink_volume_callback, NULL);
                    break;

                case SET_SINK_VOLUME:
                    o = pa_context_get_sink_info_by_name(c, sink_name, set_sink_volume_callback, NULL);
                    break;

                case GET_SOURCE_VOLUME:
                    o = pa_context_get_source_info_by_name(c, source_name, get_source_volume_callback, NULL);
                    break;

                case SET_SOURCE_VOLUME:
                    o = pa_context_get_source_info_by_name(c, source_name, set_source_volume_callback, NULL);
                    break;

                case SET_SINK_INPUT_VOLUME:
                    o = pa_context_get_sink_input_info(c, sink_input_idx, get_sink_input_volume_callback, NULL);
                    break;

                case SET_SOURCE_OUTPUT_VOLUME:
                    o = pa_context_get_source_output_info(c, source_output_idx, get_source_output_volume_callback, NULL);
                    break;

                case SET_SINK_FORMATS:
                    set_sink_formats(c, sink_idx, formats);
                    break;

                case SET_PORT_LATENCY_OFFSET:
                    o = pa_context_set_port_latency_offset(c, card_name, port_name, latency_offset, simple_callback, NULL);
                    break;

                case SEND_MESSAGE:
                    o = pa_context_send_message_to_object(c, object_path, message, message_args, send_message_callback, NULL);
                    break;

                case SUBSCRIBE:
                    pa_context_set_subscribe_callback(c, context_subscribe_callback, NULL);

                    o = pa_context_subscribe(c,
                                             PA_SUBSCRIPTION_MASK_SINK|
                                             PA_SUBSCRIPTION_MASK_SOURCE|
                                             PA_SUBSCRIPTION_MASK_SINK_INPUT|
                                             PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT|
                                             PA_SUBSCRIPTION_MASK_MODULE|
                                             PA_SUBSCRIPTION_MASK_CLIENT|
                                             PA_SUBSCRIPTION_MASK_SAMPLE_CACHE|
                                             PA_SUBSCRIPTION_MASK_SERVER|
                                             PA_SUBSCRIPTION_MASK_CARD,
                                             NULL,
                                             NULL);
                    break;

                default:
                    pa_assert_not_reached();
            }

            if (o) {
                pa_operation_unref(o);
                actions++;
            }

            if (actions == 0) {
                pa_log("Operation failed: %s", pa_strerror(pa_context_errno(c)));
                quit(1);
            }

            break;

        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            pa_log(_("Connection failure: %s"), pa_strerror(pa_context_errno(c)));
            quit(1);
    }
}

static void exit_signal_callback(pa_mainloop_api *m, pa_signal_event *e, int sig, void *userdata) {
    pa_log(_("Got SIGINT, exiting."));
    quit(0);
}

static int parse_volume(const char *vol_spec, pa_volume_t *vol, enum volume_flags *vol_flags) {
    double v;
    char *vs;
    const char *atod_input;

    pa_assert(vol_spec);
    pa_assert(vol);
    pa_assert(vol_flags);

    vs = pa_xstrdup(vol_spec);

    *vol_flags = (pa_startswith(vs, "+") || pa_startswith(vs, "-")) ? VOL_RELATIVE : VOL_ABSOLUTE;
    if (pa_endswith(vs, "%")) {
        *vol_flags |= VOL_PERCENT;
        vs[strlen(vs)-1] = 0;
    }
    else if (pa_endswith(vs, "db") || pa_endswith(vs, "dB")) {
        *vol_flags |= VOL_DECIBEL;
        vs[strlen(vs)-2] = 0;
    }
    else if (strchr(vs, '.'))
        *vol_flags |= VOL_LINEAR;

    atod_input = vs;

    if (atod_input[0] == '+')
        atod_input++; /* pa_atod() doesn't accept leading '+', so skip it. */

    if (pa_atod(atod_input, &v) < 0) {
        pa_log(_("Invalid volume specification"));
        pa_xfree(vs);
        return -1;
    }

    pa_xfree(vs);

    if (*vol_flags & VOL_RELATIVE) {
	switch (*vol_flags & 0x0F) {
	    case VOL_UINT:
		v += (double) PA_VOLUME_NORM;
		break;
	    case VOL_PERCENT:
		v += 100.0;
		break;
	    case VOL_LINEAR:
		v += 1.0;
		break;
	}
    }

    switch (*vol_flags & 0x0F) {
	case VOL_PERCENT:
	    v = v * (double) PA_VOLUME_NORM / 100;
	    break;
	case VOL_LINEAR:
	    v = pa_sw_volume_from_linear(v);
	    break;
	case VOL_DECIBEL:
	    v = pa_sw_volume_from_dB(v);
	    break;
    }

    if (!PA_VOLUME_IS_VALID((pa_volume_t) v)) {
        pa_log(_("Volume outside permissible range.\n"));
        return -1;
    }

    *vol = (pa_volume_t) v;

    return 0;
}

static int parse_volumes(char *args[], unsigned n) {
    unsigned i;

    if (n >= PA_CHANNELS_MAX) {
        pa_log(_("Invalid number of volume specifications.\n"));
        return -1;
    }

    volume.channels = n;
    for (i = 0; i < volume.channels; i++) {
        enum volume_flags flags = 0;

        if (parse_volume(args[i], &volume.values[i], &flags) < 0)
            return -1;

        if (i > 0 && flags != volume_flags) {
            pa_log(_("Inconsistent volume specification.\n"));
            return -1;
        } else
            volume_flags = flags;
    }

    return 0;
}

static enum mute_flags parse_mute(const char *mute_text) {
    int b;

    pa_assert(mute_text);

    if (pa_streq("toggle", mute_text))
        return TOGGLE_MUTE;

    b = pa_parse_boolean(mute_text);
    switch (b) {
        case 0:
            return UNMUTE;
        case 1:
            return MUTE;
        default:
            return INVALID_MUTE;
    }
}

static void help(const char *argv0) {

    printf("%s %s %s\n",    argv0, _("[options]"), "stat");
    printf("%s %s %s\n",    argv0, _("[options]"), "info");
    printf("%s %s %s %s\n", argv0, _("[options]"), "list [short]", _("[TYPE]"));
    printf("%s %s %s\n",    argv0, _("[options]"), "exit");
    printf("%s %s %s %s\n", argv0, _("[options]"), "upload-sample", _("FILENAME [NAME]"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "play-sample ", _("NAME [SINK]"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "remove-sample ", _("NAME"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "load-module ", _("NAME [ARGS ...]"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "unload-module ", _("NAME|#N"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "move-(sink-input|source-output)", _("#N SINK|SOURCE"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "suspend-(sink|source)", _("NAME|#N 1|0"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-card-profile ", _("CARD PROFILE"));
    printf("%s %s %s\n", argv0, _("[options]"), "get-default-(sink|source)");
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-default-(sink|source)", _("NAME"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-(sink|source)-port", _("NAME|#N PORT"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "get-(sink|source)-volume", _("NAME|#N"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "get-(sink|source)-mute", _("NAME|#N"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-(sink|source)-volume", _("NAME|#N VOLUME [VOLUME ...]"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-(sink-input|source-output)-volume", _("#N VOLUME [VOLUME ...]"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-(sink|source)-mute", _("NAME|#N 1|0|toggle"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-(sink-input|source-output)-mute", _("#N 1|0|toggle"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-sink-formats", _("#N FORMATS"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "set-port-latency-offset", _("CARD-NAME|CARD-#N PORT OFFSET"));
    printf("%s %s %s %s\n", argv0, _("[options]"), "send-message", _("RECIPIENT MESSAGE [MESSAGE_PARAMETERS]"));
    printf("%s %s %s\n",    argv0, _("[options]"), "subscribe");
    printf(_("\nThe special names @DEFAULT_SINK@, @DEFAULT_SOURCE@ and @DEFAULT_MONITOR@\n"
             "can be used to specify the default sink, source and monitor.\n"));

    printf(_("\n"
             "  -h, --help                            Show this help\n"
             "      --version                         Show version\n\n"
             "  -f, --format=FORMAT                   The format of the output. Either \"normal\" or \"json\"\n"
             "  -s, --server=SERVER                   The name of the server to connect to\n"
             "  -n, --client-name=NAME                How to call this client on the server\n"));
}

enum {
    ARG_VERSION = 256
};

int main(int argc, char *argv[]) {
    pa_mainloop *m = NULL;
    int ret = 1, c;
    char *server = NULL, *opt_format = NULL, *bn;

    static const struct option long_options[] = {
        {"server",      1, NULL, 's'},
        {"client-name", 1, NULL, 'n'},
        {"format",      1, NULL, 'f'},
        {"version",     0, NULL, ARG_VERSION},
        {"help",        0, NULL, 'h'},
        {NULL,          0, NULL, 0}
    };

    setlocale(LC_ALL, "");
#ifdef ENABLE_NLS
    bindtextdomain(GETTEXT_PACKAGE, PULSE_LOCALEDIR);
#endif

    bn = pa_path_get_filename(argv[0]);

    proplist = pa_proplist_new();

    while ((c = getopt_long(argc, argv, "+s:n:f:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'h' :
                help(bn);
                ret = 0;
                goto quit;

            case ARG_VERSION:
                printf(_("pactl %s\n"
                         "Compiled with libpulse %s\n"
                         "Linked with libpulse %s\n"),
                       PACKAGE_VERSION,
                       pa_get_headers_version(),
                       pa_get_library_version());
                ret = 0;
                goto quit;

            case 's':
                pa_xfree(server);
                server = pa_xstrdup(optarg);
                break;

            case 'f':
                opt_format = pa_xstrdup(optarg);
                break;

            case 'n': {
                char *t;

                if (!(t = pa_locale_to_utf8(optarg)) ||
                    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, t) < 0) {

                    pa_log(_("Invalid client name '%s'"), t ? t : optarg);
                    pa_xfree(t);
                    goto quit;
                }

                pa_xfree(t);
                break;
            }

            default:
                goto quit;
        }
    }

    if (!opt_format || pa_streq(opt_format, "text")) {
        format = TEXT;
    } else if (pa_streq(opt_format, "json")) {
        format = JSON;
        setlocale(LC_NUMERIC, "C");
    } else {
        pa_log(_("Invalid format value '%s'"), opt_format);
        goto quit;
    }

    if (optind < argc) {
        if (pa_streq(argv[optind], "stat")) {
            action = STAT;

        } else if (pa_streq(argv[optind], "info"))
            action = INFO;

        else if (pa_streq(argv[optind], "exit"))
            action = EXIT;

        else if (pa_streq(argv[optind], "list")) {
            action = LIST;

            for (int i = optind+1; i < argc; i++) {
                if (pa_streq(argv[i], "modules") || pa_streq(argv[i], "clients") ||
                    pa_streq(argv[i], "sinks")   || pa_streq(argv[i], "sink-inputs") ||
                    pa_streq(argv[i], "sources") || pa_streq(argv[i], "source-outputs") ||
                    pa_streq(argv[i], "samples") || pa_streq(argv[i], "cards") ||
                    pa_streq(argv[i], "message-handlers")) {
                    list_type = pa_xstrdup(argv[i]);
                } else if (pa_streq(argv[i], "short")) {
                    short_list_format = true;
                } else {
                    pa_log(_("Specify nothing, or one of: %s"), "modules, sinks, sources, sink-inputs, source-outputs, clients, samples, cards, message-handlers");
                    goto quit;
                }
            }

        } else if (pa_streq(argv[optind], "upload-sample")) {
            struct SF_INFO sfi;
            action = UPLOAD_SAMPLE;

            if (optind+1 >= argc) {
                pa_log(_("Please specify a sample file to load"));
                goto quit;
            }

            if (optind+2 < argc)
                sample_name = pa_xstrdup(argv[optind+2]);
            else {
                char *f = pa_path_get_filename(argv[optind+1]);
                sample_name = pa_xstrndup(f, strcspn(f, "."));
            }

            pa_zero(sfi);
            if (!(sndfile = sf_open(argv[optind+1], SFM_READ, &sfi))) {
                pa_log(_("Failed to open sound file."));
                goto quit;
            }

            if (pa_sndfile_read_sample_spec(sndfile, &sample_spec) < 0) {
                pa_log(_("Failed to determine sample specification from file."));
                goto quit;
            }
            sample_spec.format = PA_SAMPLE_FLOAT32;

            if (pa_sndfile_read_channel_map(sndfile, &channel_map) < 0) {
                if (sample_spec.channels > 2)
                    pa_log(_("Warning: Failed to determine sample specification from file."));
                pa_channel_map_init_extend(&channel_map, sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
            }

            pa_assert(pa_channel_map_compatible(&channel_map, &sample_spec));
            sample_length = (size_t) sfi.frames*pa_frame_size(&sample_spec);

        } else if (pa_streq(argv[optind], "play-sample")) {
            action = PLAY_SAMPLE;
            if (argc != optind+2 && argc != optind+3) {
                pa_log(_("You have to specify a sample name to play"));
                goto quit;
            }

            sample_name = pa_xstrdup(argv[optind+1]);

            if (optind+2 < argc)
                sink_name = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "remove-sample")) {
            action = REMOVE_SAMPLE;
            if (argc != optind+2) {
                pa_log(_("You have to specify a sample name to remove"));
                goto quit;
            }

            sample_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "move-sink-input")) {
            action = MOVE_SINK_INPUT;
            if (argc != optind+3) {
                pa_log(_("You have to specify a sink input index and a sink"));
                goto quit;
            }

            sink_input_idx = (uint32_t) atoi(argv[optind+1]);
            sink_name = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "move-source-output")) {
            action = MOVE_SOURCE_OUTPUT;
            if (argc != optind+3) {
                pa_log(_("You have to specify a source output index and a source"));
                goto quit;
            }

            source_output_idx = (uint32_t) atoi(argv[optind+1]);
            source_name = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "load-module")) {
            int i;
            size_t n = 0;
            char *p;

            action = LOAD_MODULE;

            if (argc <= optind+1) {
                pa_log(_("You have to specify a module name and arguments."));
                goto quit;
            }

            module_name = argv[optind+1];

            for (i = optind+2; i < argc; i++)
                n += strlen(argv[i])+1;

            if (n > 0) {
                p = module_args = pa_xmalloc(n);

                for (i = optind+2; i < argc; i++)
                    p += sprintf(p, "%s%s", p == module_args ? "" : " ", argv[i]);
            }

        } else if (pa_streq(argv[optind], "unload-module")) {
            action = UNLOAD_MODULE;

            if (argc != optind+2) {
                pa_log(_("You have to specify a module index or name"));
                goto quit;
            }

            if (pa_atou(argv[optind + 1], &module_index) < 0)
                module_name = argv[optind + 1];

        } else if (pa_streq(argv[optind], "suspend-sink")) {
            int b;

            action = SUSPEND_SINK;

            if (argc > optind+3 || optind+1 >= argc) {
                pa_log(_("You may not specify more than one sink. You have to specify a boolean value."));
                goto quit;
            }

            if ((b = pa_parse_boolean(argv[argc-1])) < 0) {
                pa_log(_("Invalid suspend specification."));
                goto quit;
            }

            suspend = !!b;

            if (argc > optind+2)
                sink_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "suspend-source")) {
            int b;

            action = SUSPEND_SOURCE;

            if (argc > optind+3 || optind+1 >= argc) {
                pa_log(_("You may not specify more than one source. You have to specify a boolean value."));
                goto quit;
            }

            if ((b = pa_parse_boolean(argv[argc-1])) < 0) {
                pa_log(_("Invalid suspend specification."));
                goto quit;
            }

            suspend = !!b;

            if (argc > optind+2)
                source_name = pa_xstrdup(argv[optind+1]);
        } else if (pa_streq(argv[optind], "set-card-profile")) {
            action = SET_CARD_PROFILE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a card name/index and a profile name"));
                goto quit;
            }

            card_name = pa_xstrdup(argv[optind+1]);
            profile_name = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "set-sink-port")) {
            action = SET_SINK_PORT;

            if (argc != optind+3) {
                pa_log(_("You have to specify a sink name/index and a port name"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);
            port_name = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "set-default-sink")) {
            action = SET_DEFAULT_SINK;

            if (argc != optind+2) {
                pa_log(_("You have to specify a sink name"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "get-default-sink")) {
            action = GET_DEFAULT_SINK;

        } else if (pa_streq(argv[optind], "set-source-port")) {
            action = SET_SOURCE_PORT;

            if (argc != optind+3) {
                pa_log(_("You have to specify a source name/index and a port name"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);
            port_name = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "set-default-source")) {
            action = SET_DEFAULT_SOURCE;

            if (argc != optind+2) {
                pa_log(_("You have to specify a source name"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "get-default-source")) {
            action = GET_DEFAULT_SOURCE;

        } else if (pa_streq(argv[optind], "get-sink-volume")) {
            action = GET_SINK_VOLUME;

            if (argc < optind+2) {
                pa_log(_("You have to specify a sink name/index"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "set-sink-volume")) {
            action = SET_SINK_VOLUME;

            if (argc < optind+3) {
                pa_log(_("You have to specify a sink name/index and a volume"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);

            if (parse_volumes(argv+optind+2, argc-(optind+2)) < 0)
                goto quit;

        } else if (pa_streq(argv[optind], "get-source-volume")) {
            action = GET_SOURCE_VOLUME;

            if (argc < optind+2) {
                pa_log(_("You have to specify a source name/index"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "set-source-volume")) {
            action = SET_SOURCE_VOLUME;

            if (argc < optind+3) {
                pa_log(_("You have to specify a source name/index and a volume"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);

            if (parse_volumes(argv+optind+2, argc-(optind+2)) < 0)
                goto quit;

        } else if (pa_streq(argv[optind], "set-sink-input-volume")) {
            action = SET_SINK_INPUT_VOLUME;

            if (argc < optind+3) {
                pa_log(_("You have to specify a sink input index and a volume"));
                goto quit;
            }

            if (pa_atou(argv[optind+1], &sink_input_idx) < 0) {
                pa_log(_("Invalid sink input index"));
                goto quit;
            }

            if (parse_volumes(argv+optind+2, argc-(optind+2)) < 0)
                goto quit;

        } else if (pa_streq(argv[optind], "set-source-output-volume")) {
            action = SET_SOURCE_OUTPUT_VOLUME;

            if (argc < optind+3) {
                pa_log(_("You have to specify a source output index and a volume"));
                goto quit;
            }

            if (pa_atou(argv[optind+1], &source_output_idx) < 0) {
                pa_log(_("Invalid source output index"));
                goto quit;
            }

            if (parse_volumes(argv+optind+2, argc-(optind+2)) < 0)
                goto quit;

        } else if (pa_streq(argv[optind], "get-sink-mute")) {
            action = GET_SINK_MUTE;

            if (argc < optind+2) {
                pa_log(_("You have to specify a sink name/index"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "set-sink-mute")) {
            action = SET_SINK_MUTE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a sink name/index and a mute action (0, 1, or 'toggle')"));
                goto quit;
            }

            if ((mute = parse_mute(argv[optind+2])) == INVALID_MUTE) {
                pa_log(_("Invalid mute specification"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "get-source-mute")) {
            action = GET_SOURCE_MUTE;

            if (argc < optind+2) {
                pa_log(_("You have to specify a source name/index"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "set-source-mute")) {
            action = SET_SOURCE_MUTE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a source name/index and a mute action (0, 1, or 'toggle')"));
                goto quit;
            }

            if ((mute = parse_mute(argv[optind+2])) == INVALID_MUTE) {
                pa_log(_("Invalid mute specification"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "set-sink-input-mute")) {
            action = SET_SINK_INPUT_MUTE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a sink input index and a mute action (0, 1, or 'toggle')"));
                goto quit;
            }

            if (pa_atou(argv[optind+1], &sink_input_idx) < 0) {
                pa_log(_("Invalid sink input index specification"));
                goto quit;
            }

            if ((mute = parse_mute(argv[optind+2])) == INVALID_MUTE) {
                pa_log(_("Invalid mute specification"));
                goto quit;
            }

        } else if (pa_streq(argv[optind], "set-source-output-mute")) {
            action = SET_SOURCE_OUTPUT_MUTE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a source output index and a mute action (0, 1, or 'toggle')"));
                goto quit;
            }

            if (pa_atou(argv[optind+1], &source_output_idx) < 0) {
                pa_log(_("Invalid source output index specification"));
                goto quit;
            }

            if ((mute = parse_mute(argv[optind+2])) == INVALID_MUTE) {
                pa_log(_("Invalid mute specification"));
                goto quit;
            }

        } else if (pa_streq(argv[optind], "send-message")) {
            action = SEND_MESSAGE;

            if (argc < optind+3) {
                pa_log(_("You have to specify at least an object path and a message name"));
                goto quit;
            }

            object_path = pa_xstrdup(argv[optind + 1]);
            message = pa_xstrdup(argv[optind + 2]);
            if (argc >= optind+4)
                message_args = pa_xstrdup(argv[optind + 3]);

            if (argc > optind+4)
                pa_log(_("Excess arguments given, they will be ignored. Note that all message parameters must be given as a single string."));

        } else if (pa_streq(argv[optind], "subscribe"))

            action = SUBSCRIBE;

        else if (pa_streq(argv[optind], "set-sink-formats")) {
            int32_t tmp;

            if (argc != optind+3 || pa_atoi(argv[optind+1], &tmp) < 0) {
                pa_log(_("You have to specify a sink index and a semicolon-separated list of supported formats"));
                goto quit;
            }

            sink_idx = tmp;
            action = SET_SINK_FORMATS;
            formats = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "set-port-latency-offset")) {
            action = SET_PORT_LATENCY_OFFSET;

            if (argc != optind+4) {
                pa_log(_("You have to specify a card name/index, a port name and a latency offset"));
                goto quit;
            }

            card_name = pa_xstrdup(argv[optind+1]);
            port_name = pa_xstrdup(argv[optind+2]);
            if (pa_atoi(argv[optind + 3], &latency_offset) < 0) {
                pa_log(_("Could not parse latency offset"));
                goto quit;
            }

        } else if (pa_streq(argv[optind], "help")) {
            help(bn);
            ret = 0;
            goto quit;
        }
    }

    if (action == NONE) {
        pa_log(_("No valid command specified."));
        goto quit;
    }

    if (!(m = pa_mainloop_new())) {
        pa_log(_("pa_mainloop_new() failed."));
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api(m);

    pa_assert_se(pa_signal_init(mainloop_api) == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
    pa_signal_new(SIGTERM, exit_signal_callback, NULL);
    pa_disable_sigpipe();

    if (!(context = pa_context_new_with_proplist(mainloop_api, NULL, proplist))) {
        pa_log(_("pa_context_new() failed."));
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);
    if (pa_context_connect(context, server, 0, NULL) < 0) {
        pa_log(_("pa_context_connect() failed: %s"), pa_strerror(pa_context_errno(context)));
        goto quit;
    }

    if (pa_mainloop_run(m, &ret) < 0) {
        pa_log(_("pa_mainloop_run() failed."));
        goto quit;
    }

    if (format == JSON && list_encoder && !pa_json_encoder_is_empty(list_encoder)) {
        pa_json_encoder_end_object(list_encoder);
        char* list_json_str = pa_json_encoder_to_string_free(list_encoder);
        printf("%s", list_json_str);
        pa_xfree(list_json_str);
    }

quit:
    if (sample_stream)
        pa_stream_unref(sample_stream);

    if (context)
        pa_context_unref(context);

    if (m) {
        pa_signal_done();
        pa_mainloop_free(m);
    }

    pa_xfree(server);
    pa_xfree(list_type);
    pa_xfree(sample_name);
    pa_xfree(sink_name);
    pa_xfree(source_name);
    pa_xfree(module_args);
    pa_xfree(card_name);
    pa_xfree(profile_name);
    pa_xfree(port_name);
    pa_xfree(formats);
    pa_xfree(object_path);
    pa_xfree(message);
    pa_xfree(message_args);

    if (sndfile)
        sf_close(sndfile);

    if (proplist)
        pa_proplist_free(proplist);

    return ret;
}
