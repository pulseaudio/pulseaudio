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

#include "clitext.h"
#include "module.h"
#include "client.h"
#include "sink.h"
#include "source.h"
#include "sink-input.h"
#include "source-output.h"
#include "strbuf.h"
#include "sample-util.h"
#include "scache.h"

char *pa_module_list_to_string(struct pa_core *c) {
    struct pa_strbuf *s;
    struct pa_module *m;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u module(s) loaded.\n", pa_idxset_ncontents(c->modules));
    
    for (m = pa_idxset_first(c->modules, &index); m; m = pa_idxset_next(c->modules, &index))
        pa_strbuf_printf(s, "    index: %u\n\tname: <%s>\n\targument: <%s>\n", m->index, m->name, m->argument);
    
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
    struct pa_sink *sink, *default_sink;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u sink(s) available.\n", pa_idxset_ncontents(c->sinks));

    default_sink = pa_sink_get_default(c);
    
    for (sink = pa_idxset_first(c->sinks, &index); sink; sink = pa_idxset_next(c->sinks, &index)) {
        char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
        pa_sample_snprint(ss, sizeof(ss), &sink->sample_spec);
        assert(sink->monitor_source);
        pa_strbuf_printf(
            s,
            "  %c index: %u\n\tname: <%s>\n\tvolume: <0x%04x>\n\tlatency: <%u usec>\n\tmonitor_source: <%u>\n\tsample_spec: <%s>\n",
            sink == default_sink ? '*' : ' ',
            sink->index, sink->name,
            (unsigned) sink->volume,
            pa_sink_get_latency(sink),
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
    struct pa_source *source, *default_source;
    uint32_t index = PA_IDXSET_INVALID;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u source(s) available.\n", pa_idxset_ncontents(c->sources));

    default_source = pa_source_get_default(c);
    
    for (source = pa_idxset_first(c->sources, &index); source; source = pa_idxset_next(c->sources, &index)) {
        char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
        pa_sample_snprint(ss, sizeof(ss), &source->sample_spec);
        pa_strbuf_printf(s, "  %c index: %u\n\tname: <%s>\n\tsample_spec: <%s>\n", source == default_source ? '*' : ' ', source->index, source->name, ss);

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
        pa_sample_snprint(ss, sizeof(ss), &o->sample_spec);
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
        pa_sample_snprint(ss, sizeof(ss), &i->sample_spec);
        assert(i->sink);
        pa_strbuf_printf(
            s, "    index: %u\n\tname: <%s>\n\tsink: <%u>\n\tvolume: <0x%04x>\n\tlatency: <%u usec>\n\tsample_spec: <%s>\n",
            i->index,
            i->name,
            i->sink->index,
            (unsigned) i->volume,
            pa_sink_input_get_latency(i),
            ss);

        if (i->owner)
            pa_strbuf_printf(s, "\towner module: <%u>\n", i->owner->index);
        if (i->client)
            pa_strbuf_printf(s, "\tclient: <%u>\n", i->client->index);
    }
    
    return pa_strbuf_tostring_free(s);
}

char *pa_scache_list_to_string(struct pa_core *c) {
    struct pa_scache_entry *e;
    void *state = NULL;
    struct pa_strbuf *s;
    assert(c);

    s = pa_strbuf_new();
    assert(s);

    pa_strbuf_printf(s, "%u cache entries available.\n", c->scache_hashmap ? pa_hashmap_ncontents(c->scache_hashmap) : 0);

    if (c->scache_hashmap) {

        while ((e = pa_hashmap_iterate(c->scache_hashmap, &state))) {
            double l;
            char ss[PA_SAMPLE_SNPRINT_MAX_LENGTH];
            pa_sample_snprint(ss, sizeof(ss), &e->sample_spec);
            
            l = (double) e->memchunk.length / pa_bytes_per_second(&e->sample_spec);
            
            pa_strbuf_printf(
                s, "    name: <%s>\n\tindex: <%i>\n\tsample_spec: <%s>\n\tlength: <%u>\n\tduration: <%0.1fs>\n",
                e->name,
                e->index,
                ss,
                e->memchunk.length,
                l);
        }
    }

    return pa_strbuf_tostring_free(s);
}
