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

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>

#include <pulsecore/source-output.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/sample-util.h>

#include "source.h"

static PA_DEFINE_CHECK_TYPE(pa_source, source_check_type, pa_msgobject_check_type);

static void source_free(pa_object *o);

pa_source* pa_source_new(
        pa_core *core,
        const char *driver,
        const char *name,
        int fail,
        const pa_sample_spec *spec,
        const pa_channel_map *map) {

    pa_source *s;
    char st[256];
    int r;
    pa_channel_map tmap;

    assert(core);
    assert(name);
    assert(spec);

    pa_return_null_if_fail(pa_sample_spec_valid(spec));

    if (!map)
        map = pa_channel_map_init_auto(&tmap, spec->channels, PA_CHANNEL_MAP_DEFAULT);

    pa_return_null_if_fail(map && pa_channel_map_valid(map));
    pa_return_null_if_fail(map->channels == spec->channels);
    pa_return_null_if_fail(!driver || pa_utf8_valid(driver));
    pa_return_null_if_fail(pa_utf8_valid(name) && *name);

    s = pa_msgobject_new(pa_source, source_check_type);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SOURCE, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->parent.parent.free = source_free;
    s->parent.process_msg = pa_source_process_msg;

    s->core = core;
    s->state = PA_SOURCE_IDLE;
    s->name = pa_xstrdup(name);
    s->description = NULL;
    s->driver = pa_xstrdup(driver);
    s->module = NULL;

    s->sample_spec = *spec;
    s->channel_map = *map;

    s->outputs = pa_idxset_new(NULL, NULL);
    s->monitor_of = NULL;

    pa_cvolume_reset(&s->volume, spec->channels);
    s->muted = 0;
    s->refresh_volume = s->refresh_muted = 0;

    s->is_hardware = 0;

    s->get_latency = NULL;
    s->set_volume = NULL;
    s->get_volume = NULL;
    s->set_mute = NULL;
    s->get_mute = NULL;
    s->set_state = NULL;
    s->userdata = NULL;

    s->asyncmsgq = NULL;

    r = pa_idxset_put(core->sources, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info("Created source %u \"%s\" with sample spec \"%s\"", s->index, s->name, st);

    s->thread_info.outputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    s->thread_info.soft_volume = s->volume;
    s->thread_info.soft_muted = s->muted;
    s->thread_info.state = s->state;

    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_NEW, s->index);

    return s;
}

static int source_set_state(pa_source *s, pa_source_state_t state) {
    int ret;
    
    pa_assert(s);

    if (s->state == state)
        return 0;

    if (s->set_state)
        if ((ret = s->set_state(s, state)) < 0)
            return -1;

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), NULL) < 0)
        return -1;

    s->state = state;
    return 0;
}

void pa_source_disconnect(pa_source *s) {
    pa_source_output *o, *j = NULL;

    pa_assert(s);
    pa_return_if_fail(s->state != PA_SOURCE_DISCONNECTED);

    pa_namereg_unregister(s->core, s->name);
    pa_idxset_remove_by_data(s->core->sources, s, NULL);

    pa_hook_fire(&s->core->hook_source_disconnect, s);

    while ((o = pa_idxset_first(s->outputs, NULL))) {
        pa_assert(o != j);
        pa_source_output_kill(o);
        j = o;
    }

    source_set_state(s, PA_SOURCE_DISCONNECTED);

    s->get_latency = NULL;
    s->get_volume = NULL;
    s->set_volume = NULL;
    s->set_mute = NULL;
    s->get_mute = NULL;
    s->set_state = NULL;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
}

static void source_free(pa_object *o) {
    pa_source_output *so;
    pa_source *s = PA_SOURCE(o);

    pa_assert(s);
    pa_assert(pa_source_refcnt(s) == 0);

    if (s->state != PA_SOURCE_DISCONNECTED)
        pa_source_disconnect(s);

    pa_log_info("Freeing source %u \"%s\"", s->index, s->name);

    pa_idxset_free(s->outputs, NULL, NULL);

    while ((so = pa_hashmap_steal_first(s->thread_info.outputs)))
        pa_source_output_unref(so);
    
    pa_hashmap_free(s->thread_info.outputs, NULL, NULL);

    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s->driver);
    pa_xfree(s);
}

int pa_source_update_status(pa_source*s) {
    pa_source_assert_ref(s);

    if (s->state == PA_SOURCE_SUSPENDED)
        return 0;

    return source_set_state(s, pa_source_used_by(s) ? PA_SOURCE_RUNNING : PA_SOURCE_IDLE);
}

int pa_source_suspend(pa_source *s, int suspend) {
    pa_source_assert_ref(s);

    if (suspend)
        return source_set_state(s, PA_SOURCE_SUSPENDED);
    else
        return source_set_state(s, pa_source_used_by(s) ? PA_SOURCE_RUNNING : PA_SOURCE_IDLE);
}

void pa_source_ping(pa_source *s) {
    pa_source_assert_ref(s);

    pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_PING, NULL, NULL, NULL);
}

void pa_source_post(pa_source*s, const pa_memchunk *chunk) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_assert(chunk);

    if (s->thread_info.state != PA_SOURCE_RUNNING)
        return;
    
    if (s->thread_info.soft_muted || !pa_cvolume_is_norm(&s->thread_info.soft_volume)) {
        pa_memchunk vchunk = *chunk;

        pa_memblock_ref(vchunk.memblock);
        pa_memchunk_make_writable(&vchunk, 0);

        if (s->thread_info.soft_muted || pa_cvolume_is_muted(&s->thread_info.soft_volume))
            pa_silence_memchunk(&vchunk, &s->sample_spec);
        else
            pa_volume_memchunk(&vchunk, &s->sample_spec, &s->thread_info.soft_volume);

        while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL)))
            pa_source_output_push(o, &vchunk);

        pa_memblock_unref(vchunk.memblock);
    } else {

        while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL)))
            pa_source_output_push(o, chunk);

    }
}

pa_usec_t pa_source_get_latency(pa_source *s) {
    pa_usec_t usec;

    pa_source_assert_ref(s);

    if (s->get_latency)
        return s->get_latency(s);

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_LATENCY, &usec, NULL) < 0)
        return 0;

    return usec;
}

void pa_source_set_volume(pa_source *s, const pa_cvolume *volume) {
    int changed;

    pa_source_assert_ref(s);
    pa_assert(volume);

    changed = !pa_cvolume_equal(volume, &s->volume);
    s->volume = *volume;

    if (s->set_volume && s->set_volume(s) < 0)
        s->set_volume = NULL;

    if (!s->set_volume)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_VOLUME, pa_xnewdup(struct pa_cvolume, volume, 1), NULL, pa_xfree);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

const pa_cvolume *pa_source_get_volume(pa_source *s) {
    pa_cvolume old_volume;
    pa_source_assert_ref(s);

    old_volume = s->volume;

    if (s->get_volume && s->get_volume(s) < 0)
        s->get_volume = NULL;

    if (!s->get_volume && s->refresh_volume)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_VOLUME, &s->volume, NULL);

    if (!pa_cvolume_equal(&old_volume, &s->volume))
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

    return &s->volume;
}

void pa_source_set_mute(pa_source *s, int mute) {
    int changed;

    pa_source_assert_ref(s);

    changed = s->muted != mute;

    if (s->set_mute && s->set_mute(s) < 0)
        s->set_mute = NULL;

    if (!s->set_mute)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_MUTE, PA_UINT_TO_PTR(mute), NULL, NULL);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

int pa_source_get_mute(pa_source *s) {
    int old_muted;

    pa_source_assert_ref(s);

    old_muted = s->muted;

    if (s->get_mute && s->get_mute(s) < 0)
        s->get_mute = NULL;

    if (!s->get_mute && s->refresh_muted)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_MUTE, &s->muted, NULL);

    if (old_muted != s->muted)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

    return s->muted;
}

void pa_source_set_module(pa_source *s, pa_module *m) {
    pa_source_assert_ref(s);

    if (m == s->module)
        return;

    s->module = m;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

void pa_source_set_description(pa_source *s, const char *description) {
    pa_source_assert_ref(s);

    if (!description && !s->description)
        return;

    if (description && s->description && !strcmp(description, s->description))
        return;

    pa_xfree(s->description);
    s->description = pa_xstrdup(description);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

void pa_source_set_asyncmsgq(pa_source *s, pa_asyncmsgq *q) {
    pa_source_assert_ref(s);
    pa_assert(q);

    s->asyncmsgq = q;
}

unsigned pa_source_used_by(pa_source *s) {
    pa_source_assert_ref(s);

    return pa_idxset_size(s->outputs);
}

int pa_source_process_msg(pa_msgobject *o, int code, void *userdata, pa_memchunk *chunk) {
    pa_source *s = PA_SOURCE(o);
    pa_source_assert_ref(s);

    switch ((pa_source_message_t) code) {
        case PA_SOURCE_MESSAGE_ADD_OUTPUT: {
            pa_source_output *i = userdata;
            pa_hashmap_put(s->thread_info.outputs, PA_UINT32_TO_PTR(i->index), pa_source_output_ref(i));
            return 0;
        }

        case PA_SOURCE_MESSAGE_REMOVE_OUTPUT: {
            pa_source_output *i = userdata;
            pa_hashmap_remove(s->thread_info.outputs, PA_UINT32_TO_PTR(i->index));
            return 0;
        }

        case PA_SOURCE_MESSAGE_SET_VOLUME:
            s->thread_info.soft_volume = *((pa_cvolume*) userdata);
            return 0;

        case PA_SOURCE_MESSAGE_SET_MUTE:
            s->thread_info.soft_muted = PA_PTR_TO_UINT(userdata);
            return 0;

        case PA_SOURCE_MESSAGE_GET_VOLUME:
            *((pa_cvolume*) userdata) = s->thread_info.soft_volume;
            return 0;

        case PA_SOURCE_MESSAGE_GET_MUTE:
            *((int*) userdata) = s->thread_info.soft_muted;
            return 0;

        case PA_SOURCE_MESSAGE_PING:
            return 0;

        case PA_SOURCE_MESSAGE_SET_STATE:
            s->thread_info.state = PA_PTR_TO_UINT(userdata);
            return 0;
            
        case PA_SOURCE_MESSAGE_GET_LATENCY:
        case PA_SOURCE_MESSAGE_MAX:
            ;
    }

    return -1;
}
