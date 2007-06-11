/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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
#include <pulsecore/macro.h>

#include "sink.h"

#define MAX_MIX_CHANNELS 32

static void sink_free(pa_object *s);

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

    pa_assert(core);
    pa_assert(name);
    pa_assert(spec);

    pa_return_null_if_fail(pa_sample_spec_valid(spec));

    if (!map)
        map = pa_channel_map_init_auto(&tmap, spec->channels, PA_CHANNEL_MAP_DEFAULT);

    pa_return_null_if_fail(map && pa_channel_map_valid(map));
    pa_return_null_if_fail(map->channels == spec->channels);
    pa_return_null_if_fail(!driver || pa_utf8_valid(driver));
    pa_return_null_if_fail(name && pa_utf8_valid(name) && *name);

    s = pa_msgobject_new(pa_sink);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SINK, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->parent.parent.free = sink_free;
    s->parent.process_msg = pa_sink_process_msg;
    
    s->core = core;
    pa_atomic_store(&s->state, PA_SINK_IDLE);
    s->name = pa_xstrdup(name);
    s->description = NULL;
    s->driver = pa_xstrdup(driver);
    s->module = NULL;

    s->sample_spec = *spec;
    s->channel_map = *map;

    s->inputs = pa_idxset_new(NULL, NULL);

    pa_cvolume_reset(&s->volume, spec->channels);
    s->muted = 0;
    s->refresh_volume = s->refresh_mute = 0;

    s->is_hardware = 0;

    s->get_latency = NULL;
    s->set_volume = NULL;
    s->get_volume = NULL;
    s->set_mute = NULL;
    s->get_mute = NULL;
    s->start = NULL;
    s->stop = NULL;
    s->userdata = NULL;

    pa_assert_se(s->asyncmsgq = pa_asyncmsgq_new(0));
    
    r = pa_idxset_put(core->sinks, s, &s->index);
    pa_assert(s->index != PA_IDXSET_INVALID && r >= 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info("Created sink %u \"%s\" with sample spec \"%s\"", s->index, s->name, st);

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

    s->thread_info.inputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    s->thread_info.soft_volume = s->volume;
    s->thread_info.soft_muted = s->muted;
    
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_NEW, s->index);

    return s;
}

static void sink_start(pa_sink *s) {
    pa_sink_state_t state;
    pa_assert(s);

    state = pa_sink_get_state(s);
    pa_return_if_fail(state == PA_SINK_IDLE || state == PA_SINK_SUSPENDED);

    pa_atomic_store(&s->state, PA_SINK_RUNNING);

    if (s->start)
        s->start(s);
    else
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_START, NULL, NULL, NULL);
}

static void sink_stop(pa_sink *s) {
    pa_sink_state_t state;
    int stop;
    
    pa_assert(s);
    state = pa_sink_get_state(s);
    pa_return_if_fail(state == PA_SINK_RUNNING || state == PA_SINK_SUSPENDED);

    stop = state == PA_SINK_RUNNING;
    pa_atomic_store(&s->state, PA_SINK_IDLE);

    if (stop) {
        if (s->stop)
            s->stop(s);
        else
            pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_STOP, NULL, NULL, NULL);
    }
}

void pa_sink_disconnect(pa_sink* s) {
    pa_sink_input *i, *j = NULL;

    pa_assert(s);
    pa_return_if_fail(pa_sink_get_state(s) != PA_SINK_DISCONNECTED);

    sink_stop(s);

    pa_atomic_store(&s->state, PA_SINK_DISCONNECTED);
    pa_namereg_unregister(s->core, s->name);

    pa_hook_fire(&s->core->hook_sink_disconnect, s);

    while ((i = pa_idxset_first(s->inputs, NULL))) {
        pa_assert(i != j);
        pa_sink_input_kill(i);
        j = i;
    }

    if (s->monitor_source)
        pa_source_disconnect(s->monitor_source);

    pa_idxset_remove_by_data(s->core->sinks, s, NULL);

    s->get_latency = NULL;
    s->get_volume = NULL;
    s->set_volume = NULL;
    s->set_mute = NULL;
    s->get_mute = NULL;
    s->start = NULL;
    s->stop = NULL;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
}

static void sink_free(pa_object *o) {
    pa_sink *s = PA_SINK(o);
            
    pa_assert(s);
    pa_assert(pa_sink_refcnt(s) == 0);

    pa_sink_disconnect(s);

    pa_log_info("Freeing sink %u \"%s\"", s->index, s->name);

    if (s->monitor_source) {
        pa_source_unref(s->monitor_source);
        s->monitor_source = NULL;
    }

    pa_idxset_free(s->inputs, NULL, NULL);
    
    pa_hashmap_free(s->thread_info.inputs, (pa_free2_cb_t) pa_sink_input_unref, NULL);

    pa_asyncmsgq_free(s->asyncmsgq);
    
    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s->driver);
    pa_xfree(s);
}

void pa_sink_update_status(pa_sink*s) {
    pa_sink_assert_ref(s);

    if (pa_sink_get_state(s) == PA_SINK_SUSPENDED)
        return;
    
    if (pa_sink_used_by(s) > 0)
        sink_start(s);
    else
        sink_stop(s);
}

void pa_sink_suspend(pa_sink *s, int suspend) {
    pa_sink_state_t state;

    pa_sink_assert_ref(s);

    state = pa_sink_get_state(s);
    pa_return_if_fail(suspend && (state == PA_SINK_RUNNING || state == PA_SINK_IDLE));
    pa_return_if_fail(!suspend && (state == PA_SINK_SUSPENDED));


    if (suspend) {
        pa_atomic_store(&s->state, PA_SINK_SUSPENDED);

        if (s->stop)
            s->stop(s);
        else
            pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_STOP, NULL, NULL, NULL);
        
    } else {
        pa_atomic_store(&s->state, PA_SINK_RUNNING);

        if (s->start)
            s->start(s);
        else
            pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_START, NULL, NULL, NULL);
    }
}

static unsigned fill_mix_info(pa_sink *s, pa_mix_info *info, unsigned maxinfo) {
    pa_sink_input *i;
    unsigned n = 0;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_assert(info);

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL))) {
        /* Increase ref counter, to make sure that this input doesn't
         * vanish while we still need it */
        pa_sink_input_ref(i);

        if (pa_sink_input_peek(i, &info->chunk, &info->volume) < 0) {
            pa_sink_input_unref(i);
            continue;
        }

        info->userdata = i;

        pa_assert(info->chunk.memblock);
        pa_assert(info->chunk.length);

        info++;
        maxinfo--;
        n++;
    }

    return n;
}

static void inputs_drop(pa_sink *s, pa_mix_info *info, unsigned maxinfo, size_t length) {
    pa_sink_assert_ref(s);
    pa_assert(info);

    for (; maxinfo > 0; maxinfo--, info++) {
        pa_sink_input *i = info->userdata;

        pa_assert(i);
        pa_assert(info->chunk.memblock);

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

    pa_sink_assert_ref(s);
    pa_assert(length);
    pa_assert(result);

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

        pa_sw_cvolume_multiply(&volume, &s->thread_info.soft_volume, &info[0].volume);

        if (s->thread_info.soft_muted || !pa_cvolume_is_norm(&volume)) {
            pa_memchunk_make_writable(result, 0);
            if (s->thread_info.soft_muted || pa_cvolume_is_muted(&volume))
                pa_silence_memchunk(result, &s->sample_spec);
            else
                pa_volume_memchunk(result, &s->sample_spec, &volume);
        }
    } else {
        void *ptr;
        result->memblock = pa_memblock_new(s->core->mempool, length);

        ptr = pa_memblock_acquire(result->memblock);
        result->length = pa_mix(info, n, ptr, length, &s->sample_spec, &s->thread_info.soft_volume, s->thread_info.soft_muted);
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

    pa_sink_assert_ref(s);
    pa_assert(target);
    pa_assert(target->memblock);
    pa_assert(target->length);

    pa_sink_ref(s);

    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        goto finish;


    if (n == 1) {
        if (target->length > info[0].chunk.length)
            target->length = info[0].chunk.length;

        if (s->thread_info.soft_muted)
            pa_silence_memchunk(target, &s->sample_spec);
        else {
            void *src, *ptr;
            pa_cvolume volume;
            
            ptr = pa_memblock_acquire(target->memblock);
            src = pa_memblock_acquire(info[0].chunk.memblock);
            
            memcpy((uint8_t*) ptr + target->index,
                   (uint8_t*) src + info[0].chunk.index,
                   target->length);
            
            pa_memblock_release(target->memblock);
            pa_memblock_release(info[0].chunk.memblock);
            
            pa_sw_cvolume_multiply(&volume, &s->thread_info.soft_volume, &info[0].volume);

            if (!pa_cvolume_is_norm(&volume))
                pa_volume_memchunk(target, &s->sample_spec, &volume);
        }
            
    } else {
        void *ptr;

        ptr = pa_memblock_acquire(target->memblock);
        
        target->length = pa_mix(info, n,
                                (uint8_t*) ptr + target->index,
                                target->length,
                                &s->sample_spec,
                                &s->thread_info.soft_volume,
                                s->thread_info.soft_muted);
    
        pa_memblock_release(target->memblock);
    }
    
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

    pa_sink_assert_ref(s);
    pa_assert(target);
    pa_assert(target->memblock);
    pa_assert(target->length);

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
    pa_sink_assert_ref(s);
    pa_assert(length);
    pa_assert(result);

    /*** This needs optimization ***/

    result->memblock = pa_memblock_new(s->core->mempool, result->length = length);
    result->index = 0;

    pa_sink_render_into_full(s, result);
}

pa_usec_t pa_sink_get_latency(pa_sink *s) {
    pa_usec_t usec = 0;
    
    pa_sink_assert_ref(s);

    if (s->get_latency)
        return s->get_latency(s);
    
    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_LATENCY, &usec, NULL) < 0)
        return 0;

    return usec;
}

void pa_sink_set_volume(pa_sink *s, const pa_cvolume *volume) {
    int changed;

    pa_sink_assert_ref(s);
    pa_assert(volume);

    changed = !pa_cvolume_equal(volume, &s->volume);
    s->volume = *volume;
    
    if (s->set_volume && s->set_volume(s) < 0)
        s->set_volume = NULL;

    if (!s->set_volume)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_VOLUME, pa_xnewdup(struct pa_cvolume, volume, 1), NULL, pa_xfree);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

const pa_cvolume *pa_sink_get_volume(pa_sink *s) {
    struct pa_cvolume old_volume;

    pa_sink_assert_ref(s);

    old_volume = s->volume;
    
    if (s->get_volume && s->get_volume(s) < 0)
        s->get_volume = NULL;

    if (!s->get_volume && s->refresh_volume)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_VOLUME, &s->volume, NULL);

    if (!pa_cvolume_equal(&old_volume, &s->volume))
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    
    return &s->volume;
}

void pa_sink_set_mute(pa_sink *s, int mute) {
    int changed;
    
    pa_sink_assert_ref(s);

    changed = s->muted != mute;

    if (s->set_mute && s->set_mute(s) < 0)
        s->set_mute = NULL;

    if (!s->set_mute)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_MUTE, PA_UINT_TO_PTR(mute), NULL, NULL);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

int pa_sink_get_mute(pa_sink *s) {
    int old_muted;
    
    pa_sink_assert_ref(s);

    old_muted = s->muted;
    
    if (s->get_mute && s->get_mute(s) < 0)
        s->get_mute = NULL;

    if (!s->get_mute && s->refresh_mute)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_MUTE, &s->muted, NULL);

    if (old_muted != s->muted)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    
    return s->muted;
}

void pa_sink_set_module(pa_sink *s, pa_module *m) {
    pa_sink_assert_ref(s);

    if (s->module == m)
        return;

    s->module = m;

    if (s->monitor_source)
        pa_source_set_module(s->monitor_source, m);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

void pa_sink_set_description(pa_sink *s, const char *description) {
    pa_sink_assert_ref(s);

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

    pa_sink_assert_ref(s);

    ret = pa_idxset_size(s->inputs);

    if (s->monitor_source)
        ret += pa_source_used_by(s->monitor_source);

    return ret;
}

int pa_sink_process_msg(pa_msgobject *o, int code, void *userdata, pa_memchunk *chunk) {
    pa_sink *s = PA_SINK(o);
    pa_sink_assert_ref(s);

    switch (code) {
        case PA_SINK_MESSAGE_ADD_INPUT: {
            pa_sink_input *i = userdata;
            pa_hashmap_put(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index), pa_sink_input_ref(i));
            return 0;
        }
            
        case PA_SINK_MESSAGE_REMOVE_INPUT: {
            pa_sink_input *i = userdata;
            pa_hashmap_remove(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index));
            return 0;
        }
            
        case PA_SINK_MESSAGE_SET_VOLUME:
            s->thread_info.soft_volume = *((pa_cvolume*) userdata);
            return 0;
            
        case PA_SINK_MESSAGE_SET_MUTE:
            s->thread_info.soft_muted = PA_PTR_TO_UINT(userdata);
            return 0;
            
        case PA_SINK_MESSAGE_GET_VOLUME:
            *((pa_cvolume*) userdata) = s->thread_info.soft_volume;
            return 0;
            
        case PA_SINK_MESSAGE_GET_MUTE:
            *((int*) userdata) = s->thread_info.soft_muted;
            return 0;
            
        default:
            return -1;
    }
}
