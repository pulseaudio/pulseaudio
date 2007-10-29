/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <string.h>

#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/client.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/autoload.h>
#include <pulsecore/macro.h>

#include "cli-text.h"

char *pa_module_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_module *m;
    uint32_t idx = PA_IDXSET_INVALID;
    pa_assert(c);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u module(s) loaded.\n", pa_idxset_size(c->modules));

    for (m = pa_idxset_first(c->modules, &idx); m; m = pa_idxset_next(c->modules, &idx)) {
        pa_strbuf_printf(s, "    index: %u\n"
            "\tname: <%s>\n"
            "\targument: <%s>\n"
            "\tused: %i\n"
            "\tauto unload: %s\n",
            m->index, m->name, m->argument ? m->argument : "", m->n_used,
            m->auto_unload ? "yes" : "no");
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_client_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_client *client;
    uint32_t idx = PA_IDXSET_INVALID;
    pa_assert(c);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u client(s) logged in.\n", pa_idxset_size(c->clients));

    for (client = pa_idxset_first(c->clients, &idx); client; client = pa_idxset_next(c->clients, &idx)) {
        pa_strbuf_printf(s, "    index: %u\n\tname: <%s>\n\tdriver: <%s>\n", client->index, client->name, client->driver);

        if (client->owner)
            pa_strbuf_printf(s, "\towner module: <%u>\n", client->owner->index);
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_sink_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_sink *sink;
    uint32_t idx = PA_IDXSET_INVALID;
    static const char* const state_table[] = {
        [PA_SINK_RUNNING] = "RUNNING",
        [PA_SINK_SUSPENDED] = "SUSPENDED",
        [PA_SINK_IDLE] = "IDLE",
        [PA_SINK_UNLINKED] = "UNLINKED"
    };
    pa_assert(c);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u sink(s) available.\n", pa_idxset_size(c->sinks));

    for (sink = pa_idxset_first(c->sinks, &idx); sink; sink = pa_idxset_next(c->sinks, &idx)) {
        char ss[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

        pa_strbuf_printf(
            s,
            "  %c index: %u\n"
            "\tname: <%s>\n"
            "\tdriver: <%s>\n"
            "\tflags: %s%s%s%s\n"
            "\tstate: %s\n"
            "\tvolume: <%s>\n"
            "\tmute: <%i>\n"
            "\tlatency: <%0.0f usec>\n"
            "\tmonitor source: <%u>\n"
            "\tsample spec: <%s>\n"
            "\tchannel map: <%s>\n"
            "\tused by: <%u>\n"
            "\tlinked by: <%u>\n",
            c->default_sink_name && !strcmp(sink->name, c->default_sink_name) ? '*' : ' ',
            sink->index,
            sink->name,
            sink->driver,
            sink->flags & PA_SINK_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
            sink->flags & PA_SINK_LATENCY ? "LATENCY " : "",
            sink->flags & PA_SINK_HARDWARE ? "HARDWARE " : "",
            sink->flags & PA_SINK_NETWORK ? "NETWORK " : "",
            state_table[pa_sink_get_state(sink)],
            pa_cvolume_snprint(cv, sizeof(cv), pa_sink_get_volume(sink)),
            !!pa_sink_get_mute(sink),
            (double) pa_sink_get_latency(sink),
            sink->monitor_source ? sink->monitor_source->index : PA_INVALID_INDEX,
            pa_sample_spec_snprint(ss, sizeof(ss), &sink->sample_spec),
            pa_channel_map_snprint(cm, sizeof(cm), &sink->channel_map),
            pa_sink_used_by(sink),
            pa_sink_linked_by(sink));

        if (sink->module)
            pa_strbuf_printf(s, "\tmodule: <%u>\n", sink->module->index);
        if (sink->description)
            pa_strbuf_printf(s, "\tdescription: <%s>\n", sink->description);
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_source_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_source *source;
    uint32_t idx = PA_IDXSET_INVALID;
    static const char* const state_table[] = {
        [PA_SOURCE_RUNNING] = "RUNNING",
        [PA_SOURCE_SUSPENDED] = "SUSPENDED",
        [PA_SOURCE_IDLE] = "IDLE",
        [PA_SOURCE_UNLINKED] = "UNLINKED"
    };
    pa_assert(c);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u source(s) available.\n", pa_idxset_size(c->sources));

    for (source = pa_idxset_first(c->sources, &idx); source; source = pa_idxset_next(c->sources, &idx)) {
        char ss[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX];


        pa_strbuf_printf(
            s,
            "  %c index: %u\n"
            "\tname: <%s>\n"
            "\tdriver: <%s>\n"
            "\tflags: %s%s%s%s\n"
            "\tstate: %s\n"
            "\tvolume: <%s>\n"
            "\tmute: <%u>\n"
            "\tlatency: <%0.0f usec>\n"
            "\tsample spec: <%s>\n"
            "\tchannel map: <%s>\n"
            "\tused by: <%u>\n"
            "\tlinked by: <%u>\n",
            c->default_source_name && !strcmp(source->name, c->default_source_name) ? '*' : ' ',
            source->index,
            source->name,
            source->driver,
            source->flags & PA_SOURCE_HW_VOLUME_CTRL ? "HW_VOLUME_CTRL " : "",
            source->flags & PA_SOURCE_LATENCY ? "LATENCY " : "",
            source->flags & PA_SOURCE_HARDWARE ? "HARDWARE " : "",
            source->flags & PA_SOURCE_NETWORK ? "NETWORK " : "",
            state_table[pa_source_get_state(source)],
            pa_cvolume_snprint(cv, sizeof(cv), pa_source_get_volume(source)),
            !!pa_source_get_mute(source),
            (double) pa_source_get_latency(source),
            pa_sample_spec_snprint(ss, sizeof(ss), &source->sample_spec),
            pa_channel_map_snprint(cm, sizeof(cm), &source->channel_map),
            pa_source_used_by(source),
            pa_source_linked_by(source));

        if (source->monitor_of)
            pa_strbuf_printf(s, "\tmonitor_of: <%u>\n", source->monitor_of->index);
        if (source->module)
            pa_strbuf_printf(s, "\tmodule: <%u>\n", source->module->index);
        if (source->description)
            pa_strbuf_printf(s, "\tdescription: <%s>\n", source->description);
    }

    return pa_strbuf_tostring_free(s);
}


char *pa_source_output_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_source_output *o;
    uint32_t idx = PA_IDXSET_INVALID;
    static const char* const state_table[] = {
        [PA_SOURCE_OUTPUT_RUNNING] = "RUNNING",
        [PA_SOURCE_OUTPUT_CORKED] = "CORKED",
        [PA_SOURCE_OUTPUT_UNLINKED] = "UNLINKED"
    };
    pa_assert(c);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u source outputs(s) available.\n", pa_idxset_size(c->source_outputs));

    for (o = pa_idxset_first(c->source_outputs, &idx); o; o = pa_idxset_next(c->source_outputs, &idx)) {
        char ss[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

        pa_assert(o->source);

        pa_strbuf_printf(
            s,
            "    index: %u\n"
            "\tname: '%s'\n"
            "\tdriver: <%s>\n"
            "\tflags: %s%s\n"
            "\tstate: %s\n"
            "\tsource: <%u> '%s'\n"
            "\tlatency: <%0.0f usec>\n"
            "\tsample spec: <%s>\n"
            "\tchannel map: <%s>\n"
            "\tresample method: %s\n",
            o->index,
            o->name,
            o->driver,
            o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE ? "VARIABLE_RATE " : "",
            o->flags & PA_SOURCE_OUTPUT_DONT_MOVE ? "DONT_MOVE " : "",
            state_table[pa_source_output_get_state(o)],
            o->source->index, o->source->name,
            (double) pa_source_output_get_latency(o),
            pa_sample_spec_snprint(ss, sizeof(ss), &o->sample_spec),
            pa_channel_map_snprint(cm, sizeof(cm), &o->channel_map),
            pa_resample_method_to_string(pa_source_output_get_resample_method(o)));
        if (o->module)
            pa_strbuf_printf(s, "\towner module: <%u>\n", o->module->index);
        if (o->client)
            pa_strbuf_printf(s, "\tclient: <%u> '%s'\n", o->client->index, o->client->name);
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_sink_input_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_sink_input *i;
    uint32_t idx = PA_IDXSET_INVALID;
    static const char* const state_table[] = {
        [PA_SINK_INPUT_RUNNING] = "RUNNING",
        [PA_SINK_INPUT_DRAINED] = "DRAINED",
        [PA_SINK_INPUT_CORKED] = "CORKED",
        [PA_SINK_INPUT_UNLINKED] = "UNLINKED"
    };

    pa_assert(c);
    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u sink input(s) available.\n", pa_idxset_size(c->sink_inputs));

    for (i = pa_idxset_first(c->sink_inputs, &idx); i; i = pa_idxset_next(c->sink_inputs, &idx)) {
        char ss[PA_SAMPLE_SPEC_SNPRINT_MAX], cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

        pa_assert(i->sink);

        pa_strbuf_printf(
            s,
            "    index: %u\n"
            "\tname: <%s>\n"
            "\tdriver: <%s>\n"
            "\tflags: %s%s\n"
            "\tstate: %s\n"
            "\tsink: <%u> '%s'\n"
            "\tvolume: <%s>\n"
            "\tmute: <%i>\n"
            "\tlatency: <%0.0f usec>\n"
            "\tsample spec: <%s>\n"
            "\tchannel map: <%s>\n"
            "\tresample method: %s\n",
            i->index,
            i->name,
            i->driver,
            i->flags & PA_SINK_INPUT_VARIABLE_RATE ? "VARIABLE_RATE " : "",
            i->flags & PA_SINK_INPUT_DONT_MOVE ? "DONT_MOVE " : "",
            state_table[pa_sink_input_get_state(i)],
            i->sink->index, i->sink->name,
            pa_cvolume_snprint(cv, sizeof(cv), pa_sink_input_get_volume(i)),
            !!pa_sink_input_get_mute(i),
            (double) pa_sink_input_get_latency(i),
            pa_sample_spec_snprint(ss, sizeof(ss), &i->sample_spec),
            pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
            pa_resample_method_to_string(pa_sink_input_get_resample_method(i)));

        if (i->module)
            pa_strbuf_printf(s, "\tmodule: <%u>\n", i->module->index);
        if (i->client)
            pa_strbuf_printf(s, "\tclient: <%u> '%s'\n", i->client->index, i->client->name);
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_scache_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_assert(c);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u cache entries available.\n", c->scache ? pa_idxset_size(c->scache) : 0);

    if (c->scache) {
        pa_scache_entry *e;
        uint32_t idx = PA_IDXSET_INVALID;

        for (e = pa_idxset_first(c->scache, &idx); e; e = pa_idxset_next(c->scache, &idx)) {
            double l = 0;
            char ss[PA_SAMPLE_SPEC_SNPRINT_MAX] = "n/a", cv[PA_CVOLUME_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX] = "n/a";

            if (e->memchunk.memblock) {
                pa_sample_spec_snprint(ss, sizeof(ss), &e->sample_spec);
                pa_channel_map_snprint(cm, sizeof(cm), &e->channel_map);
                l = (double) e->memchunk.length / pa_bytes_per_second(&e->sample_spec);
            }

            pa_strbuf_printf(
                s,
                "    name: <%s>\n"
                "\tindex: <%u>\n"
                "\tsample spec: <%s>\n"
                "\tchannel map: <%s>\n"
                "\tlength: <%lu>\n"
                "\tduration: <%0.1fs>\n"
                "\tvolume: <%s>\n"
                "\tlazy: %s\n"
                "\tfilename: %s\n",
                e->name,
                e->index,
                ss,
                cm,
                (long unsigned)(e->memchunk.memblock ? e->memchunk.length : 0),
                l,
                pa_cvolume_snprint(cv, sizeof(cv), &e->volume),
                e->lazy ? "yes" : "no",
                e->filename ? e->filename : "n/a");
        }
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_autoload_list_to_string(pa_core *c) {
    pa_strbuf *s;
    pa_assert(c);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "%u autoload entries available.\n", c->autoload_hashmap ? pa_hashmap_size(c->autoload_hashmap) : 0);

    if (c->autoload_hashmap) {
        pa_autoload_entry *e;
        void *state = NULL;

        while ((e = pa_hashmap_iterate(c->autoload_hashmap, &state, NULL))) {
            pa_strbuf_printf(
                s, "    name: <%s>\n\ttype: <%s>\n\tindex: <%u>\n\tmodule_name: <%s>\n\targuments: <%s>\n",
                e->name,
                e->type == PA_NAMEREG_SOURCE ? "source" : "sink",
                e->index,
                e->module,
                e->argument ? e->argument : "");

        }
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_full_status_string(pa_core *c) {
    pa_strbuf *s;
    int i;

    s = pa_strbuf_new();

    for (i = 0; i < 8; i++) {
        char *t = NULL;

        switch (i) {
            case 0:
                t = pa_sink_list_to_string(c);
                break;
            case 1:
                t = pa_source_list_to_string(c);
                break;
            case 2:
                t = pa_sink_input_list_to_string(c);
                break;
            case 3:
                t = pa_source_output_list_to_string(c);
                break;
            case 4:
                t = pa_client_list_to_string(c);
                break;
            case 5:
                t = pa_module_list_to_string(c);
                break;
            case 6:
                t = pa_scache_list_to_string(c);
                break;
            case 7:
                t = pa_autoload_list_to_string(c);
                break;
        }

        pa_strbuf_puts(s, t);
        pa_xfree(t);
    }

    return pa_strbuf_tostring_free(s);
}
