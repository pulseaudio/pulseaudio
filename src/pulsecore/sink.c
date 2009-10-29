/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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
#include <string.h>
#include <stdio.h>

#include <pulse/introspect.h>
#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/i18n.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/play-memblockq.h>

#include "sink.h"

#define MAX_MIX_CHANNELS 32
#define MIX_BUFFER_LENGTH (PA_PAGE_SIZE)
#define ABSOLUTE_MIN_LATENCY (500)
#define ABSOLUTE_MAX_LATENCY (10*PA_USEC_PER_SEC)
#define DEFAULT_FIXED_LATENCY (250*PA_USEC_PER_MSEC)

PA_DEFINE_PUBLIC_CLASS(pa_sink, pa_msgobject);

static void sink_free(pa_object *s);

pa_sink_new_data* pa_sink_new_data_init(pa_sink_new_data *data) {
    pa_assert(data);

    pa_zero(*data);
    data->proplist = pa_proplist_new();

    return data;
}

void pa_sink_new_data_set_name(pa_sink_new_data *data, const char *name) {
    pa_assert(data);

    pa_xfree(data->name);
    data->name = pa_xstrdup(name);
}

void pa_sink_new_data_set_sample_spec(pa_sink_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

void pa_sink_new_data_set_channel_map(pa_sink_new_data *data, const pa_channel_map *map) {
    pa_assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_sink_new_data_set_volume(pa_sink_new_data *data, const pa_cvolume *volume) {
    pa_assert(data);

    if ((data->volume_is_set = !!volume))
        data->volume = *volume;
}

void pa_sink_new_data_set_muted(pa_sink_new_data *data, pa_bool_t mute) {
    pa_assert(data);

    data->muted_is_set = TRUE;
    data->muted = !!mute;
}

void pa_sink_new_data_set_port(pa_sink_new_data *data, const char *port) {
    pa_assert(data);

    pa_xfree(data->active_port);
    data->active_port = pa_xstrdup(port);
}

void pa_sink_new_data_done(pa_sink_new_data *data) {
    pa_assert(data);

    pa_proplist_free(data->proplist);

    if (data->ports) {
        pa_device_port *p;

        while ((p = pa_hashmap_steal_first(data->ports)))
            pa_device_port_free(p);

        pa_hashmap_free(data->ports, NULL, NULL);
    }

    pa_xfree(data->name);
    pa_xfree(data->active_port);
}

pa_device_port *pa_device_port_new(const char *name, const char *description, size_t extra) {
    pa_device_port *p;

    pa_assert(name);

    p = pa_xmalloc(PA_ALIGN(sizeof(pa_device_port)) + extra);
    p->name = pa_xstrdup(name);
    p->description = pa_xstrdup(description);

    p->priority = 0;

    return p;
}

void pa_device_port_free(pa_device_port *p) {
    pa_assert(p);

    pa_xfree(p->name);
    pa_xfree(p->description);
    pa_xfree(p);
}

/* Called from main context */
static void reset_callbacks(pa_sink *s) {
    pa_assert(s);

    s->set_state = NULL;
    s->get_volume = NULL;
    s->set_volume = NULL;
    s->get_mute = NULL;
    s->set_mute = NULL;
    s->request_rewind = NULL;
    s->update_requested_latency = NULL;
    s->set_port = NULL;
}

/* Called from main context */
pa_sink* pa_sink_new(
        pa_core *core,
        pa_sink_new_data *data,
        pa_sink_flags_t flags) {

    pa_sink *s;
    const char *name;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_source_new_data source_data;
    const char *dn;
    char *pt;

    pa_assert(core);
    pa_assert(data);
    pa_assert(data->name);
    pa_assert_ctl_context();

    s = pa_msgobject_new(pa_sink);

    if (!(name = pa_namereg_register(core, data->name, PA_NAMEREG_SINK, s, data->namereg_fail))) {
        pa_log_debug("Failed to register name %s.", data->name);
        pa_xfree(s);
        return NULL;
    }

    pa_sink_new_data_set_name(data, name);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_NEW], data) < 0) {
        pa_xfree(s);
        pa_namereg_unregister(core, name);
        return NULL;
    }

    /* FIXME, need to free s here on failure */

    pa_return_null_if_fail(!data->driver || pa_utf8_valid(data->driver));
    pa_return_null_if_fail(data->name && pa_utf8_valid(data->name) && data->name[0]);

    pa_return_null_if_fail(data->sample_spec_is_set && pa_sample_spec_valid(&data->sample_spec));

    if (!data->channel_map_is_set)
        pa_return_null_if_fail(pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT));

    pa_return_null_if_fail(pa_channel_map_valid(&data->channel_map));
    pa_return_null_if_fail(data->channel_map.channels == data->sample_spec.channels);

    if (!data->volume_is_set)
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);

    pa_return_null_if_fail(pa_cvolume_valid(&data->volume));
    pa_return_null_if_fail(pa_cvolume_compatible(&data->volume, &data->sample_spec));

    if (!data->muted_is_set)
        data->muted = FALSE;

    if (data->card)
        pa_proplist_update(data->proplist, PA_UPDATE_MERGE, data->card->proplist);

    pa_device_init_description(data->proplist);
    pa_device_init_icon(data->proplist, TRUE);
    pa_device_init_intended_roles(data->proplist);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_FIXATE], data) < 0) {
        pa_xfree(s);
        pa_namereg_unregister(core, name);
        return NULL;
    }

    s->parent.parent.free = sink_free;
    s->parent.process_msg = pa_sink_process_msg;

    s->core = core;
    s->state = PA_SINK_INIT;
    s->flags = flags;
    s->priority = 0;
    s->suspend_cause = 0;
    s->name = pa_xstrdup(name);
    s->proplist = pa_proplist_copy(data->proplist);
    s->driver = pa_xstrdup(pa_path_get_filename(data->driver));
    s->module = data->module;
    s->card = data->card;

    s->priority = pa_device_init_priority(s->proplist);

    s->sample_spec = data->sample_spec;
    s->channel_map = data->channel_map;

    s->inputs = pa_idxset_new(NULL, NULL);
    s->n_corked = 0;

    s->reference_volume = s->real_volume = data->volume;
    pa_cvolume_reset(&s->soft_volume, s->sample_spec.channels);
    s->base_volume = PA_VOLUME_NORM;
    s->n_volume_steps = PA_VOLUME_NORM+1;
    s->muted = data->muted;
    s->refresh_volume = s->refresh_muted = FALSE;

    reset_callbacks(s);
    s->userdata = NULL;

    s->asyncmsgq = NULL;

    /* As a minor optimization we just steal the list instead of
     * copying it here */
    s->ports = data->ports;
    data->ports = NULL;

    s->active_port = NULL;
    s->save_port = FALSE;

    if (data->active_port && s->ports)
        if ((s->active_port = pa_hashmap_get(s->ports, data->active_port)))
            s->save_port = data->save_port;

    if (!s->active_port && s->ports) {
        void *state;
        pa_device_port *p;

        PA_HASHMAP_FOREACH(p, s->ports, state)
            if (!s->active_port || p->priority > s->active_port->priority)
                s->active_port = p;
    }

    s->save_volume = data->save_volume;
    s->save_muted = data->save_muted;

    pa_silence_memchunk_get(
            &core->silence_cache,
            core->mempool,
            &s->silence,
            &s->sample_spec,
            0);

    s->thread_info.rtpoll = NULL;
    s->thread_info.inputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    s->thread_info.soft_volume =  s->soft_volume;
    s->thread_info.soft_muted = s->muted;
    s->thread_info.state = s->state;
    s->thread_info.rewind_nbytes = 0;
    s->thread_info.rewind_requested = FALSE;
    s->thread_info.max_rewind = 0;
    s->thread_info.max_request = 0;
    s->thread_info.requested_latency_valid = FALSE;
    s->thread_info.requested_latency = 0;
    s->thread_info.min_latency = ABSOLUTE_MIN_LATENCY;
    s->thread_info.max_latency = ABSOLUTE_MAX_LATENCY;
    s->thread_info.fixed_latency = flags & PA_SINK_DYNAMIC_LATENCY ? 0 : DEFAULT_FIXED_LATENCY;

    /* FIXME: This should probably be moved to pa_sink_put() */
    pa_assert_se(pa_idxset_put(core->sinks, s, &s->index) >= 0);

    if (s->card)
        pa_assert_se(pa_idxset_put(s->card->sinks, s, NULL) >= 0);

    pt = pa_proplist_to_string_sep(s->proplist, "\n    ");
    pa_log_info("Created sink %u \"%s\" with sample spec %s and channel map %s\n    %s",
                s->index,
                s->name,
                pa_sample_spec_snprint(st, sizeof(st), &s->sample_spec),
                pa_channel_map_snprint(cm, sizeof(cm), &s->channel_map),
                pt);
    pa_xfree(pt);

    pa_source_new_data_init(&source_data);
    pa_source_new_data_set_sample_spec(&source_data, &s->sample_spec);
    pa_source_new_data_set_channel_map(&source_data, &s->channel_map);
    source_data.name = pa_sprintf_malloc("%s.monitor", name);
    source_data.driver = data->driver;
    source_data.module = data->module;
    source_data.card = data->card;

    dn = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_DESCRIPTION);
    pa_proplist_setf(source_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Monitor of %s", dn ? dn : s->name);
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_CLASS, "monitor");

    s->monitor_source = pa_source_new(core, &source_data,
                                      ((flags & PA_SINK_LATENCY) ? PA_SOURCE_LATENCY : 0) |
                                      ((flags & PA_SINK_DYNAMIC_LATENCY) ? PA_SOURCE_DYNAMIC_LATENCY : 0));

    pa_source_new_data_done(&source_data);

    if (!s->monitor_source) {
        pa_sink_unlink(s);
        pa_sink_unref(s);
        return NULL;
    }

    s->monitor_source->monitor_of = s;

    pa_source_set_latency_range(s->monitor_source, s->thread_info.min_latency, s->thread_info.max_latency);
    pa_source_set_fixed_latency(s->monitor_source, s->thread_info.fixed_latency);
    pa_source_set_max_rewind(s->monitor_source, s->thread_info.max_rewind);

    return s;
}

/* Called from main context */
static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    int ret;
    pa_bool_t suspend_change;
    pa_sink_state_t original_state;

    pa_assert(s);
    pa_assert_ctl_context();

    if (s->state == state)
        return 0;

    original_state = s->state;

    suspend_change =
        (original_state == PA_SINK_SUSPENDED && PA_SINK_IS_OPENED(state)) ||
        (PA_SINK_IS_OPENED(original_state) && state == PA_SINK_SUSPENDED);

    if (s->set_state)
        if ((ret = s->set_state(s, state)) < 0)
            return ret;

    if (s->asyncmsgq)
        if ((ret = pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL)) < 0) {

            if (s->set_state)
                s->set_state(s, original_state);

            return ret;
        }

    s->state = state;

    if (state != PA_SINK_UNLINKED) { /* if we enter UNLINKED state pa_sink_unlink() will fire the apropriate events */
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], s);
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    }

    if (suspend_change) {
        pa_sink_input *i;
        uint32_t idx;

        /* We're suspending or resuming, tell everyone about it */

        PA_IDXSET_FOREACH(i, s->inputs, idx)
            if (s->state == PA_SINK_SUSPENDED &&
                (i->flags & PA_SINK_INPUT_KILL_ON_SUSPEND))
                pa_sink_input_kill(i);
            else if (i->suspend)
                i->suspend(i, state == PA_SINK_SUSPENDED);

        if (s->monitor_source)
            pa_source_sync_suspend(s->monitor_source);
    }

    return 0;
}

/* Called from main context */
void pa_sink_put(pa_sink* s) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    pa_assert(s->state == PA_SINK_INIT);

    /* The following fields must be initialized properly when calling _put() */
    pa_assert(s->asyncmsgq);
    pa_assert(s->thread_info.min_latency <= s->thread_info.max_latency);

    /* Generally, flags should be initialized via pa_sink_new(). As a
     * special exception we allow volume related flags to be set
     * between _new() and _put(). */

    if (!(s->flags & PA_SINK_HW_VOLUME_CTRL))
        s->flags |= PA_SINK_DECIBEL_VOLUME;

    if ((s->flags & PA_SINK_DECIBEL_VOLUME) && s->core->flat_volumes)
        s->flags |= PA_SINK_FLAT_VOLUME;

    /* We assume that if the sink implementor changed the default
     * volume he did so in real_volume, because that is the usual
     * place where he is supposed to place his changes.  */
    s->reference_volume = s->real_volume;

    s->thread_info.soft_volume = s->soft_volume;
    s->thread_info.soft_muted = s->muted;

    pa_assert((s->flags & PA_SINK_HW_VOLUME_CTRL) || (s->base_volume == PA_VOLUME_NORM && s->flags & PA_SINK_DECIBEL_VOLUME));
    pa_assert(!(s->flags & PA_SINK_DECIBEL_VOLUME) || s->n_volume_steps == PA_VOLUME_NORM+1);
    pa_assert(!(s->flags & PA_SINK_DYNAMIC_LATENCY) == (s->thread_info.fixed_latency != 0));
    pa_assert(!(s->flags & PA_SINK_LATENCY) == !(s->monitor_source->flags & PA_SOURCE_LATENCY));
    pa_assert(!(s->flags & PA_SINK_DYNAMIC_LATENCY) == !(s->monitor_source->flags & PA_SOURCE_DYNAMIC_LATENCY));

    pa_assert(s->monitor_source->thread_info.fixed_latency == s->thread_info.fixed_latency);
    pa_assert(s->monitor_source->thread_info.min_latency == s->thread_info.min_latency);
    pa_assert(s->monitor_source->thread_info.max_latency == s->thread_info.max_latency);

    pa_assert_se(sink_set_state(s, PA_SINK_IDLE) == 0);

    pa_source_put(s->monitor_source);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_PUT], s);
}

/* Called from main context */
void pa_sink_unlink(pa_sink* s) {
    pa_bool_t linked;
    pa_sink_input *i, *j = NULL;

    pa_assert(s);
    pa_assert_ctl_context();

    /* Please note that pa_sink_unlink() does more than simply
     * reversing pa_sink_put(). It also undoes the registrations
     * already done in pa_sink_new()! */

    /* All operations here shall be idempotent, i.e. pa_sink_unlink()
     * may be called multiple times on the same sink without bad
     * effects. */

    linked = PA_SINK_IS_LINKED(s->state);

    if (linked)
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_UNLINK], s);

    if (s->state != PA_SINK_UNLINKED)
        pa_namereg_unregister(s->core, s->name);
    pa_idxset_remove_by_data(s->core->sinks, s, NULL);

    if (s->card)
        pa_idxset_remove_by_data(s->card->sinks, s, NULL);

    while ((i = pa_idxset_first(s->inputs, NULL))) {
        pa_assert(i != j);
        pa_sink_input_kill(i);
        j = i;
    }

    if (linked)
        sink_set_state(s, PA_SINK_UNLINKED);
    else
        s->state = PA_SINK_UNLINKED;

    reset_callbacks(s);

    if (s->monitor_source)
        pa_source_unlink(s->monitor_source);

    if (linked) {
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_UNLINK_POST], s);
    }
}

/* Called from main context */
static void sink_free(pa_object *o) {
    pa_sink *s = PA_SINK(o);
    pa_sink_input *i;

    pa_assert(s);
    pa_assert_ctl_context();
    pa_assert(pa_sink_refcnt(s) == 0);

    if (PA_SINK_IS_LINKED(s->state))
        pa_sink_unlink(s);

    pa_log_info("Freeing sink %u \"%s\"", s->index, s->name);

    if (s->monitor_source) {
        pa_source_unref(s->monitor_source);
        s->monitor_source = NULL;
    }

    pa_idxset_free(s->inputs, NULL, NULL);

    while ((i = pa_hashmap_steal_first(s->thread_info.inputs)))
        pa_sink_input_unref(i);

    pa_hashmap_free(s->thread_info.inputs, NULL, NULL);

    if (s->silence.memblock)
        pa_memblock_unref(s->silence.memblock);

    pa_xfree(s->name);
    pa_xfree(s->driver);

    if (s->proplist)
        pa_proplist_free(s->proplist);

    if (s->ports) {
        pa_device_port *p;

        while ((p = pa_hashmap_steal_first(s->ports)))
            pa_device_port_free(p);

        pa_hashmap_free(s->ports, NULL, NULL);
    }

    pa_xfree(s);
}

/* Called from main context, and not while the IO thread is active, please */
void pa_sink_set_asyncmsgq(pa_sink *s, pa_asyncmsgq *q) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    s->asyncmsgq = q;

    if (s->monitor_source)
        pa_source_set_asyncmsgq(s->monitor_source, q);
}

/* Called from main context, and not while the IO thread is active, please */
void pa_sink_update_flags(pa_sink *s, pa_sink_flags_t mask, pa_sink_flags_t value) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (mask == 0)
        return;

    /* For now, allow only a minimal set of flags to be changed. */
    pa_assert((mask & ~(PA_SINK_DYNAMIC_LATENCY|PA_SINK_LATENCY)) == 0);

    s->flags = (s->flags & ~mask) | (value & mask);

    pa_source_update_flags(s->monitor_source,
                           ((mask & PA_SINK_LATENCY) ? PA_SOURCE_LATENCY : 0) |
                           ((mask & PA_SINK_DYNAMIC_LATENCY) ? PA_SOURCE_DYNAMIC_LATENCY : 0),
                           ((value & PA_SINK_LATENCY) ? PA_SOURCE_LATENCY : 0) |
                           ((value & PA_SINK_DYNAMIC_LATENCY) ? PA_SINK_DYNAMIC_LATENCY : 0));
}

/* Called from IO context, or before _put() from main context */
void pa_sink_set_rtpoll(pa_sink *s, pa_rtpoll *p) {
    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    s->thread_info.rtpoll = p;

    if (s->monitor_source)
        pa_source_set_rtpoll(s->monitor_source, p);
}

/* Called from main context */
int pa_sink_update_status(pa_sink*s) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    if (s->state == PA_SINK_SUSPENDED)
        return 0;

    return sink_set_state(s, pa_sink_used_by(s) ? PA_SINK_RUNNING : PA_SINK_IDLE);
}

/* Called from main context */
int pa_sink_suspend(pa_sink *s, pa_bool_t suspend, pa_suspend_cause_t cause) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));
    pa_assert(cause != 0);

    if (suspend) {
        s->suspend_cause |= cause;
        s->monitor_source->suspend_cause |= cause;
    } else {
        s->suspend_cause &= ~cause;
        s->monitor_source->suspend_cause &= ~cause;
    }

    if ((pa_sink_get_state(s) == PA_SINK_SUSPENDED) == !!s->suspend_cause)
        return 0;

    pa_log_debug("Suspend cause of sink %s is 0x%04x, %s", s->name, s->suspend_cause, s->suspend_cause ? "suspending" : "resuming");

    if (s->suspend_cause)
        return sink_set_state(s, PA_SINK_SUSPENDED);
    else
        return sink_set_state(s, pa_sink_used_by(s) ? PA_SINK_RUNNING : PA_SINK_IDLE);
}

/* Called from main context */
pa_queue *pa_sink_move_all_start(pa_sink *s, pa_queue *q) {
    pa_sink_input *i, *n;
    uint32_t idx;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    if (!q)
        q = pa_queue_new();

    for (i = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); i; i = n) {
        n = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx));

        pa_sink_input_ref(i);

        if (pa_sink_input_start_move(i) >= 0)
            pa_queue_push(q, i);
        else
            pa_sink_input_unref(i);
    }

    return q;
}

/* Called from main context */
void pa_sink_move_all_finish(pa_sink *s, pa_queue *q, pa_bool_t save) {
    pa_sink_input *i;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));
    pa_assert(q);

    while ((i = PA_SINK_INPUT(pa_queue_pop(q)))) {
        if (pa_sink_input_finish_move(i, s, save) < 0)
            pa_sink_input_fail_move(i);

        pa_sink_input_unref(i);
    }

    pa_queue_free(q, NULL, NULL);
}

/* Called from main context */
void pa_sink_move_all_fail(pa_queue *q) {
    pa_sink_input *i;

    pa_assert_ctl_context();
    pa_assert(q);

    while ((i = PA_SINK_INPUT(pa_queue_pop(q)))) {
        pa_sink_input_fail_move(i);
        pa_sink_input_unref(i);
    }

    pa_queue_free(q, NULL, NULL);
}

/* Called from IO thread context */
void pa_sink_process_rewind(pa_sink *s, size_t nbytes) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));

    /* If nobody requested this and this is actually no real rewind
     * then we can short cut this. Please note that this means that
     * not all rewind requests triggered upstream will always be
     * translated in actual requests! */
    if (!s->thread_info.rewind_requested && nbytes <= 0)
        return;

    s->thread_info.rewind_nbytes = 0;
    s->thread_info.rewind_requested = FALSE;

    if (s->thread_info.state == PA_SINK_SUSPENDED)
        return;

    if (nbytes > 0)
        pa_log_debug("Processing rewind...");

    PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state) {
        pa_sink_input_assert_ref(i);
        pa_sink_input_process_rewind(i, nbytes);
    }

    if (nbytes > 0)
        if (s->monitor_source && PA_SOURCE_IS_LINKED(s->monitor_source->thread_info.state))
            pa_source_process_rewind(s->monitor_source, nbytes);
}

/* Called from IO thread context */
static unsigned fill_mix_info(pa_sink *s, size_t *length, pa_mix_info *info, unsigned maxinfo) {
    pa_sink_input *i;
    unsigned n = 0;
    void *state = NULL;
    size_t mixlength = *length;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(info);

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)) && maxinfo > 0) {
        pa_sink_input_assert_ref(i);

        pa_sink_input_peek(i, *length, &info->chunk, &info->volume);

        if (mixlength == 0 || info->chunk.length < mixlength)
            mixlength = info->chunk.length;

        if (pa_memblock_is_silence(info->chunk.memblock)) {
            pa_memblock_unref(info->chunk.memblock);
            continue;
        }

        info->userdata = pa_sink_input_ref(i);

        pa_assert(info->chunk.memblock);
        pa_assert(info->chunk.length > 0);

        info++;
        n++;
        maxinfo--;
    }

    if (mixlength > 0)
        *length = mixlength;

    return n;
}

/* Called from IO thread context */
static void inputs_drop(pa_sink *s, pa_mix_info *info, unsigned n, pa_memchunk *result) {
    pa_sink_input *i;
    void *state;
    unsigned p = 0;
    unsigned n_unreffed = 0;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(result);
    pa_assert(result->memblock);
    pa_assert(result->length > 0);

    /* We optimize for the case where the order of the inputs has not changed */

    PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state) {
        unsigned j;
        pa_mix_info* m = NULL;

        pa_sink_input_assert_ref(i);

        /* Let's try to find the matching entry info the pa_mix_info array */
        for (j = 0; j < n; j ++) {

            if (info[p].userdata == i) {
                m = info + p;
                break;
            }

            p++;
            if (p >= n)
                p = 0;
        }

        /* Drop read data */
        pa_sink_input_drop(i, result->length);

        if (s->monitor_source && PA_SOURCE_IS_LINKED(s->monitor_source->thread_info.state)) {

            if (pa_hashmap_size(i->thread_info.direct_outputs) > 0) {
                void *ostate = NULL;
                pa_source_output *o;
                pa_memchunk c;

                if (m && m->chunk.memblock) {
                    c = m->chunk;
                    pa_memblock_ref(c.memblock);
                    pa_assert(result->length <= c.length);
                    c.length = result->length;

                    pa_memchunk_make_writable(&c, 0);
                    pa_volume_memchunk(&c, &s->sample_spec, &m->volume);
                } else {
                    c = s->silence;
                    pa_memblock_ref(c.memblock);
                    pa_assert(result->length <= c.length);
                    c.length = result->length;
                }

                while ((o = pa_hashmap_iterate(i->thread_info.direct_outputs, &ostate, NULL))) {
                    pa_source_output_assert_ref(o);
                    pa_assert(o->direct_on_input == i);
                    pa_source_post_direct(s->monitor_source, o, &c);
                }

                pa_memblock_unref(c.memblock);
            }
        }

        if (m) {
            if (m->chunk.memblock)
                pa_memblock_unref(m->chunk.memblock);
                pa_memchunk_reset(&m->chunk);

            pa_sink_input_unref(m->userdata);
            m->userdata = NULL;

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

    if (s->monitor_source && PA_SOURCE_IS_LINKED(s->monitor_source->thread_info.state))
        pa_source_post(s->monitor_source, result);
}

/* Called from IO thread context */
void pa_sink_render(pa_sink*s, size_t length, pa_memchunk *result) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t block_size_max;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));
    pa_assert(pa_frame_aligned(length, &s->sample_spec));
    pa_assert(result);

    pa_assert(!s->thread_info.rewind_requested);
    pa_assert(s->thread_info.rewind_nbytes == 0);

    if (s->thread_info.state == PA_SINK_SUSPENDED) {
        result->memblock = pa_memblock_ref(s->silence.memblock);
        result->index = s->silence.index;
        result->length = PA_MIN(s->silence.length, length);
        return;
    }

    pa_sink_ref(s);

    if (length <= 0)
        length = pa_frame_align(MIX_BUFFER_LENGTH, &s->sample_spec);

    block_size_max = pa_mempool_block_size_max(s->core->mempool);
    if (length > block_size_max)
        length = pa_frame_align(block_size_max, &s->sample_spec);

    pa_assert(length > 0);

    n = fill_mix_info(s, &length, info, MAX_MIX_CHANNELS);

    if (n == 0) {

        *result = s->silence;
        pa_memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

    } else if (n == 1) {
        pa_cvolume volume;

        *result = info[0].chunk;
        pa_memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

        pa_sw_cvolume_multiply(&volume, &s->thread_info.soft_volume, &info[0].volume);

        if (s->thread_info.soft_muted || pa_cvolume_is_muted(&volume)) {
            pa_memblock_unref(result->memblock);
            pa_silence_memchunk_get(&s->core->silence_cache,
                                    s->core->mempool,
                                    result,
                                    &s->sample_spec,
                                    result->length);
        } else if (!pa_cvolume_is_norm(&volume)) {
            pa_memchunk_make_writable(result, 0);
            pa_volume_memchunk(result, &s->sample_spec, &volume);
        }
    } else {
        void *ptr;
        result->memblock = pa_memblock_new(s->core->mempool, length);

        ptr = pa_memblock_acquire(result->memblock);
        result->length = pa_mix(info, n,
                                ptr, length,
                                &s->sample_spec,
                                &s->thread_info.soft_volume,
                                s->thread_info.soft_muted);
        pa_memblock_release(result->memblock);

        result->index = 0;
    }

    inputs_drop(s, info, n, result);

    pa_sink_unref(s);
}

/* Called from IO thread context */
void pa_sink_render_into(pa_sink*s, pa_memchunk *target) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t length, block_size_max;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));
    pa_assert(target);
    pa_assert(target->memblock);
    pa_assert(target->length > 0);
    pa_assert(pa_frame_aligned(target->length, &s->sample_spec));

    pa_assert(!s->thread_info.rewind_requested);
    pa_assert(s->thread_info.rewind_nbytes == 0);

    if (s->thread_info.state == PA_SINK_SUSPENDED) {
        pa_silence_memchunk(target, &s->sample_spec);
        return;
    }

    pa_sink_ref(s);

    length = target->length;
    block_size_max = pa_mempool_block_size_max(s->core->mempool);
    if (length > block_size_max)
        length = pa_frame_align(block_size_max, &s->sample_spec);

    pa_assert(length > 0);

    n = fill_mix_info(s, &length, info, MAX_MIX_CHANNELS);

    if (n == 0) {
        if (target->length > length)
            target->length = length;

        pa_silence_memchunk(target, &s->sample_spec);
    } else if (n == 1) {
        pa_cvolume volume;

        if (target->length > length)
            target->length = length;

        pa_sw_cvolume_multiply(&volume, &s->thread_info.soft_volume, &info[0].volume);

        if (s->thread_info.soft_muted || pa_cvolume_is_muted(&volume))
            pa_silence_memchunk(target, &s->sample_spec);
        else {
            pa_memchunk vchunk;

            vchunk = info[0].chunk;
            pa_memblock_ref(vchunk.memblock);

            if (vchunk.length > length)
                vchunk.length = length;

            if (!pa_cvolume_is_norm(&volume)) {
                pa_memchunk_make_writable(&vchunk, 0);
                pa_volume_memchunk(&vchunk, &s->sample_spec, &volume);
            }

            pa_memchunk_memcpy(target, &vchunk);
            pa_memblock_unref(vchunk.memblock);
        }

    } else {
        void *ptr;

        ptr = pa_memblock_acquire(target->memblock);

        target->length = pa_mix(info, n,
                                (uint8_t*) ptr + target->index, length,
                                &s->sample_spec,
                                &s->thread_info.soft_volume,
                                s->thread_info.soft_muted);

        pa_memblock_release(target->memblock);
    }

    inputs_drop(s, info, n, target);

    pa_sink_unref(s);
}

/* Called from IO thread context */
void pa_sink_render_into_full(pa_sink *s, pa_memchunk *target) {
    pa_memchunk chunk;
    size_t l, d;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));
    pa_assert(target);
    pa_assert(target->memblock);
    pa_assert(target->length > 0);
    pa_assert(pa_frame_aligned(target->length, &s->sample_spec));

    pa_assert(!s->thread_info.rewind_requested);
    pa_assert(s->thread_info.rewind_nbytes == 0);

    if (s->thread_info.state == PA_SINK_SUSPENDED) {
        pa_silence_memchunk(target, &s->sample_spec);
        return;
    }

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

/* Called from IO thread context */
void pa_sink_render_full(pa_sink *s, size_t length, pa_memchunk *result) {
    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));
    pa_assert(length > 0);
    pa_assert(pa_frame_aligned(length, &s->sample_spec));
    pa_assert(result);

    pa_assert(!s->thread_info.rewind_requested);
    pa_assert(s->thread_info.rewind_nbytes == 0);

    pa_sink_ref(s);

    pa_sink_render(s, length, result);

    if (result->length < length) {
        pa_memchunk chunk;

        pa_memchunk_make_writable(result, length);

        chunk.memblock = result->memblock;
        chunk.index = result->index + result->length;
        chunk.length = length - result->length;

        pa_sink_render_into_full(s, &chunk);

        result->length = length;
    }

    pa_sink_unref(s);
}

/* Called from main thread */
pa_usec_t pa_sink_get_latency(pa_sink *s) {
    pa_usec_t usec = 0;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    /* The returned value is supposed to be in the time domain of the sound card! */

    if (s->state == PA_SINK_SUSPENDED)
        return 0;

    if (!(s->flags & PA_SINK_LATENCY))
        return 0;

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) == 0);

    return usec;
}

/* Called from IO thread */
pa_usec_t pa_sink_get_latency_within_thread(pa_sink *s) {
    pa_usec_t usec = 0;
    pa_msgobject *o;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));

    /* The returned value is supposed to be in the time domain of the sound card! */

    if (s->thread_info.state == PA_SINK_SUSPENDED)
        return 0;

    if (!(s->flags & PA_SINK_LATENCY))
        return 0;

    o = PA_MSGOBJECT(s);

    /* FIXME: We probably should make this a proper vtable callback instead of going through process_msg() */

    if (o->process_msg(o, PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
        return -1;

    return usec;
}

/* Called from main context */
static void compute_reference_ratios(pa_sink *s) {
    uint32_t idx;
    pa_sink_input *i;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));
    pa_assert(s->flags & PA_SINK_FLAT_VOLUME);

    PA_IDXSET_FOREACH(i, s->inputs, idx) {
        unsigned c;
        pa_cvolume remapped;

        /*
         * Calculates the reference volume from the sink's reference
         * volume. This basically calculates:
         *
         * i->reference_ratio = i->volume / s->reference_volume
         */

        remapped = s->reference_volume;
        pa_cvolume_remap(&remapped, &s->channel_map, &i->channel_map);

        i->reference_ratio.channels = i->sample_spec.channels;

        for (c = 0; c < i->sample_spec.channels; c++) {

            /* We don't update when the sink volume is 0 anyway */
            if (remapped.values[c] <= PA_VOLUME_MUTED)
                continue;

            /* Don't update the reference ratio unless necessary */
            if (pa_sw_volume_multiply(
                        i->reference_ratio.values[c],
                        remapped.values[c]) == i->volume.values[c])
                continue;

            i->reference_ratio.values[c] = pa_sw_volume_divide(
                    i->volume.values[c],
                    remapped.values[c]);
        }
    }
}

/* Called from main context */
static void compute_real_ratios(pa_sink *s) {
    pa_sink_input *i;
    uint32_t idx;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));
    pa_assert(s->flags & PA_SINK_FLAT_VOLUME);

    PA_IDXSET_FOREACH(i, s->inputs, idx) {
        unsigned c;
        pa_cvolume remapped;

        /*
         * This basically calculates:
         *
         * i->real_ratio := i->volume / s->real_volume
         * i->soft_volume := i->real_ratio * i->volume_factor
         */

        remapped = s->real_volume;
        pa_cvolume_remap(&remapped, &s->channel_map, &i->channel_map);

        i->real_ratio.channels = i->sample_spec.channels;
        i->soft_volume.channels = i->sample_spec.channels;

        for (c = 0; c < i->sample_spec.channels; c++) {

            if (remapped.values[c] <= PA_VOLUME_MUTED) {
                /* We leave i->real_ratio untouched */
                i->soft_volume.values[c] = PA_VOLUME_MUTED;
                continue;
            }

            /* Don't lose accuracy unless necessary */
            if (pa_sw_volume_multiply(
                        i->real_ratio.values[c],
                        remapped.values[c]) != i->volume.values[c])

                i->real_ratio.values[c] = pa_sw_volume_divide(
                        i->volume.values[c],
                        remapped.values[c]);

            i->soft_volume.values[c] = pa_sw_volume_multiply(
                    i->real_ratio.values[c],
                    i->volume_factor.values[c]);
        }

        /* We don't copy the soft_volume to the thread_info data
         * here. That must be done by the caller */
    }
}

/* Called from main thread */
static void compute_real_volume(pa_sink *s) {
    pa_sink_input *i;
    uint32_t idx;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));
    pa_assert(s->flags & PA_SINK_FLAT_VOLUME);

    /* This determines the maximum volume of all streams and sets
     * s->real_volume accordingly. */

    if (pa_idxset_isempty(s->inputs)) {
        /* In the special case that we have no sink input we leave the
         * volume unmodified. */
        s->real_volume = s->reference_volume;
        return;
    }

    pa_cvolume_mute(&s->real_volume, s->channel_map.channels);

    /* First let's determine the new maximum volume of all inputs
     * connected to this sink */
    PA_IDXSET_FOREACH(i, s->inputs, idx) {
        pa_cvolume remapped;

        remapped = i->volume;
        pa_cvolume_remap(&remapped, &i->channel_map, &s->channel_map);
        pa_cvolume_merge(&s->real_volume, &s->real_volume, &remapped);
    }

    /* Then, let's update the real ratios/soft volumes of all inputs
     * connected to this sink */
    compute_real_ratios(s);
}

/* Called from main thread */
static void propagate_reference_volume(pa_sink *s) {
    pa_sink_input *i;
    uint32_t idx;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));
    pa_assert(s->flags & PA_SINK_FLAT_VOLUME);

    /* This is called whenever the sink volume changes that is not
     * caused by a sink input volume change. We need to fix up the
     * sink input volumes accordingly */

    PA_IDXSET_FOREACH(i, s->inputs, idx) {
        pa_cvolume old_volume, remapped;

        old_volume = i->volume;

        /* This basically calculates:
         *
         * i->volume := s->reference_volume * i->reference_ratio  */

        remapped = s->reference_volume;
        pa_cvolume_remap(&remapped, &s->channel_map, &i->channel_map);
        pa_sw_cvolume_multiply(&i->volume, &remapped, &i->reference_ratio);

        /* The volume changed, let's tell people so */
        if (!pa_cvolume_equal(&old_volume, &i->volume)) {

            if (i->volume_changed)
                i->volume_changed(i);

            pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
        }
    }
}

/* Called from main thread */
void pa_sink_set_volume(
        pa_sink *s,
        const pa_cvolume *volume,
        pa_bool_t sendmsg,
        pa_bool_t save) {

    pa_cvolume old_reference_volume;
    pa_bool_t reference_changed;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));
    pa_assert(!volume || pa_cvolume_valid(volume));
    pa_assert(volume || (s->flags & PA_SINK_FLAT_VOLUME));
    pa_assert(!volume || volume->channels == 1 || pa_cvolume_compatible(volume, &s->sample_spec));

    /* As a special exception we accept mono volumes on all sinks --
     * even on those with more complex channel maps */

    /* If volume is NULL we synchronize the sink's real and reference
     * volumes with the stream volumes. If it is not NULL we update
     * the reference_volume with it. */

    old_reference_volume = s->reference_volume;

    if (volume) {

        if (pa_cvolume_compatible(volume, &s->sample_spec))
            s->reference_volume = *volume;
        else
            pa_cvolume_scale(&s->reference_volume, pa_cvolume_max(volume));

        if (s->flags & PA_SINK_FLAT_VOLUME) {
            /* OK, propagate this volume change back to the inputs */
            propagate_reference_volume(s);

            /* And now recalculate the real volume */
            compute_real_volume(s);
        } else
            s->real_volume = s->reference_volume;

    } else {
        pa_assert(s->flags & PA_SINK_FLAT_VOLUME);

        /* Ok, let's determine the new real volume */
        compute_real_volume(s);

        /* Let's 'push' the reference volume if necessary */
        pa_cvolume_merge(&s->reference_volume, &s->reference_volume, &s->real_volume);

        /* We need to fix the reference ratios of all streams now that
         * we changed the reference volume */
        compute_reference_ratios(s);
    }

    reference_changed = !pa_cvolume_equal(&old_reference_volume, &s->reference_volume);
    s->save_volume = (!reference_changed && s->save_volume) || save;

    if (s->set_volume) {
        /* If we have a function set_volume(), then we do not apply a
         * soft volume by default. However, set_volume() is free to
         * apply one to s->soft_volume */

        pa_cvolume_reset(&s->soft_volume, s->sample_spec.channels);
        s->set_volume(s);

    } else
        /* If we have no function set_volume(), then the soft volume
         * becomes the virtual volume */
        s->soft_volume = s->real_volume;

    /* This tells the sink that soft and/or virtual volume changed */
    if (sendmsg)
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_VOLUME, NULL, 0, NULL) == 0);

    if (reference_changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

/* Called from main thread. Only to be called by sink implementor */
void pa_sink_set_soft_volume(pa_sink *s, const pa_cvolume *volume) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (!volume)
        pa_cvolume_reset(&s->soft_volume, s->sample_spec.channels);
    else
        s->soft_volume = *volume;

    if (PA_SINK_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_VOLUME, NULL, 0, NULL) == 0);
    else
        s->thread_info.soft_volume = s->soft_volume;
}

static void propagate_real_volume(pa_sink *s, const pa_cvolume *old_real_volume) {
    pa_sink_input *i;
    uint32_t idx;
    pa_cvolume old_reference_volume;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    /* This is called when the hardware's real volume changes due to
     * some external event. We copy the real volume into our
     * reference volume and then rebuild the stream volumes based on
     * i->real_ratio which should stay fixed. */

    if (pa_cvolume_equal(old_real_volume, &s->real_volume))
        return;

    old_reference_volume = s->reference_volume;

    /* 1. Make the real volume the reference volume */
    s->reference_volume = s->real_volume;

    if (s->flags & PA_SINK_FLAT_VOLUME) {

        PA_IDXSET_FOREACH(i, s->inputs, idx) {
            pa_cvolume old_volume, remapped;

            old_volume = i->volume;

            /* 2. Since the sink's reference and real volumes are equal
             * now our ratios should be too. */
            i->reference_ratio = i->real_ratio;

            /* 3. Recalculate the new stream reference volume based on the
             * reference ratio and the sink's reference volume.
             *
             * This basically calculates:
             *
             * i->volume = s->reference_volume * i->reference_ratio
             *
             * This is identical to propagate_reference_volume() */
            remapped = s->reference_volume;
            pa_cvolume_remap(&remapped, &s->channel_map, &i->channel_map);
            pa_sw_cvolume_multiply(&i->volume, &remapped, &i->reference_ratio);

            /* Notify if something changed */
            if (!pa_cvolume_equal(&old_volume, &i->volume)) {

                if (i->volume_changed)
                    i->volume_changed(i);

                pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
            }
        }
    }

    /* Something got changed in the hardware. It probably makes sense
     * to save changed hw settings given that hw volume changes not
     * triggered by PA are almost certainly done by the user. */
    s->save_volume = TRUE;

    if (!pa_cvolume_equal(&old_reference_volume, &s->reference_volume))
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

/* Called from main thread */
const pa_cvolume *pa_sink_get_volume(pa_sink *s, pa_bool_t force_refresh) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    if (s->refresh_volume || force_refresh) {
        struct pa_cvolume old_real_volume;

        old_real_volume = s->real_volume;

        if (s->get_volume)
            s->get_volume(s);

        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_VOLUME, NULL, 0, NULL) == 0);

        propagate_real_volume(s, &old_real_volume);
    }

    return &s->reference_volume;
}

/* Called from main thread */
void pa_sink_volume_changed(pa_sink *s, const pa_cvolume *new_real_volume) {
    pa_cvolume old_real_volume;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    /* The sink implementor may call this if the volume changed to make sure everyone is notified */

    old_real_volume = s->real_volume;
    s->real_volume = *new_real_volume;

    propagate_real_volume(s, &old_real_volume);
}

/* Called from main thread */
void pa_sink_set_mute(pa_sink *s, pa_bool_t mute, pa_bool_t save) {
    pa_bool_t old_muted;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    old_muted = s->muted;
    s->muted = mute;
    s->save_muted = (old_muted == s->muted && s->save_muted) || save;

    if (s->set_mute)
        s->set_mute(s);

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_MUTE, NULL, 0, NULL) == 0);

    if (old_muted != s->muted)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

/* Called from main thread */
pa_bool_t pa_sink_get_mute(pa_sink *s, pa_bool_t force_refresh) {

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    if (s->refresh_muted || force_refresh) {
        pa_bool_t old_muted = s->muted;

        if (s->get_mute)
            s->get_mute(s);

        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_MUTE, NULL, 0, NULL) == 0);

        if (old_muted != s->muted) {
            s->save_muted = TRUE;

            pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

            /* Make sure the soft mute status stays in sync */
            pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_MUTE, NULL, 0, NULL) == 0);
        }
    }

    return s->muted;
}

/* Called from main thread */
void pa_sink_mute_changed(pa_sink *s, pa_bool_t new_muted) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    /* The sink implementor may call this if the volume changed to make sure everyone is notified */

    if (s->muted == new_muted)
        return;

    s->muted = new_muted;
    s->save_muted = TRUE;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

/* Called from main thread */
pa_bool_t pa_sink_update_proplist(pa_sink *s, pa_update_mode_t mode, pa_proplist *p) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (p)
        pa_proplist_update(s->proplist, mode, p);

    if (PA_SINK_IS_LINKED(s->state)) {
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], s);
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    }

    return TRUE;
}

/* Called from main thread */
/* FIXME -- this should be dropped and be merged into pa_sink_update_proplist() */
void pa_sink_set_description(pa_sink *s, const char *description) {
    const char *old;
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (!description && !pa_proplist_contains(s->proplist, PA_PROP_DEVICE_DESCRIPTION))
        return;

    old = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_DESCRIPTION);

    if (old && description && pa_streq(old, description))
        return;

    if (description)
        pa_proplist_sets(s->proplist, PA_PROP_DEVICE_DESCRIPTION, description);
    else
        pa_proplist_unset(s->proplist, PA_PROP_DEVICE_DESCRIPTION);

    if (s->monitor_source) {
        char *n;

        n = pa_sprintf_malloc("Monitor Source of %s", description ? description : s->name);
        pa_source_set_description(s->monitor_source, n);
        pa_xfree(n);
    }

    if (PA_SINK_IS_LINKED(s->state)) {
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], s);
    }
}

/* Called from main thread */
unsigned pa_sink_linked_by(pa_sink *s) {
    unsigned ret;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    ret = pa_idxset_size(s->inputs);

    /* We add in the number of streams connected to us here. Please
     * note the asymmmetry to pa_sink_used_by()! */

    if (s->monitor_source)
        ret += pa_source_linked_by(s->monitor_source);

    return ret;
}

/* Called from main thread */
unsigned pa_sink_used_by(pa_sink *s) {
    unsigned ret;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    ret = pa_idxset_size(s->inputs);
    pa_assert(ret >= s->n_corked);

    /* Streams connected to our monitor source do not matter for
     * pa_sink_used_by()!.*/

    return ret - s->n_corked;
}

/* Called from main thread */
unsigned pa_sink_check_suspend(pa_sink *s) {
    unsigned ret;
    pa_sink_input *i;
    uint32_t idx;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (!PA_SINK_IS_LINKED(s->state))
        return 0;

    ret = 0;

    PA_IDXSET_FOREACH(i, s->inputs, idx) {
        pa_sink_input_state_t st;

        st = pa_sink_input_get_state(i);
        pa_assert(PA_SINK_INPUT_IS_LINKED(st));

        if (st == PA_SINK_INPUT_CORKED)
            continue;

        if (i->flags & PA_SINK_INPUT_DONT_INHIBIT_AUTO_SUSPEND)
            continue;

        ret ++;
    }

    if (s->monitor_source)
        ret += pa_source_check_suspend(s->monitor_source);

    return ret;
}

/* Called from the IO thread */
static void sync_input_volumes_within_thread(pa_sink *s) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state) {
        if (pa_cvolume_equal(&i->thread_info.soft_volume, &i->soft_volume))
            continue;

        i->thread_info.soft_volume = i->soft_volume;
        pa_sink_input_request_rewind(i, 0, TRUE, FALSE, FALSE);
    }
}

/* Called from IO thread, except when it is not */
int pa_sink_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink *s = PA_SINK(o);
    pa_sink_assert_ref(s);

    switch ((pa_sink_message_t) code) {

        case PA_SINK_MESSAGE_ADD_INPUT: {
            pa_sink_input *i = PA_SINK_INPUT(userdata);

            /* If you change anything here, make sure to change the
             * sink input handling a few lines down at
             * PA_SINK_MESSAGE_FINISH_MOVE, too. */

            pa_hashmap_put(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index), pa_sink_input_ref(i));

            /* Since the caller sleeps in pa_sink_input_put(), we can
             * safely access data outside of thread_info even though
             * it is mutable */

            if ((i->thread_info.sync_prev = i->sync_prev)) {
                pa_assert(i->sink == i->thread_info.sync_prev->sink);
                pa_assert(i->sync_prev->sync_next == i);
                i->thread_info.sync_prev->thread_info.sync_next = i;
            }

            if ((i->thread_info.sync_next = i->sync_next)) {
                pa_assert(i->sink == i->thread_info.sync_next->sink);
                pa_assert(i->sync_next->sync_prev == i);
                i->thread_info.sync_next->thread_info.sync_prev = i;
            }

            pa_assert(!i->thread_info.attached);
            i->thread_info.attached = TRUE;

            if (i->attach)
                i->attach(i);

            pa_sink_input_set_state_within_thread(i, i->state);

            /* The requested latency of the sink input needs to be
             * fixed up and then configured on the sink */

            if (i->thread_info.requested_sink_latency != (pa_usec_t) -1)
                pa_sink_input_set_requested_latency_within_thread(i, i->thread_info.requested_sink_latency);

            pa_sink_input_update_max_rewind(i, s->thread_info.max_rewind);
            pa_sink_input_update_max_request(i, s->thread_info.max_request);

            /* We don't rewind here automatically. This is left to the
             * sink input implementor because some sink inputs need a
             * slow start, i.e. need some time to buffer client
             * samples before beginning streaming. */

            /* In flat volume mode we need to update the volume as
             * well */
            return o->process_msg(o, PA_SINK_MESSAGE_SET_VOLUME, NULL, 0, NULL);
        }

        case PA_SINK_MESSAGE_REMOVE_INPUT: {
            pa_sink_input *i = PA_SINK_INPUT(userdata);

            /* If you change anything here, make sure to change the
             * sink input handling a few lines down at
             * PA_SINK_MESSAGE_PREPAPRE_MOVE, too. */

            if (i->detach)
                i->detach(i);

            pa_sink_input_set_state_within_thread(i, i->state);

            pa_assert(i->thread_info.attached);
            i->thread_info.attached = FALSE;

            /* Since the caller sleeps in pa_sink_input_unlink(),
             * we can safely access data outside of thread_info even
             * though it is mutable */

            pa_assert(!i->sync_prev);
            pa_assert(!i->sync_next);

            if (i->thread_info.sync_prev) {
                i->thread_info.sync_prev->thread_info.sync_next = i->thread_info.sync_prev->sync_next;
                i->thread_info.sync_prev = NULL;
            }

            if (i->thread_info.sync_next) {
                i->thread_info.sync_next->thread_info.sync_prev = i->thread_info.sync_next->sync_prev;
                i->thread_info.sync_next = NULL;
            }

            if (pa_hashmap_remove(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index)))
                pa_sink_input_unref(i);

            pa_sink_invalidate_requested_latency(s, TRUE);
            pa_sink_request_rewind(s, (size_t) -1);

            /* In flat volume mode we need to update the volume as
             * well */
            return o->process_msg(o, PA_SINK_MESSAGE_SET_VOLUME, NULL, 0, NULL);
        }

        case PA_SINK_MESSAGE_START_MOVE: {
            pa_sink_input *i = PA_SINK_INPUT(userdata);

            /* We don't support moving synchronized streams. */
            pa_assert(!i->sync_prev);
            pa_assert(!i->sync_next);
            pa_assert(!i->thread_info.sync_next);
            pa_assert(!i->thread_info.sync_prev);

            if (i->thread_info.state != PA_SINK_INPUT_CORKED) {
                pa_usec_t usec = 0;
                size_t sink_nbytes, total_nbytes;

                /* Get the latency of the sink */
                usec = pa_sink_get_latency_within_thread(s);
                sink_nbytes = pa_usec_to_bytes(usec, &s->sample_spec);
                total_nbytes = sink_nbytes + pa_memblockq_get_length(i->thread_info.render_memblockq);

                if (total_nbytes > 0) {
                    i->thread_info.rewrite_nbytes = i->thread_info.resampler ? pa_resampler_request(i->thread_info.resampler, total_nbytes) : total_nbytes;
                    i->thread_info.rewrite_flush = TRUE;
                    pa_sink_input_process_rewind(i, sink_nbytes);
                }
            }

            if (i->detach)
                i->detach(i);

            pa_assert(i->thread_info.attached);
            i->thread_info.attached = FALSE;

            /* Let's remove the sink input ...*/
            if (pa_hashmap_remove(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index)))
                pa_sink_input_unref(i);

            pa_sink_invalidate_requested_latency(s, TRUE);

            pa_log_debug("Requesting rewind due to started move");
            pa_sink_request_rewind(s, (size_t) -1);

            /* In flat volume mode we need to update the volume as
             * well */
            return o->process_msg(o, PA_SINK_MESSAGE_SET_VOLUME, NULL, 0, NULL);
        }

        case PA_SINK_MESSAGE_FINISH_MOVE: {
            pa_sink_input *i = PA_SINK_INPUT(userdata);

            /* We don't support moving synchronized streams. */
            pa_assert(!i->sync_prev);
            pa_assert(!i->sync_next);
            pa_assert(!i->thread_info.sync_next);
            pa_assert(!i->thread_info.sync_prev);

            pa_hashmap_put(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index), pa_sink_input_ref(i));

            pa_assert(!i->thread_info.attached);
            i->thread_info.attached = TRUE;

            if (i->attach)
                i->attach(i);

            if (i->thread_info.requested_sink_latency != (pa_usec_t) -1)
                pa_sink_input_set_requested_latency_within_thread(i, i->thread_info.requested_sink_latency);

            pa_sink_input_update_max_rewind(i, s->thread_info.max_rewind);
            pa_sink_input_update_max_request(i, s->thread_info.max_request);

            if (i->thread_info.state != PA_SINK_INPUT_CORKED) {
                pa_usec_t usec = 0;
                size_t nbytes;

                /* Get the latency of the sink */
                usec = pa_sink_get_latency_within_thread(s);
                nbytes = pa_usec_to_bytes(usec, &s->sample_spec);

                if (nbytes > 0)
                    pa_sink_input_drop(i, nbytes);

                pa_log_debug("Requesting rewind due to finished move");
                pa_sink_request_rewind(s, nbytes);
            }

            /* In flat volume mode we need to update the volume as
             * well */
            return o->process_msg(o, PA_SINK_MESSAGE_SET_VOLUME, NULL, 0, NULL);
        }

        case PA_SINK_MESSAGE_SET_VOLUME:

            if (!pa_cvolume_equal(&s->thread_info.soft_volume, &s->soft_volume)) {
                s->thread_info.soft_volume = s->soft_volume;
                pa_sink_request_rewind(s, (size_t) -1);
            }

            if (!(s->flags & PA_SINK_FLAT_VOLUME))
                return 0;

            /* Fall through ... */

        case PA_SINK_MESSAGE_SYNC_VOLUMES:
            sync_input_volumes_within_thread(s);
            return 0;

        case PA_SINK_MESSAGE_GET_VOLUME:
            return 0;

        case PA_SINK_MESSAGE_SET_MUTE:

            if (s->thread_info.soft_muted != s->muted) {
                s->thread_info.soft_muted = s->muted;
                pa_sink_request_rewind(s, (size_t) -1);
            }

            return 0;

        case PA_SINK_MESSAGE_GET_MUTE:
            return 0;

        case PA_SINK_MESSAGE_SET_STATE: {

            pa_bool_t suspend_change =
                (s->thread_info.state == PA_SINK_SUSPENDED && PA_SINK_IS_OPENED(PA_PTR_TO_UINT(userdata))) ||
                (PA_SINK_IS_OPENED(s->thread_info.state) && PA_PTR_TO_UINT(userdata) == PA_SINK_SUSPENDED);

            s->thread_info.state = PA_PTR_TO_UINT(userdata);

            if (s->thread_info.state == PA_SINK_SUSPENDED) {
                s->thread_info.rewind_nbytes = 0;
                s->thread_info.rewind_requested = FALSE;
            }

            if (suspend_change) {
                pa_sink_input *i;
                void *state = NULL;

                while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)))
                    if (i->suspend_within_thread)
                        i->suspend_within_thread(i, s->thread_info.state == PA_SINK_SUSPENDED);
            }

            return 0;
        }

        case PA_SINK_MESSAGE_DETACH:

            /* Detach all streams */
            pa_sink_detach_within_thread(s);
            return 0;

        case PA_SINK_MESSAGE_ATTACH:

            /* Reattach all streams */
            pa_sink_attach_within_thread(s);
            return 0;

        case PA_SINK_MESSAGE_GET_REQUESTED_LATENCY: {

            pa_usec_t *usec = userdata;
            *usec = pa_sink_get_requested_latency_within_thread(s);

            /* Yes, that's right, the IO thread will see -1 when no
             * explicit requested latency is configured, the main
             * thread will see max_latency */
            if (*usec == (pa_usec_t) -1)
                *usec = s->thread_info.max_latency;

            return 0;
        }

        case PA_SINK_MESSAGE_SET_LATENCY_RANGE: {
            pa_usec_t *r = userdata;

            pa_sink_set_latency_range_within_thread(s, r[0], r[1]);

            return 0;
        }

        case PA_SINK_MESSAGE_GET_LATENCY_RANGE: {
            pa_usec_t *r = userdata;

            r[0] = s->thread_info.min_latency;
            r[1] = s->thread_info.max_latency;

            return 0;
        }

        case PA_SINK_MESSAGE_GET_FIXED_LATENCY:

            *((pa_usec_t*) userdata) = s->thread_info.fixed_latency;
            return 0;

        case PA_SINK_MESSAGE_SET_FIXED_LATENCY:

            pa_sink_set_fixed_latency_within_thread(s, (pa_usec_t) offset);
            return 0;

        case PA_SINK_MESSAGE_GET_MAX_REWIND:

            *((size_t*) userdata) = s->thread_info.max_rewind;
            return 0;

        case PA_SINK_MESSAGE_GET_MAX_REQUEST:

            *((size_t*) userdata) = s->thread_info.max_request;
            return 0;

        case PA_SINK_MESSAGE_SET_MAX_REWIND:

            pa_sink_set_max_rewind_within_thread(s, (size_t) offset);
            return 0;

        case PA_SINK_MESSAGE_SET_MAX_REQUEST:

            pa_sink_set_max_request_within_thread(s, (size_t) offset);
            return 0;

        case PA_SINK_MESSAGE_GET_LATENCY:
        case PA_SINK_MESSAGE_MAX:
            ;
    }

    return -1;
}

/* Called from main thread */
int pa_sink_suspend_all(pa_core *c, pa_bool_t suspend, pa_suspend_cause_t cause) {
    pa_sink *sink;
    uint32_t idx;
    int ret = 0;

    pa_core_assert_ref(c);
    pa_assert_ctl_context();
    pa_assert(cause != 0);

    PA_IDXSET_FOREACH(sink, c->sinks, idx) {
        int r;

        if ((r = pa_sink_suspend(sink, suspend, cause)) < 0)
            ret = r;
    }

    return ret;
}

/* Called from main thread */
void pa_sink_detach(pa_sink *s) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_DETACH, NULL, 0, NULL) == 0);
}

/* Called from main thread */
void pa_sink_attach(pa_sink *s) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_ATTACH, NULL, 0, NULL) == 0);
}

/* Called from IO thread */
void pa_sink_detach_within_thread(pa_sink *s) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));

    PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
        if (i->detach)
            i->detach(i);

    if (s->monitor_source)
        pa_source_detach_within_thread(s->monitor_source);
}

/* Called from IO thread */
void pa_sink_attach_within_thread(pa_sink *s) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));

    PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
        if (i->attach)
            i->attach(i);

    if (s->monitor_source)
        pa_source_attach_within_thread(s->monitor_source);
}

/* Called from IO thread */
void pa_sink_request_rewind(pa_sink*s, size_t nbytes) {
    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);
    pa_assert(PA_SINK_IS_LINKED(s->thread_info.state));

    if (s->thread_info.state == PA_SINK_SUSPENDED)
        return;

    if (nbytes == (size_t) -1)
        nbytes = s->thread_info.max_rewind;

    nbytes = PA_MIN(nbytes, s->thread_info.max_rewind);

    if (s->thread_info.rewind_requested &&
        nbytes <= s->thread_info.rewind_nbytes)
        return;

    s->thread_info.rewind_nbytes = nbytes;
    s->thread_info.rewind_requested = TRUE;

    if (s->request_rewind)
        s->request_rewind(s);
}

/* Called from IO thread */
pa_usec_t pa_sink_get_requested_latency_within_thread(pa_sink *s) {
    pa_usec_t result = (pa_usec_t) -1;
    pa_sink_input *i;
    void *state = NULL;
    pa_usec_t monitor_latency;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    if (!(s->flags & PA_SINK_DYNAMIC_LATENCY))
        return PA_CLAMP(s->thread_info.fixed_latency, s->thread_info.min_latency, s->thread_info.max_latency);

    if (s->thread_info.requested_latency_valid)
        return s->thread_info.requested_latency;

    PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
        if (i->thread_info.requested_sink_latency != (pa_usec_t) -1 &&
            (result == (pa_usec_t) -1 || result > i->thread_info.requested_sink_latency))
            result = i->thread_info.requested_sink_latency;

    monitor_latency = pa_source_get_requested_latency_within_thread(s->monitor_source);

    if (monitor_latency != (pa_usec_t) -1 &&
        (result == (pa_usec_t) -1 || result > monitor_latency))
        result = monitor_latency;

    if (result != (pa_usec_t) -1)
        result = PA_CLAMP(result, s->thread_info.min_latency, s->thread_info.max_latency);

    if (PA_SINK_IS_LINKED(s->thread_info.state)) {
        /* Only cache if properly initialized */
        s->thread_info.requested_latency = result;
        s->thread_info.requested_latency_valid = TRUE;
    }

    return result;
}

/* Called from main thread */
pa_usec_t pa_sink_get_requested_latency(pa_sink *s) {
    pa_usec_t usec = 0;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_IS_LINKED(s->state));

    if (s->state == PA_SINK_SUSPENDED)
        return 0;

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);
    return usec;
}

/* Called from IO as well as the main thread -- the latter only before the IO thread started up */
void pa_sink_set_max_rewind_within_thread(pa_sink *s, size_t max_rewind) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    if (max_rewind == s->thread_info.max_rewind)
        return;

    s->thread_info.max_rewind = max_rewind;

    if (PA_SINK_IS_LINKED(s->thread_info.state))
        PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
            pa_sink_input_update_max_rewind(i, s->thread_info.max_rewind);

    if (s->monitor_source)
        pa_source_set_max_rewind_within_thread(s->monitor_source, s->thread_info.max_rewind);
}

/* Called from main thread */
void pa_sink_set_max_rewind(pa_sink *s, size_t max_rewind) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (PA_SINK_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_MAX_REWIND, NULL, max_rewind, NULL) == 0);
    else
        pa_sink_set_max_rewind_within_thread(s, max_rewind);
}

/* Called from IO as well as the main thread -- the latter only before the IO thread started up */
void pa_sink_set_max_request_within_thread(pa_sink *s, size_t max_request) {
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    if (max_request == s->thread_info.max_request)
        return;

    s->thread_info.max_request = max_request;

    if (PA_SINK_IS_LINKED(s->thread_info.state)) {
        pa_sink_input *i;

        PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
            pa_sink_input_update_max_request(i, s->thread_info.max_request);
    }
}

/* Called from main thread */
void pa_sink_set_max_request(pa_sink *s, size_t max_request) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (PA_SINK_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_MAX_REQUEST, NULL, max_request, NULL) == 0);
    else
        pa_sink_set_max_request_within_thread(s, max_request);
}

/* Called from IO thread */
void pa_sink_invalidate_requested_latency(pa_sink *s, pa_bool_t dynamic) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    if ((s->flags & PA_SINK_DYNAMIC_LATENCY))
        s->thread_info.requested_latency_valid = FALSE;
    else if (dynamic)
        return;

    if (PA_SINK_IS_LINKED(s->thread_info.state)) {

        if (s->update_requested_latency)
            s->update_requested_latency(s);

        PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
            if (i->update_sink_requested_latency)
                i->update_sink_requested_latency(i);
    }
}

/* Called from main thread */
void pa_sink_set_latency_range(pa_sink *s, pa_usec_t min_latency, pa_usec_t max_latency) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    /* min_latency == 0:           no limit
     * min_latency anything else:  specified limit
     *
     * Similar for max_latency */

    if (min_latency < ABSOLUTE_MIN_LATENCY)
        min_latency = ABSOLUTE_MIN_LATENCY;

    if (max_latency <= 0 ||
        max_latency > ABSOLUTE_MAX_LATENCY)
        max_latency = ABSOLUTE_MAX_LATENCY;

    pa_assert(min_latency <= max_latency);

    /* Hmm, let's see if someone forgot to set PA_SINK_DYNAMIC_LATENCY here... */
    pa_assert((min_latency == ABSOLUTE_MIN_LATENCY &&
               max_latency == ABSOLUTE_MAX_LATENCY) ||
              (s->flags & PA_SINK_DYNAMIC_LATENCY));

    if (PA_SINK_IS_LINKED(s->state)) {
        pa_usec_t r[2];

        r[0] = min_latency;
        r[1] = max_latency;

        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_LATENCY_RANGE, r, 0, NULL) == 0);
    } else
        pa_sink_set_latency_range_within_thread(s, min_latency, max_latency);
}

/* Called from main thread */
void pa_sink_get_latency_range(pa_sink *s, pa_usec_t *min_latency, pa_usec_t *max_latency) {
   pa_sink_assert_ref(s);
   pa_assert_ctl_context();
   pa_assert(min_latency);
   pa_assert(max_latency);

   if (PA_SINK_IS_LINKED(s->state)) {
       pa_usec_t r[2] = { 0, 0 };

       pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_LATENCY_RANGE, r, 0, NULL) == 0);

       *min_latency = r[0];
       *max_latency = r[1];
   } else {
       *min_latency = s->thread_info.min_latency;
       *max_latency = s->thread_info.max_latency;
   }
}

/* Called from IO thread */
void pa_sink_set_latency_range_within_thread(pa_sink *s, pa_usec_t min_latency, pa_usec_t max_latency) {
    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    pa_assert(min_latency >= ABSOLUTE_MIN_LATENCY);
    pa_assert(max_latency <= ABSOLUTE_MAX_LATENCY);
    pa_assert(min_latency <= max_latency);

    /* Hmm, let's see if someone forgot to set PA_SINK_DYNAMIC_LATENCY here... */
    pa_assert((min_latency == ABSOLUTE_MIN_LATENCY &&
               max_latency == ABSOLUTE_MAX_LATENCY) ||
              (s->flags & PA_SINK_DYNAMIC_LATENCY));

    if (s->thread_info.min_latency == min_latency &&
        s->thread_info.max_latency == max_latency)
        return;

    s->thread_info.min_latency = min_latency;
    s->thread_info.max_latency = max_latency;

    if (PA_SINK_IS_LINKED(s->thread_info.state)) {
        pa_sink_input *i;
        void *state = NULL;

        PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
            if (i->update_sink_latency_range)
                i->update_sink_latency_range(i);
    }

    pa_sink_invalidate_requested_latency(s, FALSE);

    pa_source_set_latency_range_within_thread(s->monitor_source, min_latency, max_latency);
}

/* Called from main thread */
void pa_sink_set_fixed_latency(pa_sink *s, pa_usec_t latency) {
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (s->flags & PA_SINK_DYNAMIC_LATENCY) {
        pa_assert(latency == 0);
        return;
    }

    if (latency < ABSOLUTE_MIN_LATENCY)
        latency = ABSOLUTE_MIN_LATENCY;

    if (latency > ABSOLUTE_MAX_LATENCY)
        latency = ABSOLUTE_MAX_LATENCY;

    if (PA_SINK_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_FIXED_LATENCY, NULL, (int64_t) latency, NULL) == 0);
    else
        s->thread_info.fixed_latency = latency;

    pa_source_set_fixed_latency(s->monitor_source, latency);
}

/* Called from main thread */
pa_usec_t pa_sink_get_fixed_latency(pa_sink *s) {
    pa_usec_t latency;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (s->flags & PA_SINK_DYNAMIC_LATENCY)
        return 0;

    if (PA_SINK_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_FIXED_LATENCY, &latency, 0, NULL) == 0);
    else
        latency = s->thread_info.fixed_latency;

    return latency;
}

/* Called from IO thread */
void pa_sink_set_fixed_latency_within_thread(pa_sink *s, pa_usec_t latency) {
    pa_sink_assert_ref(s);
    pa_sink_assert_io_context(s);

    if (s->flags & PA_SINK_DYNAMIC_LATENCY) {
        pa_assert(latency == 0);
        return;
    }

    pa_assert(latency >= ABSOLUTE_MIN_LATENCY);
    pa_assert(latency <= ABSOLUTE_MAX_LATENCY);

    if (s->thread_info.fixed_latency == latency)
        return;

    s->thread_info.fixed_latency = latency;

    if (PA_SINK_IS_LINKED(s->thread_info.state)) {
        pa_sink_input *i;
        void *state = NULL;

        PA_HASHMAP_FOREACH(i, s->thread_info.inputs, state)
            if (i->update_sink_fixed_latency)
                i->update_sink_fixed_latency(i);
    }

    pa_sink_invalidate_requested_latency(s, FALSE);

    pa_source_set_fixed_latency_within_thread(s->monitor_source, latency);
}

/* Called from main context */
size_t pa_sink_get_max_rewind(pa_sink *s) {
    size_t r;
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (!PA_SINK_IS_LINKED(s->state))
        return s->thread_info.max_rewind;

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_MAX_REWIND, &r, 0, NULL) == 0);

    return r;
}

/* Called from main context */
size_t pa_sink_get_max_request(pa_sink *s) {
    size_t r;
    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (!PA_SINK_IS_LINKED(s->state))
        return s->thread_info.max_request;

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_MAX_REQUEST, &r, 0, NULL) == 0);

    return r;
}

/* Called from main context */
int pa_sink_set_port(pa_sink *s, const char *name, pa_bool_t save) {
    pa_device_port *port;

    pa_sink_assert_ref(s);
    pa_assert_ctl_context();

    if (!s->set_port) {
        pa_log_debug("set_port() operation not implemented for sink %u \"%s\"", s->index, s->name);
        return -PA_ERR_NOTIMPLEMENTED;
    }

    if (!s->ports)
        return -PA_ERR_NOENTITY;

    if (!(port = pa_hashmap_get(s->ports, name)))
        return -PA_ERR_NOENTITY;

    if (s->active_port == port) {
        s->save_port = s->save_port || save;
        return 0;
    }

    if ((s->set_port(s, port)) < 0)
        return -PA_ERR_NOENTITY;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

    pa_log_info("Changed port of sink %u \"%s\" to %s", s->index, s->name, port->name);

    s->active_port = port;
    s->save_port = save;

    return 0;
}

pa_bool_t pa_device_init_icon(pa_proplist *p, pa_bool_t is_sink) {
    const char *ff, *c, *t = NULL, *s = "", *profile, *bus;

    pa_assert(p);

    if (pa_proplist_contains(p, PA_PROP_DEVICE_ICON_NAME))
        return TRUE;

    if ((ff = pa_proplist_gets(p, PA_PROP_DEVICE_FORM_FACTOR))) {

        if (pa_streq(ff, "microphone"))
            t = "audio-input-microphone";
        else if (pa_streq(ff, "webcam"))
            t = "camera-web";
        else if (pa_streq(ff, "computer"))
            t = "computer";
        else if (pa_streq(ff, "handset"))
            t = "phone";
        else if (pa_streq(ff, "portable"))
            t = "multimedia-player";
        else if (pa_streq(ff, "tv"))
            t = "video-display";

        /*
         * The following icons are not part of the icon naming spec,
         * because Rodney Dawes sucks as the maintainer of that spec.
         *
         * http://lists.freedesktop.org/archives/xdg/2009-May/010397.html
         */
        else if (pa_streq(ff, "headset"))
            t = "audio-headset";
        else if (pa_streq(ff, "headphone"))
            t = "audio-headphones";
        else if (pa_streq(ff, "speaker"))
            t = "audio-speakers";
        else if (pa_streq(ff, "hands-free"))
            t = "audio-handsfree";
    }

    if (!t)
        if ((c = pa_proplist_gets(p, PA_PROP_DEVICE_CLASS)))
            if (pa_streq(c, "modem"))
                t = "modem";

    if (!t) {
        if (is_sink)
            t = "audio-card";
        else
            t = "audio-input-microphone";
    }

    if ((profile = pa_proplist_gets(p, PA_PROP_DEVICE_PROFILE_NAME))) {
        if (strstr(profile, "analog"))
            s = "-analog";
        else if (strstr(profile, "iec958"))
            s = "-iec958";
        else if (strstr(profile, "hdmi"))
            s = "-hdmi";
    }

    bus = pa_proplist_gets(p, PA_PROP_DEVICE_BUS);

    pa_proplist_setf(p, PA_PROP_DEVICE_ICON_NAME, "%s%s%s%s", t, pa_strempty(s), bus ? "-" : "", pa_strempty(bus));

    return TRUE;
}

pa_bool_t pa_device_init_description(pa_proplist *p) {
    const char *s, *d = NULL, *k;
    pa_assert(p);

    if (pa_proplist_contains(p, PA_PROP_DEVICE_DESCRIPTION))
        return TRUE;

    if ((s = pa_proplist_gets(p, PA_PROP_DEVICE_FORM_FACTOR)))
        if (pa_streq(s, "internal"))
            d = _("Internal Audio");

    if (!d)
        if ((s = pa_proplist_gets(p, PA_PROP_DEVICE_CLASS)))
            if (pa_streq(s, "modem"))
                d = _("Modem");

    if (!d)
        d = pa_proplist_gets(p, PA_PROP_DEVICE_PRODUCT_NAME);

    if (!d)
        return FALSE;

    k = pa_proplist_gets(p, PA_PROP_DEVICE_PROFILE_DESCRIPTION);

    if (d && k)
        pa_proplist_setf(p, PA_PROP_DEVICE_DESCRIPTION, _("%s %s"), d, k);
    else if (d)
        pa_proplist_sets(p, PA_PROP_DEVICE_DESCRIPTION, d);

    return TRUE;
}

pa_bool_t pa_device_init_intended_roles(pa_proplist *p) {
    const char *s;
    pa_assert(p);

    if (pa_proplist_contains(p, PA_PROP_DEVICE_INTENDED_ROLES))
        return TRUE;

    if ((s = pa_proplist_gets(p, PA_PROP_DEVICE_FORM_FACTOR)))
        if (pa_streq(s, "handset") || pa_streq(s, "hands-free")) {
            pa_proplist_sets(p, PA_PROP_DEVICE_INTENDED_ROLES, "phone");
            return TRUE;
        }

    return FALSE;
}

unsigned pa_device_init_priority(pa_proplist *p) {
    const char *s;
    unsigned priority = 0;

    pa_assert(p);

    if ((s = pa_proplist_gets(p, PA_PROP_DEVICE_CLASS))) {

        if (pa_streq(s, "sound"))
            priority += 9000;
        else if (!pa_streq(s, "modem"))
            priority += 1000;
    }

    if ((s = pa_proplist_gets(p, PA_PROP_DEVICE_FORM_FACTOR))) {

        if (pa_streq(s, "internal"))
            priority += 900;
        else if (pa_streq(s, "speaker"))
            priority += 500;
        else if (pa_streq(s, "headphone"))
            priority += 400;
    }

    if ((s = pa_proplist_gets(p, PA_PROP_DEVICE_BUS))) {

        if (pa_streq(s, "pci"))
            priority += 50;
        else if (pa_streq(s, "usb"))
            priority += 40;
        else if (pa_streq(s, "bluetooth"))
            priority += 30;
    }

    if ((s = pa_proplist_gets(p, PA_PROP_DEVICE_PROFILE_NAME))) {

        if (pa_startswith(s, "analog-"))
            priority += 9;
        else if (pa_startswith(s, "iec958-"))
            priority += 8;
    }

    return priority;
}
