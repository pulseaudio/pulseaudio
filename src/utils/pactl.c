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
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
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
#include <limits.h>
#include <getopt.h>
#include <locale.h>

#include <sndfile.h>

#include <pulse/i18n.h>
#include <pulse/pulseaudio.h>

#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/sndfile-util.h>

#define BUFSIZE (16*1024)

static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static char
    *sample_name = NULL,
    *sink_name = NULL,
    *source_name = NULL,
    *module_name = NULL,
    *module_args = NULL,
    *card_name = NULL,
    *profile_name = NULL,
    *port_name = NULL;

static uint32_t
    sink_input_idx = PA_INVALID_INDEX,
    source_output_idx = PA_INVALID_INDEX;

static uint32_t module_index;
static pa_bool_t suspend;
static pa_bool_t mute;
static pa_volume_t volume;

static pa_proplist *proplist = NULL;

static SNDFILE *sndfile = NULL;
static pa_stream *sample_stream = NULL;
static pa_sample_spec sample_spec;
static pa_channel_map channel_map;
static size_t sample_length = 0;
static int actions = 1;

static pa_bool_t nl = FALSE;

static enum {
    NONE,
    EXIT,
    STAT,
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
    SET_SOURCE_PORT,
    SET_SINK_VOLUME,
    SET_SOURCE_VOLUME,
    SET_SINK_INPUT_VOLUME,
    SET_SINK_MUTE,
    SET_SOURCE_MUTE,
    SET_SINK_INPUT_MUTE
} action = NONE;

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

    pa_bytes_snprint(s, sizeof(s), i->memblock_total_size);
    printf(_("Currently in use: %u blocks containing %s bytes total.\n"), i->memblock_total, s);

    pa_bytes_snprint(s, sizeof(s), i->memblock_allocated_size);
    printf(_("Allocated during whole lifetime: %u blocks containing %s bytes total.\n"), i->memblock_allocated, s);

    pa_bytes_snprint(s, sizeof(s), i->scache_size);
    printf(_("Sample cache size: %s\n"), s);

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

    printf(_("User name: %s\n"
             "Host Name: %s\n"
             "Server Name: %s\n"
             "Server Version: %s\n"
             "Default Sample Specification: %s\n"
             "Default Channel Map: %s\n"
             "Default Sink: %s\n"
             "Default Source: %s\n"
             "Cookie: %08x\n"),
           i->user_name,
           i->host_name,
           i->server_name,
           i->server_version,
           ss,
           cm,
           i->default_sink_name,
           i->default_source_name,
           i->cookie);

    complete_action();
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
        cv[PA_CVOLUME_SNPRINT_MAX],
        cvdb[PA_SW_CVOLUME_SNPRINT_DB_MAX],
        v[PA_VOLUME_SNPRINT_MAX],
        vdb[PA_SW_VOLUME_SNPRINT_DB_MAX],
        cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get sink information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;

    printf(_("Sink #%u\n"
             "\tState: %s\n"
             "\tName: %s\n"
             "\tDescription: %s\n"
             "\tDriver: %s\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tOwner Module: %u\n"
             "\tMute: %s\n"
             "\tVolume: %s%s%s\n"
             "\t        balance %0.2f\n"
             "\tBase Volume: %s%s%s\n"
             "\tMonitor Source: %s\n"
             "\tLatency: %0.0f usec, configured %0.0f usec\n"
             "\tFlags: %s%s%s%s%s%s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           state_table[1+i->state],
           i->name,
           pa_strnull(i->description),
           pa_strnull(i->driver),
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           i->owner_module,
           pa_yes_no(i->mute),
           pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           i->flags & PA_SINK_DECIBEL_VOLUME ? "\n\t        " : "",
           i->flags & PA_SINK_DECIBEL_VOLUME ? pa_sw_cvolume_snprint_dB(cvdb, sizeof(cvdb), &i->volume) : "",
           pa_cvolume_get_balance(&i->volume, &i->channel_map),
           pa_volume_snprint(v, sizeof(v), i->base_volume),
           i->flags & PA_SINK_DECIBEL_VOLUME ? "\n\t             " : "",
           i->flags & PA_SINK_DECIBEL_VOLUME ? pa_sw_volume_snprint_dB(vdb, sizeof(vdb), i->base_volume) : "",
           pa_strnull(i->monitor_source_name),
           (double) i->latency, (double) i->configured_latency,
           i->flags & PA_SINK_HARDWARE ? "HARDWARE " : "",
           i->flags & PA_SINK_NETWORK ? "NETWORK " : "",
           i->flags & PA_SINK_HW_MUTE_CTRL ? "HW_MUTE_CTRL " : "",
           i->flags & PA_SINK_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
           i->flags & PA_SINK_DECIBEL_VOLUME ? "DECIBEL_VOLUME " : "",
           i->flags & PA_SINK_LATENCY ? "LATENCY " : "",
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

    pa_xfree(pl);

    if (i->ports) {
        pa_sink_port_info **p;

        printf(_("\tPorts:\n"));
        for (p = i->ports; *p; p++)
            printf("\t\t%s: %s (priority. %u)\n", (*p)->name, (*p)->description, (*p)->priority);
    }

    if (i->active_port)
        printf(_("\tActive Port: %s\n"),
               i->active_port->name);
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
        cv[PA_CVOLUME_SNPRINT_MAX],
        cvdb[PA_SW_CVOLUME_SNPRINT_DB_MAX],
        v[PA_VOLUME_SNPRINT_MAX],
        vdb[PA_SW_VOLUME_SNPRINT_DB_MAX],
        cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get source information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;

    printf(_("Source #%u\n"
             "\tState: %s\n"
             "\tName: %s\n"
             "\tDescription: %s\n"
             "\tDriver: %s\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tOwner Module: %u\n"
             "\tMute: %s\n"
             "\tVolume: %s%s%s\n"
             "\t        balance %0.2f\n"
             "\tBase Volume: %s%s%s\n"
             "\tMonitor of Sink: %s\n"
             "\tLatency: %0.0f usec, configured %0.0f usec\n"
             "\tFlags: %s%s%s%s%s%s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           state_table[1+i->state],
           i->name,
           pa_strnull(i->description),
           pa_strnull(i->driver),
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           i->owner_module,
           pa_yes_no(i->mute),
           pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           i->flags & PA_SOURCE_DECIBEL_VOLUME ? "\n\t        " : "",
           i->flags & PA_SOURCE_DECIBEL_VOLUME ? pa_sw_cvolume_snprint_dB(cvdb, sizeof(cvdb), &i->volume) : "",
           pa_cvolume_get_balance(&i->volume, &i->channel_map),
           pa_volume_snprint(v, sizeof(v), i->base_volume),
           i->flags & PA_SOURCE_DECIBEL_VOLUME ? "\n\t             " : "",
           i->flags & PA_SOURCE_DECIBEL_VOLUME ? pa_sw_volume_snprint_dB(vdb, sizeof(vdb), i->base_volume) : "",
           i->monitor_of_sink_name ? i->monitor_of_sink_name : _("n/a"),
           (double) i->latency, (double) i->configured_latency,
           i->flags & PA_SOURCE_HARDWARE ? "HARDWARE " : "",
           i->flags & PA_SOURCE_NETWORK ? "NETWORK " : "",
           i->flags & PA_SOURCE_HW_MUTE_CTRL ? "HW_MUTE_CTRL " : "",
           i->flags & PA_SOURCE_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
           i->flags & PA_SOURCE_DECIBEL_VOLUME ? "DECIBEL_VOLUME " : "",
           i->flags & PA_SOURCE_LATENCY ? "LATENCY " : "",
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

    pa_xfree(pl);

    if (i->ports) {
        pa_source_port_info **p;

        printf(_("\tPorts:\n"));
        for (p = i->ports; *p; p++)
            printf("\t\t%s: %s (priority. %u)\n", (*p)->name, (*p)->description, (*p)->priority);
    }

    if (i->active_port)
        printf(_("\tActive Port: %s\n"),
               i->active_port->name);
}

static void get_module_info_callback(pa_context *c, const pa_module_info *i, int is_last, void *userdata) {
    char t[32];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get module information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;

    pa_snprintf(t, sizeof(t), "%u", i->n_used);

    printf(_("Module #%u\n"
             "\tName: %s\n"
             "\tArgument: %s\n"
             "\tUsage counter: %s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           i->name,
           i->argument ? i->argument : "",
           i->n_used != PA_INVALID_INDEX ? t : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

    pa_xfree(pl);
}

static void get_client_info_callback(pa_context *c, const pa_client_info *i, int is_last, void *userdata) {
    char t[32];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get client information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;

    pa_snprintf(t, sizeof(t), "%u", i->owner_module);

    printf(_("Client #%u\n"
             "\tDriver: %s\n"
             "\tOwner Module: %s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           pa_strnull(i->driver),
           i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

    pa_xfree(pl);
}

static void get_card_info_callback(pa_context *c, const pa_card_info *i, int is_last, void *userdata) {
    char t[32];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get card information: %s"), pa_strerror(pa_context_errno(c)));
        complete_action();
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;

    pa_snprintf(t, sizeof(t), "%u", i->owner_module);

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

    if (i->profiles) {
        pa_card_profile_info *p;

        printf(_("\tProfiles:\n"));
        for (p = i->profiles; p->name; p++)
            printf("\t\t%s: %s (sinks: %u, sources: %u, priority. %u)\n", p->name, p->description, p->n_sinks, p->n_sources, p->priority);
    }

    if (i->active_profile)
        printf(_("\tActive Profile: %s\n"),
               i->active_profile->name);

    pa_xfree(pl);
}

static void get_sink_input_info_callback(pa_context *c, const pa_sink_input_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cvdb[PA_SW_CVOLUME_SNPRINT_DB_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get sink input information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;

    pa_snprintf(t, sizeof(t), "%u", i->owner_module);
    pa_snprintf(k, sizeof(k), "%u", i->client);

    printf(_("Sink Input #%u\n"
             "\tDriver: %s\n"
             "\tOwner Module: %s\n"
             "\tClient: %s\n"
             "\tSink: %u\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tMute: %s\n"
             "\tVolume: %s\n"
             "\t        %s\n"
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
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           pa_yes_no(i->mute),
           pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           pa_sw_cvolume_snprint_dB(cvdb, sizeof(cvdb), &i->volume),
           pa_cvolume_get_balance(&i->volume, &i->channel_map),
           (double) i->buffer_usec,
           (double) i->sink_usec,
           i->resample_method ? i->resample_method : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

    pa_xfree(pl);
}

static void get_source_output_info_callback(pa_context *c, const pa_source_output_info *i, int is_last, void *userdata) {
    char t[32], k[32], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get source output information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;


    pa_snprintf(t, sizeof(t), "%u", i->owner_module);
    pa_snprintf(k, sizeof(k), "%u", i->client);

    printf(_("Source Output #%u\n"
             "\tDriver: %s\n"
             "\tOwner Module: %s\n"
             "\tClient: %s\n"
             "\tSource: %u\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tBuffer Latency: %0.0f usec\n"
             "\tSource Latency: %0.0f usec\n"
             "\tResample method: %s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           pa_strnull(i->driver),
           i->owner_module != PA_INVALID_INDEX ? t : _("n/a"),
           i->client != PA_INVALID_INDEX ? k : _("n/a"),
           i->source,
           pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec),
           pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
           (double) i->buffer_usec,
           (double) i->source_usec,
           i->resample_method ? i->resample_method : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

    pa_xfree(pl);
}

static void get_sample_info_callback(pa_context *c, const pa_sample_info *i, int is_last, void *userdata) {
    char t[PA_BYTES_SNPRINT_MAX], s[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cvdb[PA_SW_CVOLUME_SNPRINT_DB_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pl;

    if (is_last < 0) {
        pa_log(_("Failed to get sample information: %s"), pa_strerror(pa_context_errno(c)));
        quit(1);
        return;
    }

    if (is_last) {
        complete_action();
        return;
    }

    pa_assert(i);

    if (nl)
        printf("\n");
    nl = TRUE;

    pa_bytes_snprint(t, sizeof(t), i->bytes);

    printf(_("Sample #%u\n"
             "\tName: %s\n"
             "\tSample Specification: %s\n"
             "\tChannel Map: %s\n"
             "\tVolume: %s\n"
             "\t        %s\n"
             "\t        balance %0.2f\n"
             "\tDuration: %0.1fs\n"
             "\tSize: %s\n"
             "\tLazy: %s\n"
             "\tFilename: %s\n"
             "\tProperties:\n\t\t%s\n"),
           i->index,
           i->name,
           pa_sample_spec_valid(&i->sample_spec) ? pa_sample_spec_snprint(s, sizeof(s), &i->sample_spec) : _("n/a"),
           pa_sample_spec_valid(&i->sample_spec) ? pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map) : _("n/a"),
           pa_cvolume_snprint(cv, sizeof(cv), &i->volume),
           pa_sw_cvolume_snprint_dB(cvdb, sizeof(cvdb), &i->volume),
           pa_cvolume_get_balance(&i->volume, &i->channel_map),
           (double) i->duration/1000000.0,
           t,
           pa_yes_no(i->lazy),
           i->filename ? i->filename : _("n/a"),
           pl = pa_proplist_to_string_sep(i->proplist, "\n\t\t"));

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

    printf("%u\n", idx);

    complete_action();
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

    if (sample_length  <= 0) {
        pa_stream_set_write_callback(sample_stream, NULL, NULL);
        pa_stream_finish_upload(sample_stream);
    }
}

static void context_state_callback(pa_context *c, void *userdata) {
    pa_assert(c);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            switch (action) {
                case STAT:
                    actions = 2;
                    pa_operation_unref(pa_context_stat(c, stat_callback, NULL));
                    pa_operation_unref(pa_context_get_server_info(c, get_server_info_callback, NULL));
                    break;

                case PLAY_SAMPLE:
                    pa_operation_unref(pa_context_play_sample(c, sample_name, sink_name, PA_VOLUME_NORM, simple_callback, NULL));
                    break;

                case REMOVE_SAMPLE:
                    pa_operation_unref(pa_context_remove_sample(c, sample_name, simple_callback, NULL));
                    break;

                case UPLOAD_SAMPLE:
                    sample_stream = pa_stream_new(c, sample_name, &sample_spec, NULL);
                    pa_assert(sample_stream);

                    pa_stream_set_state_callback(sample_stream, stream_state_callback, NULL);
                    pa_stream_set_write_callback(sample_stream, stream_write_callback, NULL);
                    pa_stream_connect_upload(sample_stream, sample_length);
                    break;

                case EXIT:
                    pa_operation_unref(pa_context_exit_daemon(c, simple_callback, NULL));
                    break;

                case LIST:
                    actions = 8;
                    pa_operation_unref(pa_context_get_module_info_list(c, get_module_info_callback, NULL));
                    pa_operation_unref(pa_context_get_sink_info_list(c, get_sink_info_callback, NULL));
                    pa_operation_unref(pa_context_get_source_info_list(c, get_source_info_callback, NULL));
                    pa_operation_unref(pa_context_get_sink_input_info_list(c, get_sink_input_info_callback, NULL));
                    pa_operation_unref(pa_context_get_source_output_info_list(c, get_source_output_info_callback, NULL));
                    pa_operation_unref(pa_context_get_client_info_list(c, get_client_info_callback, NULL));
                    pa_operation_unref(pa_context_get_sample_info_list(c, get_sample_info_callback, NULL));
                    pa_operation_unref(pa_context_get_card_info_list(c, get_card_info_callback, NULL));
                    break;

                case MOVE_SINK_INPUT:
                    pa_operation_unref(pa_context_move_sink_input_by_name(c, sink_input_idx, sink_name, simple_callback, NULL));
                    break;

                case MOVE_SOURCE_OUTPUT:
                    pa_operation_unref(pa_context_move_source_output_by_name(c, source_output_idx, source_name, simple_callback, NULL));
                    break;

                case LOAD_MODULE:
                    pa_operation_unref(pa_context_load_module(c, module_name, module_args, index_callback, NULL));
                    break;

                case UNLOAD_MODULE:
                    pa_operation_unref(pa_context_unload_module(c, module_index, simple_callback, NULL));
                    break;

                case SUSPEND_SINK:
                    if (sink_name)
                        pa_operation_unref(pa_context_suspend_sink_by_name(c, sink_name, suspend, simple_callback, NULL));
                    else
                        pa_operation_unref(pa_context_suspend_sink_by_index(c, PA_INVALID_INDEX, suspend, simple_callback, NULL));
                    break;

                case SUSPEND_SOURCE:
                    if (source_name)
                        pa_operation_unref(pa_context_suspend_source_by_name(c, source_name, suspend, simple_callback, NULL));
                    else
                        pa_operation_unref(pa_context_suspend_source_by_index(c, PA_INVALID_INDEX, suspend, simple_callback, NULL));
                    break;

                case SET_CARD_PROFILE:
                    pa_operation_unref(pa_context_set_card_profile_by_name(c, card_name, profile_name, simple_callback, NULL));
                    break;

                case SET_SINK_PORT:
                    pa_operation_unref(pa_context_set_sink_port_by_name(c, sink_name, port_name, simple_callback, NULL));
                    break;

                case SET_SOURCE_PORT:
                    pa_operation_unref(pa_context_set_source_port_by_name(c, source_name, port_name, simple_callback, NULL));
                    break;

                case SET_SINK_MUTE:
                    pa_operation_unref(pa_context_set_sink_mute_by_name(c, sink_name, mute, simple_callback, NULL));
                    break;

                case SET_SOURCE_MUTE:
                    pa_operation_unref(pa_context_set_source_mute_by_name(c, source_name, mute, simple_callback, NULL));
                    break;

                case SET_SINK_INPUT_MUTE:
                    pa_operation_unref(pa_context_set_sink_input_mute(c, sink_input_idx, mute, simple_callback, NULL));
                    break;

                case SET_SINK_VOLUME: {
                    pa_cvolume v;

                    pa_cvolume_set(&v, 1, volume);
                    pa_operation_unref(pa_context_set_sink_volume_by_name(c, sink_name, &v, simple_callback, NULL));
                    break;
                }

                case SET_SOURCE_VOLUME: {
                    pa_cvolume v;

                    pa_cvolume_set(&v, 1, volume);
                    pa_operation_unref(pa_context_set_source_volume_by_name(c, source_name, &v, simple_callback, NULL));
                    break;
                }

                case SET_SINK_INPUT_VOLUME: {
                    pa_cvolume v;

                    pa_cvolume_set(&v, 1, volume);
                    pa_operation_unref(pa_context_set_sink_input_volume(c, sink_input_idx, &v, simple_callback, NULL));
                    break;
                }

                default:
                    pa_assert_not_reached();
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

static void help(const char *argv0) {

    printf(_("%s [options] stat\n"
             "%s [options] list\n"
             "%s [options] exit\n"
             "%s [options] upload-sample FILENAME [NAME]\n"
             "%s [options] play-sample NAME [SINK]\n"
             "%s [options] remove-sample NAME\n"
             "%s [options] move-sink-input SINKINPUT SINK\n"
             "%s [options] move-source-output SOURCEOUTPUT SOURCE\n"
             "%s [options] load-module NAME [ARGS ...]\n"
             "%s [options] unload-module MODULE\n"
             "%s [options] suspend-sink SINK 1|0\n"
             "%s [options] suspend-source SOURCE 1|0\n"
             "%s [options] set-card-profile CARD PROFILE\n"
             "%s [options] set-sink-port SINK PORT\n"
             "%s [options] set-source-port SOURCE PORT\n"
             "%s [options] set-sink-volume SINK VOLUME\n"
             "%s [options] set-source-volume SOURCE VOLUME\n"
             "%s [options] set-sink-input-volume SINKINPUT VOLUME\n"
             "%s [options] set-sink-mute SINK 1|0\n"
             "%s [options] set-source-mute SOURCE 1|0\n"
             "%s [options] set-sink-input-mute SINKINPUT 1|0\n\n"
             "  -h, --help                            Show this help\n"
             "      --version                         Show version\n\n"
             "  -s, --server=SERVER                   The name of the server to connect to\n"
             "  -n, --client-name=NAME                How to call this client on the server\n"),
           argv0, argv0, argv0, argv0, argv0,
           argv0, argv0, argv0, argv0, argv0,
           argv0, argv0, argv0, argv0, argv0,
           argv0, argv0, argv0, argv0, argv0,
           argv0);
}

enum {
    ARG_VERSION = 256
};

int main(int argc, char *argv[]) {
    pa_mainloop* m = NULL;
    int ret = 1, c;
    char *server = NULL, *bn;

    static const struct option long_options[] = {
        {"server",      1, NULL, 's'},
        {"client-name", 1, NULL, 'n'},
        {"version",     0, NULL, ARG_VERSION},
        {"help",        0, NULL, 'h'},
        {NULL,          0, NULL, 0}
    };

    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, PULSE_LOCALEDIR);

    bn = pa_path_get_filename(argv[0]);

    proplist = pa_proplist_new();

    while ((c = getopt_long(argc, argv, "s:n:h", long_options, NULL)) != -1) {
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

    if (optind < argc) {
        if (pa_streq(argv[optind], "stat"))
            action = STAT;
        else if (pa_streq(argv[optind], "exit"))
            action = EXIT;
        else if (pa_streq(argv[optind], "list"))
            action = LIST;
        else if (pa_streq(argv[optind], "upload-sample")) {
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
                pa_log(_("You have to specify a module index"));
                goto quit;
            }

            module_index = (uint32_t) atoi(argv[optind+1]);

        } else if (pa_streq(argv[optind], "suspend-sink")) {
            action = SUSPEND_SINK;

            if (argc > optind+3 || optind+1 >= argc) {
                pa_log(_("You may not specify more than one sink. You have to specify a boolean value."));
                goto quit;
            }

            suspend = pa_parse_boolean(argv[argc-1]);

            if (argc > optind+2)
                sink_name = pa_xstrdup(argv[optind+1]);

        } else if (pa_streq(argv[optind], "suspend-source")) {
            action = SUSPEND_SOURCE;

            if (argc > optind+3 || optind+1 >= argc) {
                pa_log(_("You may not specify more than one source. You have to specify a boolean value."));
                goto quit;
            }

            suspend = pa_parse_boolean(argv[argc-1]);

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

        } else if (pa_streq(argv[optind], "set-source-port")) {
            action = SET_SOURCE_PORT;

            if (argc != optind+3) {
                pa_log(_("You have to specify a source name/index and a port name"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);
            port_name = pa_xstrdup(argv[optind+2]);

        } else if (pa_streq(argv[optind], "set-sink-volume")) {
            uint32_t v;
            action = SET_SINK_VOLUME;

            if (argc != optind+3) {
                pa_log(_("You have to specify a sink name/index and a volume"));
                goto quit;
            }

            if (pa_atou(argv[optind+2], &v) < 0) {
                pa_log(_("Invalid volume specification"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);
            volume = (pa_volume_t) v;

        } else if (pa_streq(argv[optind], "set-source-volume")) {
            uint32_t v;
            action = SET_SOURCE_VOLUME;

            if (argc != optind+3) {
                pa_log(_("You have to specify a source name/index and a volume"));
                goto quit;
            }

            if (pa_atou(argv[optind+2], &v) < 0) {
                pa_log(_("Invalid volume specification"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);
            volume = (pa_volume_t) v;

        } else if (pa_streq(argv[optind], "set-sink-input-volume")) {
            uint32_t v;
            action = SET_SINK_INPUT_VOLUME;

            if (argc != optind+3) {
                pa_log(_("You have to specify a sink input index and a volume"));
                goto quit;
            }

            if (pa_atou(argv[optind+1], &sink_input_idx) < 0) {
                pa_log(_("Invalid sink input index"));
                goto quit;
            }

            if (pa_atou(argv[optind+2], &v) < 0) {
                pa_log(_("Invalid volume specification"));
                goto quit;
            }

            volume = (pa_volume_t) v;

        } else if (pa_streq(argv[optind], "set-sink-mute")) {
            int b;
            action = SET_SINK_MUTE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a sink name/index and a mute boolean"));
                goto quit;
            }

            if ((b = pa_parse_boolean(argv[optind+2])) < 0) {
                pa_log(_("Invalid volume specification"));
                goto quit;
            }

            sink_name = pa_xstrdup(argv[optind+1]);
            mute = b;

        } else if (pa_streq(argv[optind], "set-source-mute")) {
            int b;
            action = SET_SOURCE_MUTE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a source name/index and a mute boolean"));
                goto quit;
            }

            if ((b = pa_parse_boolean(argv[optind+2])) < 0) {
                pa_log(_("Invalid volume specification"));
                goto quit;
            }

            source_name = pa_xstrdup(argv[optind+1]);
            mute = b;

        } else if (pa_streq(argv[optind], "set-sink-input-mute")) {
            int b;
            action = SET_SINK_INPUT_MUTE;

            if (argc != optind+3) {
                pa_log(_("You have to specify a sink input index and a mute boolean"));
                goto quit;
            }

            if (pa_atou(argv[optind+1], &sink_input_idx) < 0) {
                pa_log(_("Invalid sink input index specification"));
                goto quit;
            }

            if ((b = pa_parse_boolean(argv[optind+2])) < 0) {
                pa_log(_("Invalid volume specification"));
                goto quit;
            }

            mute = b;

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
    pa_xfree(sample_name);
    pa_xfree(sink_name);
    pa_xfree(source_name);
    pa_xfree(module_args);
    pa_xfree(card_name);
    pa_xfree(profile_name);

    if (sndfile)
        sf_close(sndfile);

    if (proplist)
        pa_proplist_free(proplist);

    return ret;
}
