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

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "source-output.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "log.h"

struct pa_source_output* pa_source_output_new(struct pa_source *s, const char *name, const struct pa_sample_spec *spec, int resample_method) {
    struct pa_source_output *o;
    struct pa_resampler *resampler = NULL;
    int r;
    assert(s && spec);

    if (pa_idxset_ncontents(s->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log(__FILE__": Failed to create source output: too many outputs per source.\n");
        return NULL;
    }

    if (resample_method < 0)
        resample_method = s->core->resample_method;

    if (!pa_sample_spec_equal(&s->sample_spec, spec))
        if (!(resampler = pa_resampler_new(&s->sample_spec, spec, s->core->memblock_stat, resample_method)))
            return NULL;
    
    o = pa_xmalloc(sizeof(struct pa_source_output));
    o->ref = 1;
    o->state = PA_SOURCE_OUTPUT_RUNNING;
    o->name = pa_xstrdup(name);
    o->client = NULL;
    o->owner = NULL;
    o->source = s;
    o->sample_spec = *spec;

    o->push = NULL;
    o->kill = NULL;
    o->userdata = NULL;
    o->get_latency = NULL;
    
    o->resampler = resampler;
    
    assert(s->core);
    r = pa_idxset_put(s->core->source_outputs, o, &o->index);
    assert(r == 0 && o->index != PA_IDXSET_INVALID);
    r = pa_idxset_put(s->outputs, o, NULL);
    assert(r == 0);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW, o->index);
    
    return o;    
}

void pa_source_output_disconnect(struct pa_source_output*o) {
    assert(o && o->state != PA_SOURCE_OUTPUT_DISCONNECTED && o->source && o->source->core);
    
    pa_idxset_remove_by_data(o->source->core->source_outputs, o, NULL);
    pa_idxset_remove_by_data(o->source->outputs, o, NULL);

    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_REMOVE, o->index);
    o->source = NULL;

    o->push = NULL;
    o->kill = NULL;
    
    
    o->state = PA_SOURCE_OUTPUT_DISCONNECTED;
}

static void source_output_free(struct pa_source_output* o) {
    assert(o);

    if (o->state != PA_SOURCE_OUTPUT_DISCONNECTED)
        pa_source_output_disconnect(o);

    if (o->resampler)
        pa_resampler_free(o->resampler);

    pa_xfree(o->name);
    pa_xfree(o);
}


void pa_source_output_unref(struct pa_source_output* o) {
    assert(o && o->ref >= 1);

    if (!(--o->ref))
        source_output_free(o);
}

struct pa_source_output* pa_source_output_ref(struct pa_source_output *o) {
    assert(o && o->ref >= 1);
    o->ref++;
    return o;
}


void pa_source_output_kill(struct pa_source_output*o) {
    assert(o && o->ref >= 1);

    if (o->kill)
        o->kill(o);
}

void pa_source_output_push(struct pa_source_output *o, const struct pa_memchunk *chunk) {
    struct pa_memchunk rchunk;
    assert(o && chunk && chunk->length && o->push);

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

void pa_source_output_set_name(struct pa_source_output *o, const char *name) {
    assert(o && o->ref >= 1);
    pa_xfree(o->name);
    o->name = pa_xstrdup(name);

    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
}

pa_usec_t pa_source_output_get_latency(struct pa_source_output *o) {
    assert(o && o->ref >= 1);
    
    if (o->get_latency)
        return o->get_latency(o);

    return 0;
}

void pa_source_output_cork(struct pa_source_output *o, int b) {
    assert(o && o->ref >= 1);

    if (o->state == PA_SOURCE_OUTPUT_DISCONNECTED)
        return;
    
    o->state = b ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING;
}
