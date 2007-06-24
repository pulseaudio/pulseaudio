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
#define SILENCE_BUFFER_LENGTH (64*1024)

static PA_DEFINE_CHECK_TYPE(pa_sink, sink_check_type, pa_msgobject_check_type);

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

    s = pa_msgobject_new(pa_sink, sink_check_type);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SINK, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->parent.parent.free = sink_free;
    s->parent.process_msg = pa_sink_process_msg;

    s->core = core;
    s->state = PA_SINK_IDLE;
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
    s->set_state = NULL;
    s->userdata = NULL;

    s->asyncmsgq = NULL;
    s->silence = NULL;

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
    s->thread_info.state = s->state;

    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_NEW, s->index);

    return s;
}

static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    int ret;
    
    pa_assert(s);

    if (s->state == state)
        return 0;

    if (s->set_state)
        if ((ret = s->set_state(s, state)) < 0)
            return -1;

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), NULL) < 0)
        return -1;

    s->state = state;
    return 0;
}

void pa_sink_disconnect(pa_sink* s) {
    pa_sink_input *i, *j = NULL;

    pa_assert(s);
    pa_return_if_fail(s->state != PA_SINK_DISCONNECTED);

    pa_namereg_unregister(s->core, s->name);
    pa_idxset_remove_by_data(s->core->sinks, s, NULL);

    pa_hook_fire(&s->core->hook_sink_disconnect, s);

    while ((i = pa_idxset_first(s->inputs, NULL))) {
        pa_assert(i != j);
        pa_sink_input_kill(i);
        j = i;
    }

    if (s->monitor_source)
        pa_source_disconnect(s->monitor_source);

    sink_set_state(s, PA_SINK_DISCONNECTED);

    s->get_latency = NULL;
    s->get_volume = NULL;
    s->set_volume = NULL;
    s->set_mute = NULL;
    s->get_mute = NULL;
    s->set_state = NULL;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
}

static void sink_free(pa_object *o) {
    pa_sink *s = PA_SINK(o);
    pa_sink_input *i;

    pa_assert(s);
    pa_assert(pa_sink_refcnt(s) == 0);

    if (s->state != PA_SINK_DISCONNECTED)
        pa_sink_disconnect(s);

    pa_log_info("Freeing sink %u \"%s\"", s->index, s->name);

    if (s->monitor_source) {
        pa_source_unref(s->monitor_source);
        s->monitor_source = NULL;
    }

    pa_idxset_free(s->inputs, NULL, NULL);

    while ((i = pa_hashmap_steal_first(s->thread_info.inputs)))
        pa_sink_input_unref(i);
    
    pa_hashmap_free(s->thread_info.inputs, NULL, NULL);

    if (s->silence)
        pa_memblock_unref(s->silence);
    
    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s->driver);
    pa_xfree(s);
}

void pa_sink_set_asyncmsgq(pa_sink *s, pa_asyncmsgq *q) {
    pa_sink_assert_ref(s);
    pa_assert(q);

    s->asyncmsgq = q;

    if (s->monitor_source)
        pa_source_set_asyncmsgq(s->monitor_source, q);
}

int pa_sink_update_status(pa_sink*s) {
    pa_sink_assert_ref(s);

    if (s->state == PA_SINK_SUSPENDED)
        return 0;

    return sink_set_state(s, pa_sink_used_by(s) ? PA_SINK_RUNNING : PA_SINK_IDLE);
}

int pa_sink_suspend(pa_sink *s, int suspend) {
    pa_sink_assert_ref(s);

    if (suspend)
        return sink_set_state(s, PA_SINK_SUSPENDED);
    else
        return sink_set_state(s, pa_sink_used_by(s) ? PA_SINK_RUNNING : PA_SINK_IDLE);
}

void pa_sink_ping(pa_sink *s) {
    pa_sink_assert_ref(s);

    pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_PING, NULL, NULL, NULL);
}

static unsigned fill_mix_info(pa_sink *s, pa_mix_info *info, unsigned maxinfo) {
    pa_sink_input *i;
    unsigned n = 0;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_assert(info);

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)) && maxinfo > 0) {
        pa_sink_input_assert_ref(i);

        if (pa_sink_input_peek(i, &info->chunk, &info->volume) < 0)
            continue;

        info->userdata = pa_sink_input_ref(i);

        pa_assert(info->chunk.memblock);
        pa_assert(info->chunk.length > 0);

        info++;
        n++;
        maxinfo--;
    }

    return n;
}

static void inputs_drop(pa_sink *s, pa_mix_info *info, unsigned n, size_t length) {
    pa_sink_input *i;
    void *state = NULL;
    unsigned p = 0;
    unsigned n_unreffed = 0;

    pa_sink_assert_ref(s);

    /* We optimize for the case where the order of the inputs has not changed */

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL))) {
        unsigned j;
        pa_mix_info* m;

        pa_sink_input_assert_ref(i);

        m = NULL;

        /* Let's try to find the matching entry info the pa_mix_info array */
        for (j = 0; j < n; j ++) {

            if (info[p].userdata == i) {
                m = info + p;
                break;
            }

            if (++p > n)
                p = 0;
        }

        /* Drop read data */
        pa_sink_input_drop(i, m ? &m->chunk : NULL, length);

        if (m) {
            pa_sink_input_unref(m->userdata);
            m->userdata = NULL;
            if (m->chunk.memblock)
                pa_memblock_unref(m->chunk.memblock);
            pa_memchunk_reset(&m->chunk);

            n_unreffed += 1;
        }
    }

    /* Now drop references to entries that are included in the
     * pa_mix_info array but don't exist anymore */

    if (n_unreffed < n) {
        for (; n > 0; info++, n--) {
            if (info->userdata)
                pa_sink_input_unref(info->userdata);
            if (info->chunk.memblock)
                pa_memblock_unref(info->chunk.memblock);
        }
    }
}

void pa_sink_render(pa_sink*s, size_t length, pa_memchunk *result) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;

    pa_sink_assert_ref(s);
    pa_assert(length);
    pa_assert(result);

    pa_sink_ref(s);

    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n == 0) {

        if (length > SILENCE_BUFFER_LENGTH)
            length = SILENCE_BUFFER_LENGTH;

        if (!s->silence || pa_memblock_get_length(s->silence) < length) {
            if (s->silence)
                pa_memblock_unref(s->silence);
            s->silence = pa_silence_memblock_new(s->core->mempool, &s->sample_spec, length);
        }

        result->memblock = pa_memblock_ref(s->silence);
        result->length = length;
        result->index = 0;

    } else if (n == 1) {
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

    pa_sink_unref(s);
}

void pa_sink_render_into(pa_sink*s, pa_memchunk *target) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;

    pa_sink_assert_ref(s);
    pa_assert(target);
    pa_assert(target->memblock);
    pa_assert(target->length);

    pa_sink_ref(s);

    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n == 0) {
        pa_silence_memchunk(target, &s->sample_spec);
    } else if (n == 1) {
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

    pa_sink_unref(s);
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

        pa_sink_render_into(s, &chunk);

        d += chunk.length;
        l -= chunk.length;
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

    switch ((pa_sink_message_t) code) {
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

        case PA_SINK_MESSAGE_PING:
            return 0;

        case PA_SINK_MESSAGE_SET_STATE:
            s->thread_info.state = PA_PTR_TO_UINT(userdata);
            return 0;

        case PA_SINK_MESSAGE_GET_LATENCY:
        case PA_SINK_MESSAGE_MAX:
            ;
    }

    return -1;
}
