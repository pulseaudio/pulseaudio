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

#include "sink-input.h"
#include "sample-util.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "log.h"

#define CONVERT_BUFFER_LENGTH 4096

pa_sink_input* pa_sink_input_new(
    pa_sink *s,
    const char *driver,
    const char *name,
    const pa_sample_spec *spec,
    const pa_channel_map *map,
    int variable_rate,
    int resample_method) {
    
    pa_sink_input *i;
    pa_resampler *resampler = NULL;
    int r;
    char st[256];
    pa_channel_map tmap;

    assert(s);
    assert(spec);
    assert(s->state == PA_SINK_RUNNING);

    if (pa_idxset_size(s->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn(__FILE__": Failed to create sink input: too many inputs per sink.\n");
        return NULL;
    }

    if (resample_method == PA_RESAMPLER_INVALID)
        resample_method = s->core->resample_method;
    
    if (!map) {
        pa_channel_map_init_auto(&tmap, spec->channels);
        map = &tmap;
    }

    if (variable_rate || !pa_sample_spec_equal(spec, &s->sample_spec) || !pa_channel_map_equal(map, &s->channel_map))
        if (!(resampler = pa_resampler_new(spec, map, &s->sample_spec, &s->channel_map, s->core->memblock_stat, resample_method)))
            return NULL;
    
    i = pa_xnew(pa_sink_input, 1);
    i->ref = 1;
    i->state = PA_SINK_INPUT_RUNNING;
    i->name = pa_xstrdup(name);
    i->driver = pa_xstrdup(driver);
    i->owner = NULL;
    i->sink = s;
    i->client = NULL;

    i->sample_spec = *spec;
    i->channel_map = *map;

    pa_cvolume_reset(&i->volume, spec->channels);
    
    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->underrun = NULL;
    i->userdata = NULL;

    i->playing = 0;

    pa_memchunk_reset(&i->resampled_chunk);
    i->resampler = resampler;
    
    assert(s->core);
    r = pa_idxset_put(s->core->sink_inputs, i, &i->index);
    assert(r == 0 && i->index != PA_IDXSET_INVALID);
    r = pa_idxset_put(s->inputs, i, NULL);
    assert(r == 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info(__FILE__": created %u \"%s\" on %u with sample spec \"%s\"\n", i->index, i->name, s->index, st);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, i->index);
    
    return i;    
}

void pa_sink_input_disconnect(pa_sink_input *i) {
    assert(i);
    assert(i->state != PA_SINK_INPUT_DISCONNECTED);
    assert(i->sink);
    assert(i->sink->core);

    pa_idxset_remove_by_data(i->sink->core->sink_inputs, i, NULL);
    pa_idxset_remove_by_data(i->sink->inputs, i, NULL);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_REMOVE, i->index);
    i->sink = NULL;

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->underrun = NULL;

    i->playing = 0;
    i->state = PA_SINK_INPUT_DISCONNECTED;
}

static void sink_input_free(pa_sink_input* i) {
    assert(i);

    if (i->state != PA_SINK_INPUT_DISCONNECTED)
        pa_sink_input_disconnect(i);

    pa_log_info(__FILE__": freed %u \"%s\"\n", i->index, i->name); 
    
    if (i->resampled_chunk.memblock)
        pa_memblock_unref(i->resampled_chunk.memblock);
    
    if (i->resampler)
        pa_resampler_free(i->resampler);

    pa_xfree(i->name);
    pa_xfree(i->driver);
    pa_xfree(i);
}

void pa_sink_input_unref(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);

    if (!(--i->ref))
        sink_input_free(i);
}

pa_sink_input* pa_sink_input_ref(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);
    
    i->ref++;
    return i;
}

void pa_sink_input_kill(pa_sink_input*i) {
    assert(i);
    assert(i->ref >= 1);

    if (i->kill)
        i->kill(i);
}

pa_usec_t pa_sink_input_get_latency(pa_sink_input *i) {
    pa_usec_t r = 0;
    
    assert(i);
    assert(i->ref >= 1);
    
    if (i->get_latency)
        r += i->get_latency(i);

    if (i->resampled_chunk.memblock)
        r += pa_bytes_to_usec(i->resampled_chunk.length, &i->sample_spec);

    return r;
}

int pa_sink_input_peek(pa_sink_input *i, pa_memchunk *chunk, pa_cvolume *volume) {
    int ret = -1;
    int do_volume_adj_here;
    
    assert(i);
    assert(i->ref >= 1);
    assert(chunk);
    assert(volume);

    pa_sink_input_ref(i);

    if (!i->peek || !i->drop || i->state == PA_SINK_INPUT_CORKED)
        goto finish;

    if (!i->resampler) {
        do_volume_adj_here = 0;
        ret = i->peek(i, chunk);
        goto finish;
    }

    do_volume_adj_here = !pa_channel_map_equal(&i->channel_map, &i->sink->channel_map);
    
    while (!i->resampled_chunk.memblock) {
        pa_memchunk tchunk;
        size_t l;
        
        if ((ret = i->peek(i, &tchunk)) < 0)
            goto finish;

        assert(tchunk.length);

        /* It might be necessary to adjust the volume here */
        if (do_volume_adj_here) {
            pa_memchunk_make_writable(&tchunk, i->sink->core->memblock_stat, 0);
            pa_volume_memchunk(&tchunk, &i->sample_spec, &i->volume);
        }
        
        l = pa_resampler_request(i->resampler, CONVERT_BUFFER_LENGTH);

        if (l > tchunk.length)
            l = tchunk.length;

        i->drop(i, &tchunk, l);
        tchunk.length = l;

        pa_resampler_run(i->resampler, &tchunk, &i->resampled_chunk);
        pa_memblock_unref(tchunk.memblock);
    }

    assert(i->resampled_chunk.memblock);
    assert(i->resampled_chunk.length);
    
    *chunk = i->resampled_chunk;
    pa_memblock_ref(i->resampled_chunk.memblock);

    ret = 0;

finish:

    if (ret < 0 && i->playing && i->underrun)
        i->underrun(i);

    i->playing = ret >= 0;

    if (ret >= 0) {
        /* Let's see if we had to apply the volume adjustment
         * ourselves, or if this can be done by the sink for us */

        if (do_volume_adj_here)
            /* We've both the same channel map, so let's have the sink do the adjustment for us*/

            pa_cvolume_reset(volume, i->sample_spec.channels);
        else
            /* We had different channel maps, so we already did the adjustment */
            *volume = i->volume;
    }
    
    pa_sink_input_unref(i);
    
    return ret;
}

void pa_sink_input_drop(pa_sink_input *i, const pa_memchunk *chunk, size_t length) {
    assert(i);
    assert(i->ref >= 1);
    assert(length > 0);

    if (!i->resampler) {
        if (i->drop)
            i->drop(i, chunk, length);
        return;
    }
    
    assert(i->resampled_chunk.memblock);
    assert(i->resampled_chunk.length >= length);

    i->resampled_chunk.index += length;
    i->resampled_chunk.length -= length;

    if (i->resampled_chunk.length <= 0) {
        pa_memblock_unref(i->resampled_chunk.memblock);
        i->resampled_chunk.memblock = NULL;
        i->resampled_chunk.index = i->resampled_chunk.length = 0;
    }
}

void pa_sink_input_set_volume(pa_sink_input *i, const pa_cvolume *volume) {
    assert(i);
    assert(i->ref >= 1);
    assert(i->sink);
    assert(i->sink->core);

    if (pa_cvolume_equal(&i->volume, volume))
        return;
        
    i->volume = *volume;
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

const pa_cvolume * pa_sink_input_get_volume(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);

    return &i->volume;
}

void pa_sink_input_cork(pa_sink_input *i, int b) {
    int n;
    
    assert(i);
    assert(i->ref >= 1);

    if (i->state == PA_SINK_INPUT_DISCONNECTED)
        return;

    n = i->state == PA_SINK_INPUT_CORKED && !b;
    
    i->state = b ? PA_SINK_INPUT_CORKED : PA_SINK_INPUT_RUNNING;

    if (n)
        pa_sink_notify(i->sink);
}

void pa_sink_input_set_rate(pa_sink_input *i, uint32_t rate) {
    assert(i);
    assert(i->resampler);
    assert(i->ref >= 1);

    if (i->sample_spec.rate == rate)
        return;

    i->sample_spec.rate = rate;
    pa_resampler_set_input_rate(i->resampler, rate);
}

void pa_sink_input_set_name(pa_sink_input *i, const char *name) {
    assert(i);
    assert(i->ref >= 1);

    pa_xfree(i->name);
    i->name = pa_xstrdup(name);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

pa_resample_method_t pa_sink_input_get_resample_method(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);

    if (!i->resampler)
        return PA_RESAMPLER_INVALID;

    return pa_resampler_get_method(i->resampler);
}
