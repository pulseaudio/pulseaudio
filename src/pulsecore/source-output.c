/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>

#include "source-output.h"

#define CHECK_VALIDITY_RETURN_NULL(condition) \
do {\
if (!(condition)) \
    return NULL; \
} while (0)

pa_source_output_new_data* pa_source_output_new_data_init(pa_source_output_new_data *data) {
    assert(data);
    
    memset(data, 0, sizeof(*data));
    data->resample_method = PA_RESAMPLER_INVALID;
    return data;
}

void pa_source_output_new_data_set_channel_map(pa_source_output_new_data *data, const pa_channel_map *map) {
    assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_source_output_new_data_set_sample_spec(pa_source_output_new_data *data, const pa_sample_spec *spec) {
    assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

pa_source_output* pa_source_output_new(
        pa_core *core,
        pa_source_output_new_data *data,
        pa_source_output_flags_t flags) {
    
    pa_source_output *o;
    pa_resampler *resampler = NULL;
    int r;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX];

    assert(core);
    assert(data);

    if (!(flags & PA_SOURCE_OUTPUT_NO_HOOKS))
        if (pa_hook_fire(&core->hook_source_output_new, data) < 0)
            return NULL;

    CHECK_VALIDITY_RETURN_NULL(!data->driver || pa_utf8_valid(data->driver));
    CHECK_VALIDITY_RETURN_NULL(!data->name || pa_utf8_valid(data->name));

    if (!data->source)
        data->source = pa_namereg_get(core, NULL, PA_NAMEREG_SOURCE, 1);

    CHECK_VALIDITY_RETURN_NULL(data->source);
    CHECK_VALIDITY_RETURN_NULL(data->source->state == PA_SOURCE_RUNNING);
    
    if (!data->sample_spec_is_set)
        data->sample_spec = data->source->sample_spec;
    
    CHECK_VALIDITY_RETURN_NULL(pa_sample_spec_valid(&data->sample_spec));

    if (!data->channel_map_is_set)
        pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
    
    CHECK_VALIDITY_RETURN_NULL(pa_channel_map_valid(&data->channel_map));
    CHECK_VALIDITY_RETURN_NULL(data->channel_map.channels == data->sample_spec.channels);

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    CHECK_VALIDITY_RETURN_NULL(data->resample_method < PA_RESAMPLER_MAX);
    
    if (pa_idxset_size(data->source->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log(__FILE__": Failed to create source output: too many outputs per source.");
        return NULL;
    }

    if (!pa_sample_spec_equal(&data->sample_spec, &data->source->sample_spec) ||
        !pa_channel_map_equal(&data->channel_map, &data->source->channel_map))
        if (!(resampler = pa_resampler_new(
                      &data->source->sample_spec, &data->source->channel_map,
                      &data->sample_spec, &data->channel_map,
                      core->memblock_stat,
                      data->resample_method))) {
            pa_log_warn(__FILE__": Unsupported resampling operation.");
            return NULL;
        }
    
    o = pa_xnew(pa_source_output, 1);
    o->ref = 1;
    o->state = PA_SOURCE_OUTPUT_RUNNING;
    o->name = pa_xstrdup(data->name);
    o->driver = pa_xstrdup(data->driver);
    o->module = data->module;
    o->source = data->source;
    o->client = data->client;
    
    o->sample_spec = data->sample_spec;
    o->channel_map = data->channel_map;

    o->push = NULL;
    o->kill = NULL;
    o->get_latency = NULL;
    o->userdata = NULL;
    
    o->resampler = resampler;
    o->resample_method = data->resample_method;
    
    r = pa_idxset_put(core->source_outputs, o, &o->index);
    assert(r == 0);
    r = pa_idxset_put(o->source->outputs, o, NULL);
    assert(r == 0);

    pa_log_info(__FILE__": created %u \"%s\" on %s with sample spec %s",
                o->index,
                o->name,
                o->source->name,
                pa_sample_spec_snprint(st, sizeof(st), &o->sample_spec));
    
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW, o->index);

    /* We do not call pa_source_notify() here, because the virtual
     * functions have not yet been initialized */
    
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

    if (!o->name && !name)
        return;

    if (o->name && name && !strcmp(o->name, name))
        return;
    
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
    int n;
    
    assert(o);
    assert(o->ref >= 1);

    if (o->state == PA_SOURCE_OUTPUT_DISCONNECTED)
        return;

    n = o->state == PA_SOURCE_OUTPUT_CORKED && !b;
    
    o->state = b ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING;
    
    if (n)
        pa_source_notify(o->source);
}

pa_resample_method_t pa_source_output_get_resample_method(pa_source_output *o) {
    assert(o);
    assert(o->ref >= 1);
    
    if (!o->resampler)
        return o->resample_method;

    return pa_resampler_get_method(o->resampler);
}

int pa_source_output_move_to(pa_source_output *o, pa_source *dest) {
    pa_source *origin;
    pa_resampler *new_resampler;

    assert(o);
    assert(o->ref >= 1);
    assert(dest);

    origin = o->source;

    if (dest == origin)
        return 0;

    if (pa_idxset_size(dest->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log_warn(__FILE__": Failed to move source output: too many outputs per source.");
        return -1;
    }

    if (o->resampler &&
        pa_sample_spec_equal(&origin->sample_spec, &dest->sample_spec) &&
        pa_channel_map_equal(&origin->channel_map, &dest->channel_map))

        /* Try to reuse the old resampler if possible */
        new_resampler = o->resampler;
    
    else if (!pa_sample_spec_equal(&o->sample_spec, &dest->sample_spec) ||
        !pa_channel_map_equal(&o->channel_map, &dest->channel_map)) {

        /* Okey, we need a new resampler for the new sink */
        
        if (!(new_resampler = pa_resampler_new(
                      &dest->sample_spec, &dest->channel_map,
                      &o->sample_spec, &o->channel_map,
                      dest->core->memblock_stat,
                      o->resample_method))) {
            pa_log_warn(__FILE__": Unsupported resampling operation.");
            return -1;
        }
    }

    /* Okey, let's move it */
    pa_idxset_remove_by_data(origin->outputs, o, NULL);
    pa_idxset_put(dest->outputs, o, NULL);
    o->source = dest;

    /* Replace resampler */
    if (new_resampler != o->resampler) {
        if (o->resampler)
            pa_resampler_free(o->resampler);
        o->resampler = new_resampler;
    }

    /* Notify everyone */
    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    pa_source_notify(o->source);

    return 0;
}
