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

static PA_DEFINE_CHECK_TYPE(pa_source, pa_msgobject);

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
    pa_channel_map tmap;

    pa_assert(core);
    pa_assert(name);
    pa_assert(spec);

    pa_return_null_if_fail(pa_sample_spec_valid(spec));

    if (!map)
        pa_return_null_if_fail(map = pa_channel_map_init_auto(&tmap, spec->channels, PA_CHANNEL_MAP_DEFAULT));

    pa_return_null_if_fail(map && pa_channel_map_valid(map));
    pa_return_null_if_fail(map->channels == spec->channels);
    pa_return_null_if_fail(!driver || pa_utf8_valid(driver));
    pa_return_null_if_fail(pa_utf8_valid(name) && *name);

    s = pa_msgobject_new(pa_source);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SOURCE, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->parent.parent.free = source_free;
    s->parent.process_msg = pa_source_process_msg;

    s->core = core;
    s->state = PA_SOURCE_INIT;
    s->flags = 0;
    s->name = pa_xstrdup(name);
    s->description = NULL;
    s->driver = pa_xstrdup(driver);
    s->module = NULL;

    s->sample_spec = *spec;
    s->channel_map = *map;

    s->outputs = pa_idxset_new(NULL, NULL);
    s->n_corked = 0;
    s->monitor_of = NULL;

    pa_cvolume_reset(&s->volume, spec->channels);
    s->muted = FALSE;
    s->refresh_volume = s->refresh_muted = FALSE;

    s->get_latency = NULL;
    s->set_volume = NULL;
    s->get_volume = NULL;
    s->set_mute = NULL;
    s->get_mute = NULL;
    s->set_state = NULL;
    s->userdata = NULL;

    s->asyncmsgq = NULL;
    s->rtpoll = NULL;

    pa_assert_se(pa_idxset_put(core->sources, s, &s->index) >= 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info("Created source %u \"%s\" with sample spec \"%s\"", s->index, s->name, st);

    s->thread_info.outputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    s->thread_info.soft_volume = s->volume;
    s->thread_info.soft_muted = s->muted;
    s->thread_info.state = s->state;

    return s;
}

static int source_set_state(pa_source *s, pa_source_state_t state) {
    int ret;
    pa_bool_t suspend_change;

    pa_assert(s);

    if (s->state == state)
        return 0;

    suspend_change =
        (s->state == PA_SOURCE_SUSPENDED && PA_SOURCE_OPENED(state)) ||
        (PA_SOURCE_OPENED(s->state) && state == PA_SOURCE_SUSPENDED);

    if (s->set_state)
        if ((ret = s->set_state(s, state)) < 0)
            return -1;

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL) < 0)
        return -1;

    s->state = state;

    if (suspend_change) {
        pa_source_output *o;
        uint32_t idx;

        /* We're suspending or resuming, tell everyone about it */

        for (o = PA_SOURCE_OUTPUT(pa_idxset_first(s->outputs, &idx)); o; o = PA_SOURCE_OUTPUT(pa_idxset_next(s->outputs, &idx)))
            if (o->suspend)
                o->suspend(o, state == PA_SINK_SUSPENDED);
    }

    if (state != PA_SOURCE_UNLINKED) /* if we enter UNLINKED state pa_source_unlink() will fire the apropriate events */
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED], s);

    return 0;
}

void pa_source_put(pa_source *s) {
    pa_source_assert_ref(s);

    pa_assert(s->state == PA_SINK_INIT);
    pa_assert(s->rtpoll);
    pa_assert(s->asyncmsgq);

    pa_assert_se(source_set_state(s, PA_SOURCE_IDLE) == 0);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_NEW_POST], s);
}

void pa_source_unlink(pa_source *s) {
    pa_bool_t linked;
    pa_source_output *o, *j = NULL;

    pa_assert(s);

    /* See pa_sink_unlink() for a couple of comments how this function
     * works. */

    linked = PA_SOURCE_LINKED(s->state);

    if (linked)
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], s);

    if (s->state != PA_SOURCE_UNLINKED)
        pa_namereg_unregister(s->core, s->name);
    pa_idxset_remove_by_data(s->core->sources, s, NULL);

    while ((o = pa_idxset_first(s->outputs, NULL))) {
        pa_assert(o != j);
        pa_source_output_kill(o);
        j = o;
    }

    if (linked)
        source_set_state(s, PA_SOURCE_UNLINKED);
    else
        s->state = PA_SOURCE_UNLINKED;

    s->get_latency = NULL;
    s->get_volume = NULL;
    s->set_volume = NULL;
    s->set_mute = NULL;
    s->get_mute = NULL;
    s->set_state = NULL;

    if (linked) {
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK_POST], s);
    }
}

static void source_free(pa_object *o) {
    pa_source_output *so;
    pa_source *s = PA_SOURCE(o);

    pa_assert(s);
    pa_assert(pa_source_refcnt(s) == 0);

    if (PA_SOURCE_LINKED(s->state))
        pa_source_unlink(s);

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
    pa_assert(PA_SOURCE_LINKED(s->state));

    if (s->state == PA_SOURCE_SUSPENDED)
        return 0;

    return source_set_state(s, pa_source_used_by(s) ? PA_SOURCE_RUNNING : PA_SOURCE_IDLE);
}

int pa_source_suspend(pa_source *s, pa_bool_t suspend) {
    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    if (suspend)
        return source_set_state(s, PA_SOURCE_SUSPENDED);
    else
        return source_set_state(s, pa_source_used_by(s) ? PA_SOURCE_RUNNING : PA_SOURCE_IDLE);
}

void pa_source_ping(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_PING, NULL, 0, NULL, NULL);
}

void pa_source_post(pa_source*s, const pa_memchunk *chunk) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_OPENED(s->thread_info.state));
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
    pa_assert(PA_SOURCE_LINKED(s->state));

    if (!PA_SOURCE_OPENED(s->state))
        return 0;

    if (s->get_latency)
        return s->get_latency(s);

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
        return 0;

    return usec;
}

void pa_source_set_volume(pa_source *s, const pa_cvolume *volume) {
    int changed;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));
    pa_assert(volume);

    changed = !pa_cvolume_equal(volume, &s->volume);
    s->volume = *volume;

    if (s->set_volume && s->set_volume(s) < 0)
        s->set_volume = NULL;

    if (!s->set_volume)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_VOLUME, pa_xnewdup(struct pa_cvolume, volume, 1), 0, NULL, pa_xfree);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

const pa_cvolume *pa_source_get_volume(pa_source *s) {
    pa_cvolume old_volume;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    old_volume = s->volume;

    if (s->get_volume && s->get_volume(s) < 0)
        s->get_volume = NULL;

    if (!s->get_volume && s->refresh_volume)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_VOLUME, &s->volume, 0, NULL);

    if (!pa_cvolume_equal(&old_volume, &s->volume))
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

    return &s->volume;
}

void pa_source_set_mute(pa_source *s, pa_bool_t mute) {
    int changed;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    changed = s->muted != mute;
    s->muted = mute;

    if (s->set_mute && s->set_mute(s) < 0)
        s->set_mute = NULL;

    if (!s->set_mute)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_MUTE, PA_UINT_TO_PTR(mute), 0, NULL, NULL);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

pa_bool_t pa_source_get_mute(pa_source *s) {
    pa_bool_t old_muted;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    old_muted = s->muted;

    if (s->get_mute && s->get_mute(s) < 0)
        s->get_mute = NULL;

    if (!s->get_mute && s->refresh_muted)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_MUTE, &s->muted, 0, NULL);

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

    if (PA_SOURCE_LINKED(s->state)) {
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_DESCRIPTION_CHANGED], s);
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    }
}

void pa_source_set_asyncmsgq(pa_source *s, pa_asyncmsgq *q) {
    pa_source_assert_ref(s);
    pa_assert(q);

    s->asyncmsgq = q;
}

void pa_source_set_rtpoll(pa_source *s, pa_rtpoll *p) {
    pa_source_assert_ref(s);
    pa_assert(p);

    s->rtpoll = p;
}

unsigned pa_source_linked_by(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    return pa_idxset_size(s->outputs);
}

unsigned pa_source_used_by(pa_source *s) {
    unsigned ret;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    ret = pa_idxset_size(s->outputs);
    pa_assert(ret >= s->n_corked);

    return ret - s->n_corked;
}

int pa_source_process_msg(pa_msgobject *object, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_source *s = PA_SOURCE(object);
    pa_source_assert_ref(s);
    pa_assert(s->thread_info.state != PA_SOURCE_UNLINKED);

    switch ((pa_source_message_t) code) {
        case PA_SOURCE_MESSAGE_ADD_OUTPUT: {
            pa_source_output *o = PA_SOURCE_OUTPUT(userdata);
            pa_hashmap_put(s->thread_info.outputs, PA_UINT32_TO_PTR(o->index), pa_source_output_ref(o));

            pa_assert(!o->thread_info.attached);
            o->thread_info.attached = TRUE;

            if (o->attach)
                o->attach(o);

            return 0;
        }

        case PA_SOURCE_MESSAGE_REMOVE_OUTPUT: {
            pa_source_output *o = PA_SOURCE_OUTPUT(userdata);

            if (o->detach)
                o->detach(o);

            pa_assert(o->thread_info.attached);
            o->thread_info.attached = FALSE;

            if (pa_hashmap_remove(s->thread_info.outputs, PA_UINT32_TO_PTR(o->index)))
                pa_source_output_unref(o);

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
            *((pa_bool_t*) userdata) = s->thread_info.soft_muted;
            return 0;

        case PA_SOURCE_MESSAGE_PING:
            return 0;

        case PA_SOURCE_MESSAGE_SET_STATE:
            s->thread_info.state = PA_PTR_TO_UINT(userdata);
            return 0;

        case PA_SOURCE_MESSAGE_DETACH:

            /* We're detaching all our output streams so that the
             * asyncmsgq and rtpoll fields can be changed without
             * problems */
            pa_source_detach_within_thread(s);
            break;

        case PA_SOURCE_MESSAGE_ATTACH:

            /* Reattach all streams */
            pa_source_attach_within_thread(s);
            break;

        case PA_SOURCE_MESSAGE_GET_LATENCY:
        case PA_SOURCE_MESSAGE_MAX:
            ;
    }

    return -1;
}

int pa_source_suspend_all(pa_core *c, pa_bool_t suspend) {
    uint32_t idx;
    pa_source *source;
    int ret = 0;

    pa_core_assert_ref(c);

    for (source = PA_SOURCE(pa_idxset_first(c->sources, &idx)); source; source = PA_SOURCE(pa_idxset_next(c->sources, &idx)))
        ret -= pa_source_suspend(source, suspend) < 0;

    return ret;
}

void pa_source_detach(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_DETACH, NULL, 0, NULL);
}

void pa_source_attach(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->state));

    pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_ATTACH, NULL, 0, NULL);
}

void pa_source_detach_within_thread(pa_source *s) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->thread_info.state));

    while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL)))
        if (o->detach)
            o->detach(o);
}

void pa_source_attach_within_thread(pa_source *s) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_LINKED(s->thread_info.state));

    while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL)))
        if (o->attach)
            o->attach(o);

}
