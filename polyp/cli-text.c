/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>

#include "cli-text.h"
#include "module.h"
#include "client.h"
#include "sink.h"
#include "source.h"
#include "sink-input.h"
#include "source-output.h"
#include "strbuf.h"
#include "sample-util.h"
#include "scache.h"
#include "autoload.h"

char *pa_module_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_module *m;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u module(s) loaded.\n", pa_idxset_ncontents(c->modules));
    
    for (m = pa_idxset_first(c->modules, &index); m; m = pa_idxset_next(c->modules, &index))
        pa_strbuf_printf(s, "    index: %u\n\tname: <%s>\n\targument: <%s>\n\tused: %i\n\tauto unload: %s\n", m->index, m->name, m->argument, m->n_used, m->auto_unload ? "yes" : "no");
    
    return pa_strbuf_tostring_free(s);
}

char *pa_client_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_client *client;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u client(s).\n", pa_idxset_ncontents(c->clients));
    
    for (client = pa_idxset_first(c->clients, &index); client; client = pa_idxset_next(c->clients, &index)) {
        pa_strbuf_printf(s, "    index: %u\n\tname: <%s>\n\tprotocol_name: <%s>\n", client->index, client->name, client->protocol_name);

        if (client->owner)
            pa_strbuf_printf(s, "\towner module: <%u>\n", client->owner->index);
    }
        
    return pa_strbuf_tostring_free(s);
}

char *pa_sink_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_sink *sink;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u sink(s) available.\n", pa_idxset_ncontents(c->sinks));

    for (sink = pa_idxset_first(c->sinks, &index); sink; sink = pa_idxset_next(c->sinks, &index)) {
        char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
        pa_sample_spec_snprint(ss, sizeof(ss), &sink->sample_spec);
        assert(sink->monitor_source);
        pa_strbuf_printf(
            s,
            "  %c index: %u\n\tname: <%s>\n\tvolume: <0x%04x> (%0.2fdB)\n\tlatency: <%0.0f usec>\n\tmonitor_source: <%u>\n\tsample_spec: <%s>\n",
            c->default_sink_name && !strcmp(sink->name, c->default_sink_name) ? '*' : ' ',
            sink->index, sink->name,
            (unsigned) sink->volume,
            pa_volume_to_dB(sink->volume),
            (float) pa_sink_get_latency(sink),
            sink->monitor_source->index,
            ss);

        if (sink->owner)
            pa_strbuf_printf(s, "\towner module: <%u>\n", sink->owner->index);
        if (sink->description)
            pa_strbuf_printf(s, "\tdescription: <%s>\n", sink->description);
    }
    
    return pa_strbuf_tostring_free(s);
}

char *pa_source_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_source *source;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u source(s) available.\n", pa_idxset_ncontents(c->sources));

    for (source = pa_idxset_first(c->sources, &index); source; source = pa_idxset_next(c->sources, &index)) {
        char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
        pa_sample_spec_snprint(ss, sizeof(ss), &source->sample_spec);
        pa_strbuf_printf(s, "  %c index: %u\n\tname: <%s>\n\tsample_spec: <%s>\n",
                         c->default_source_name && !strcmp(source->name, c->default_source_name) ? '*' : ' ',
                         source->index,
                         source->name,
                         ss);

        if (source->monitor_of) 
            pa_strbuf_printf(s, "\tmonitor_of: <%u>\n", source->monitor_of->index);
        if (source->owner)
            pa_strbuf_printf(s, "\towner module: <%u>\n", source->owner->index);
        if (source->description)
            pa_strbuf_printf(s, "\tdescription: <%s>\n", source->description);
    }
    
    return pa_strbuf_tostring_free(s);
}


char *pa_source_output_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_source_output *o;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u source outputs(s) available.\n", pa_idxset_ncontents(c->source_outputs));

    for (o = pa_idxset_first(c->source_outputs, &index); o; o = pa_idxset_next(c->source_outputs, &index)) {
        char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
        pa_sample_spec_snprint(ss, sizeof(ss), &o->sample_spec);
        assert(o->source);
        pa_strbuf_printf(
            s, "  index: %u\n\tname: <%s>\n\tsource: <%u>\n\tsample_spec: <%s>\n",
            o->index,
            o->name,
            o->source->index,
            ss);
        if (o->owner)
            pa_strbuf_printf(s, "\towner module: <%u>\n", o->owner->index);
        if (o->client)
            pa_strbuf_printf(s, "\tclient: <%u>\n", o->client->index);
    }
    
    return pa_strbuf_tostring_free(s);
}

char *pa_sink_input_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_sink_input *i;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u sink input(s) available.\n", pa_idxset_ncontents(c->sink_inputs));

    for (i = pa_idxset_first(c->sink_inputs, &index); i; i = pa_idxset_next(c->sink_inputs, &index)) {
        char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
        pa_sample_spec_snprint(ss, sizeof(ss), &i->sample_spec);
        assert(i->sink);
        pa_strbuf_printf(
            s, "    index: %u\n\tname: <%s>\n\tsink: <%u>\n\tvolume: <0x%04x> (%0.2fdB)\n\tlatency: <%0.0f usec>\n\tsample_spec: <%s>\n",
            i->index,
            i->name,
            i->sink->index,
            (unsigned) i->volume,
            pa_volume_to_dB(i->volume),
            (float) pa_sink_input_get_latency(i),
            ss);

        if (i->owner)
            pa_strbuf_printf(s, "\towner module: <%u>\n", i->owner->index);
        if (i->client)
            pa_strbuf_printf(s, "\tclient: <%u>\n", i->client->index);
    }
    
    return pa_strbuf_tostring_free(s);
}

char *pa_scache_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u cache entries available.\n", c->scache ? pa_idxset_ncontents(c->scache) : 0);

    if (c->scache) {
        struct pa_scache_entry *e;
        uint32_t index = PA_IDXSET_INVALID;

        for (e = pa_idxset_first(c->scache, &index); e; e = pa_idxset_next(c->scache, &index)) {
            double l = 0;
            char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH] = "n/a";
            
            if (e->memchunk.memblock) {
                pa_sample_spec_snprint(ss, sizeof(ss), &e->sample_spec);
                l = (double) e->memchunk.length / pa_bytes_per_second(&e->sample_spec);
            }
            
            pa_strbuf_printf(
                s, "    name: <%s>\n\tindex: <%i>\n\tsample_spec: <%s>\n\tlength: <%u>\n\tduration: <%0.1fs>\n\tvolume: <0x%04x>\n\tlazy: %s\n\tfilename: %s\n",
                e->name,
                e->index,
                ss,
                e->memchunk.memblock ? e->memchunk.length : 0,
                l,
                e->volume,
                e->lazy ? "yes" : "no",
                e->filename ? e->filename : "n/a");
        }
    }

    return pa_strbuf_tostring_free(s);
}

char *pa_autoload_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u autoload entries available.\n", c->autoload_hashmap ? pa_hashmap_ncontents(c->autoload_hashmap) : 0);

    if (c->autoload_hashmap) {
        struct pa_autoload_entry *e;
        void *state = NULL;

        while ((e = pa_hashmap_iterate(c->autoload_hashmap, &state))) {
            pa_strbuf_printf(
                s, "    name: <%s>\n\ttype: <%s>\n\tmodule_name: <%s>\n\targuments: <%s>\n",
                e->name,
                e->type == PA_NAMEREG_SOURCE ? "source" : "sink",
                e->module,
                e->argument);

        }
    }

    return pa_strbuf_tostring_free(s);
}
