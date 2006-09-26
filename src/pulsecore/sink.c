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

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <pulse/introspect.h>
#include <pulse/utf8.h>
#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>

#include "sink.h"

#define MAX_MIX_CHANNELS 32

#define CHECK_VALIDITY_RETURN_NULL(condition) \
do {\
if (!(condition)) \
    return NULL; \
} while (0)

pa_sink* pa_sink_new(
        pa_core *core,
        const char *driver,
        const char *name,
        int fail,
        const pa_sample_spec *spec,
        const pa_channel_map *map) {
    
    pa_sink *s;
    char *n = NULL;
    char st[256];
    int r;
    pa_channel_map tmap;

    assert(core);
    assert(name);
    assert(spec);

    CHECK_VALIDITY_RETURN_NULL(pa_sample_spec_valid(spec));
    
    if (!map)
        map = pa_channel_map_init_auto(&tmap, spec->channels, PA_CHANNEL_MAP_DEFAULT);

    CHECK_VALIDITY_RETURN_NULL(map && pa_channel_map_valid(map));
    CHECK_VALIDITY_RETURN_NULL(map->channels == spec->channels);
    CHECK_VALIDITY_RETURN_NULL(!driver || pa_utf8_valid(driver));
    CHECK_VALIDITY_RETURN_NULL(pa_utf8_valid(name) && *name);
    
    s = pa_xnew(pa_sink, 1);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SINK, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->ref = 1;
    s->core = core;
    s->state = PA_SINK_RUNNING;
    s->name = pa_xstrdup(name);
    s->description = NULL;
    s->driver = pa_xstrdup(driver);
    s->owner = NULL;

    s->sample_spec = *spec;
    s->channel_map = *map;
    
    s->inputs = pa_idxset_new(NULL, NULL);

    pa_cvolume_reset(&s->sw_volume, spec->channels);
    pa_cvolume_reset(&s->hw_volume, spec->channels);
    s->sw_muted = 0;
    s->hw_muted = 0;

    s->is_hardware = 0;
    
    s->get_latency = NULL;
    s->notify = NULL;
    s->set_hw_volume = NULL;
    s->get_hw_volume = NULL;
    s->set_hw_mute = NULL;
    s->get_hw_mute = NULL;
    s->userdata = NULL;

    r = pa_idxset_put(core->sinks, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);
    
    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info("created %u \"%s\" with sample spec \"%s\"", s->index, s->name, st);

    n = pa_sprintf_malloc("%s.monitor", name);
    
    if (!(s->monitor_source = pa_source_new(core, driver, n, 0, spec, map)))
        pa_log_warn("failed to create monitor source.");
    else {
        char *d;
        s->monitor_source->monitor_of = s;
        d = pa_sprintf_malloc("Monitor Source of %s", s->name);
        pa_source_set_description(s->monitor_source, d);
        pa_xfree(d);
    }

    pa_xfree(n);
    
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    
    return s;
}

void pa_sink_disconnect(pa_sink* s) {
    pa_sink_input *i, *j = NULL;
    
    assert(s);
    assert(s->state == PA_SINK_RUNNING);

    s->state = PA_SINK_DISCONNECTED;
    pa_namereg_unregister(s->core, s->name);

    pa_hook_fire(&s->core->hook_sink_disconnect, s);
    
    while ((i = pa_idxset_first(s->inputs, NULL))) {
        assert(i != j);
        pa_sink_input_kill(i);
        j = i;
    }

    if (s->monitor_source)
        pa_source_disconnect(s->monitor_source);

    pa_idxset_remove_by_data(s->core->sinks, s, NULL);

    s->get_latency = NULL;
    s->notify = NULL;
    s->get_hw_volume = NULL;
    s->set_hw_volume = NULL;
    s->set_hw_mute = NULL;
    s->get_hw_mute = NULL;
    
    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
}

static void sink_free(pa_sink *s) {
    assert(s);
    assert(!s->ref);
    
    if (s->state != PA_SINK_DISCONNECTED)
        pa_sink_disconnect(s);

    pa_log_info("freed %u \"%s\"", s->index, s->name); 

    if (s->monitor_source) {
        pa_source_unref(s->monitor_source);
        s->monitor_source = NULL;
    }
    
    pa_idxset_free(s->inputs, NULL, NULL);

    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s->driver);
    pa_xfree(s);
}

void pa_sink_unref(pa_sink*s) {
    assert(s);
    assert(s->ref >= 1);

    if (!(--s->ref))
        sink_free(s);
}

pa_sink* pa_sink_ref(pa_sink *s) {
    assert(s);
    assert(s->ref >= 1);
    
    s->ref++;
    return s;
}

void pa_sink_notify(pa_sink*s) {
    assert(s);
    assert(s->ref >= 1);

    if (s->notify)
        s->notify(s);
}

static unsigned fill_mix_info(pa_sink *s, pa_mix_info *info, unsigned maxinfo) {
    uint32_t idx = PA_IDXSET_INVALID;
    pa_sink_input *i;
    unsigned n = 0;
    
    assert(s);
    assert(s->ref >= 1);
    assert(info);

    for (i = pa_idxset_first(s->inputs, &idx); maxinfo > 0 && i; i = pa_idxset_next(s->inputs, &idx)) {
        /* Increase ref counter, to make sure that this input doesn't
         * vanish while we still need it */
        pa_sink_input_ref(i);

        if (pa_sink_input_peek(i, &info->chunk, &info->volume) < 0) {
            pa_sink_input_unref(i);
            continue;
        }

        info->userdata = i;
        
        assert(info->chunk.memblock);
        assert(info->chunk.length);
        
        info++;
        maxinfo--;
        n++;
    }

    return n;
}

static void inputs_drop(pa_sink *s, pa_mix_info *info, unsigned maxinfo, size_t length) {
    assert(s);
    assert(s->ref >= 1);
    assert(info);

    for (; maxinfo > 0; maxinfo--, info++) {
        pa_sink_input *i = info->userdata;
        
        assert(i);
        assert(info->chunk.memblock);

        /* Drop read data */
        pa_sink_input_drop(i, &info->chunk, length);
        pa_memblock_unref(info->chunk.memblock);

        /* Decrease ref counter */
        pa_sink_input_unref(i);
        info->userdata = NULL;
    }
}
        
int pa_sink_render(pa_sink*s, size_t length, pa_memchunk *result) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    int r = -1;
    
    assert(s);
    assert(s->ref >= 1);
    assert(length);
    assert(result);

    pa_sink_ref(s);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        goto finish;

    if (n == 1) {
        pa_cvolume volume;

        *result = info[0].chunk;
        pa_memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

        pa_sw_cvolume_multiply(&volume, &s->sw_volume, &info[0].volume);
        
        if (s->sw_muted || !pa_cvolume_is_norm(&volume)) {
            pa_memchunk_make_writable(result, 0);
            if (s->sw_muted)
                pa_silence_memchunk(result, &s->sample_spec);
            else
                pa_volume_memchunk(result, &s->sample_spec, &volume);
        }
    } else {
        void *ptr;
        result->memblock = pa_memblock_new(s->core->mempool, length);
        assert(result->memblock);

/*          pa_log("mixing %i", n);  */

        ptr = pa_memblock_acquire(result->memblock);
        result->length = pa_mix(info, n, ptr, length, &s->sample_spec, &s->sw_volume, s->sw_muted);
        pa_memblock_release(result->memblock);
        
        result->index = 0;
    }

    inputs_drop(s, info, n, result->length);

    if (s->monitor_source)
        pa_source_post(s->monitor_source, result);

    r = 0;

finish:
    pa_sink_unref(s);

    return r;
}

int pa_sink_render_into(pa_sink*s, pa_memchunk *target) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    int r = -1;
    void *ptr;
    
    assert(s);
    assert(s->ref >= 1);
    assert(target);
    assert(target->memblock);
    assert(target->length);

    pa_sink_ref(s);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        goto finish;

    ptr = pa_memblock_acquire(target->memblock);
    
    if (n == 1) {
        void *src;
        pa_cvolume volume;

        if (target->length > info[0].chunk.length)
            target->length = info[0].chunk.length;

        src = pa_memblock_acquire(info[0].chunk.memblock);
        
        memcpy((uint8_t*) ptr + target->index,
               (uint8_t*) src + info[0].chunk.index,
               target->length);

        pa_memblock_release(info[0].chunk.memblock);

        pa_sw_cvolume_multiply(&volume, &s->sw_volume, &info[0].volume);

        if (s->sw_muted)
            pa_silence_memchunk(target, &s->sample_spec);        
        else if (!pa_cvolume_is_norm(&volume))
            pa_volume_memchunk(target, &s->sample_spec, &volume);
    } else
        target->length = pa_mix(info, n,
                                (uint8_t*) ptr + target->index,
                                target->length,
                                &s->sample_spec,
                                &s->sw_volume,
                                s->sw_muted);

    pa_memblock_release(target->memblock);
    
    inputs_drop(s, info, n, target->length);

    if (s->monitor_source)
        pa_source_post(s->monitor_source, target);

    r = 0;

finish:
    pa_sink_unref(s);
    
    return r;
}

void pa_sink_render_into_full(pa_sink *s, pa_memchunk *target) {
    pa_memchunk chunk;
    size_t l, d;
    
    assert(s);
    assert(s->ref >= 1);
    assert(target);
    assert(target->memblock);
    assert(target->length);

    pa_sink_ref(s);
    
    l = target->length;
    d = 0;
    while (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        
        if (pa_sink_render_into(s, &chunk) < 0)
            break;

        d += chunk.length;
        l -= chunk.length;
    }

    if (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        pa_silence_memchunk(&chunk, &s->sample_spec);
    }

    pa_sink_unref(s);
}

void pa_sink_render_full(pa_sink *s, size_t length, pa_memchunk *result) {
    assert(s);
    assert(s->ref >= 1);
    assert(length);
    assert(result);

    /*** This needs optimization ***/
    
    result->memblock = pa_memblock_new(s->core->mempool, result->length = length);
    result->index = 0;

    pa_sink_render_into_full(s, result);
}

pa_usec_t pa_sink_get_latency(pa_sink *s) {
    assert(s);
    assert(s->ref >= 1);

    if (!s->get_latency)
        return 0;

    return s->get_latency(s);
}

void pa_sink_set_owner(pa_sink *s, pa_module *m) {
    assert(s);
    assert(s->ref >= 1);

    if (s->owner == m)
        return;
    
    s->owner = m;

    if (s->monitor_source)
        pa_source_set_owner(s->monitor_source, m);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

void pa_sink_set_volume(pa_sink *s, pa_mixer_t m, const pa_cvolume *volume) {
    pa_cvolume *v;
    
    assert(s);
    assert(s->ref >= 1);
    assert(volume);

    if (m == PA_MIXER_HARDWARE && s->set_hw_volume) 
        v = &s->hw_volume;
    else
        v = &s->sw_volume;

    if (pa_cvolume_equal(v, volume))
        return;
        
    *v = *volume;

    if (v == &s->hw_volume)
        if (s->set_hw_volume(s) < 0)
            s->sw_volume =  *volume;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

const pa_cvolume *pa_sink_get_volume(pa_sink *s, pa_mixer_t m) {
    assert(s);
    assert(s->ref >= 1);

    if (m == PA_MIXER_HARDWARE && s->set_hw_volume) {

        if (s->get_hw_volume)
            s->get_hw_volume(s);
        
        return &s->hw_volume;
    } else
        return &s->sw_volume;
}

void pa_sink_set_mute(pa_sink *s, pa_mixer_t m, int mute) {
    int *t;
    
    assert(s);
    assert(s->ref >= 1);

    if (m == PA_MIXER_HARDWARE && s->set_hw_mute) 
        t = &s->hw_muted;
    else
        t = &s->sw_muted;

    if (!!*t == !!mute)
        return;
        
    *t = !!mute;

    if (t == &s->hw_muted)
        if (s->set_hw_mute(s) < 0)
            s->sw_muted = !!mute;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

int pa_sink_get_mute(pa_sink *s, pa_mixer_t m) {
    assert(s);
    assert(s->ref >= 1);

    if (m == PA_MIXER_HARDWARE && s->set_hw_mute) {

        if (s->get_hw_mute)
            s->get_hw_mute(s);
        
        return s->hw_muted;
    } else
        return s->sw_muted;
}

void pa_sink_set_description(pa_sink *s, const char *description) {
    assert(s);
    assert(s->ref >= 1);

    if (!description && !s->description)
        return;

    if (description && s->description && !strcmp(description, s->description))
        return;
    
    pa_xfree(s->description);
    s->description = pa_xstrdup(description);

    if (s->monitor_source) {
        char *n;
    
        n = pa_sprintf_malloc("Monitor Source of %s", s->description? s->description : s->name);
        pa_source_set_description(s->monitor_source, n);
        pa_xfree(n);
    }
        
    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

unsigned pa_sink_used_by(pa_sink *s) {
    unsigned ret;

    assert(s);
    assert(s->ref >= 1);

    ret = pa_idxset_size(s->inputs);

    if (s->monitor_source)
        ret += pa_source_used_by(s->monitor_source);

    return ret;
}
