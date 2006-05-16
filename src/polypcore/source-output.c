/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <polypcore/xmalloc.h>
#include <polypcore/core-subscribe.h>
#include <polypcore/log.h>
#include <polypcore/utf8.h>

#include "source-output.h"

#define CHECK_VALIDITY_RETURN_NULL(condition) \
do {\
if (!(condition)) \
    return NULL; \
} while (0)

pa_source_output* pa_source_output_new(
        pa_source *s,
        const char *driver,
        const char *name,
        const pa_sample_spec *spec,
        const pa_channel_map *map,
        int resample_method) {
    
    pa_source_output *o;
    pa_resampler *resampler = NULL;
    int r;
    char st[256];
    pa_channel_map tmap;

    assert(s);
    assert(spec);
    assert(s->state == PA_SOURCE_RUNNING);

    CHECK_VALIDITY_RETURN_NULL(pa_sample_spec_valid(spec));

    if (!map)
        map = pa_channel_map_init_auto(&tmap, spec->channels, PA_CHANNEL_MAP_DEFAULT);
    
    CHECK_VALIDITY_RETURN_NULL(map && pa_channel_map_valid(map));
    CHECK_VALIDITY_RETURN_NULL(map->channels == spec->channels);
    CHECK_VALIDITY_RETURN_NULL(!driver || pa_utf8_valid(driver));
    CHECK_VALIDITY_RETURN_NULL(pa_utf8_valid(name));

    if (pa_idxset_size(s->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log(__FILE__": Failed to create source output: too many outputs per source.");
        return NULL;
    }

    if (resample_method == PA_RESAMPLER_INVALID)
        resample_method = s->core->resample_method;

    if (!pa_sample_spec_equal(&s->sample_spec, spec) || !pa_channel_map_equal(&s->channel_map, map))
        if (!(resampler = pa_resampler_new(&s->sample_spec, &s->channel_map, spec, map, s->core->memblock_stat, resample_method)))
            return NULL;
    
    o = pa_xnew(pa_source_output, 1);
    o->ref = 1;
    o->state = PA_SOURCE_OUTPUT_RUNNING;
    o->name = pa_xstrdup(name);
    o->driver = pa_xstrdup(driver);
    o->owner = NULL;
    o->source = s;
    o->client = NULL;
    
    o->sample_spec = *spec;
    o->channel_map = *map;

    o->push = NULL;
    o->kill = NULL;
    o->get_latency = NULL;
    o->userdata = NULL;
    
    o->resampler = resampler;
    
    assert(s->core);
    r = pa_idxset_put(s->core->source_outputs, o, &o->index);
    assert(r == 0 && o->index != PA_IDXSET_INVALID);
    r = pa_idxset_put(s->outputs, o, NULL);
    assert(r == 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info(__FILE__": created %u \"%s\" on %u with sample spec \"%s\"", o->index, o->name, s->index, st);
    
    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW, o->index);
    
    return o;    
}

void pa_source_output_disconnect(pa_source_output*o) {
    assert(o);
    assert(o->state != PA_SOURCE_OUTPUT_DISCONNECTED);
    assert(o->source);
    assert(o->source->core);
    
    pa_idxset_remove_by_data(o->source->core->source_outputs, o, NULL);
    pa_idxset_remove_by_data(o->source->outputs, o, NULL);

    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_REMOVE, o->index);
    o->source = NULL;

    o->push = NULL;
    o->kill = NULL;
    o->get_latency = NULL;
    
    o->state = PA_SOURCE_OUTPUT_DISCONNECTED;
}

static void source_output_free(pa_source_output* o) {
    assert(o);

    if (o->state != PA_SOURCE_OUTPUT_DISCONNECTED)
        pa_source_output_disconnect(o);

    pa_log_info(__FILE__": freed %u \"%s\"", o->index, o->name); 
    
    if (o->resampler)
        pa_resampler_free(o->resampler);

    pa_xfree(o->name);
    pa_xfree(o->driver);
    pa_xfree(o);
}

void pa_source_output_unref(pa_source_output* o) {
    assert(o);
    assert(o->ref >= 1);

    if (!(--o->ref))
        source_output_free(o);
}

pa_source_output* pa_source_output_ref(pa_source_output *o) {
    assert(o);
    assert(o->ref >= 1);
    
    o->ref++;
    return o;
}


void pa_source_output_kill(pa_source_output*o) {
    assert(o);
    assert(o->ref >= 1);

    if (o->kill)
        o->kill(o);
}

void pa_source_output_push(pa_source_output *o, const pa_memchunk *chunk) {
    pa_memchunk rchunk;
    
    assert(o);
    assert(chunk);
    assert(chunk->length);
    assert(o->push);

    if (o->state == PA_SOURCE_OUTPUT_CORKED)
        return;
    
    if (!o->resampler) {
        o->push(o, chunk);
        return;
    }

    pa_resampler_run(o->resampler, chunk, &rchunk);
    if (!rchunk.length)
        return;
    
    assert(rchunk.memblock);
    o->push(o, &rchunk);
    pa_memblock_unref(rchunk.memblock);
}

void pa_source_output_set_name(pa_source_output *o, const char *name) {
    assert(o);
    assert(o->ref >= 1);
    
    pa_xfree(o->name);
    o->name = pa_xstrdup(name);

    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
}

pa_usec_t pa_source_output_get_latency(pa_source_output *o) {
    assert(o);
    assert(o->ref >= 1);
    
    if (o->get_latency)
        return o->get_latency(o);

    return 0;
}

void pa_source_output_cork(pa_source_output *o, int b) {
    assert(o);
    assert(o->ref >= 1);

    if (o->state == PA_SOURCE_OUTPUT_DISCONNECTED)
        return;
    
    o->state = b ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING;
}

pa_resample_method_t pa_source_output_get_resample_method(pa_source_output *o) {
    assert(o);
    assert(o->ref >= 1);
    
    if (!o->resampler)
        return PA_RESAMPLER_INVALID;

    return pa_resampler_get_method(o->resampler);
}
