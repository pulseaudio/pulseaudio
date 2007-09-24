/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>

#include "source-output.h"

static PA_DEFINE_CHECK_TYPE(pa_source_output, pa_msgobject);

static void source_output_free(pa_object* mo);

pa_source_output_new_data* pa_source_output_new_data_init(pa_source_output_new_data *data) {
    pa_assert(data);

    memset(data, 0, sizeof(*data));
    data->resample_method = PA_RESAMPLER_INVALID;
    return data;
}

void pa_source_output_new_data_set_channel_map(pa_source_output_new_data *data, const pa_channel_map *map) {
    pa_assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_source_output_new_data_set_sample_spec(pa_source_output_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

pa_source_output* pa_source_output_new(
        pa_core *core,
        pa_source_output_new_data *data,
        pa_source_output_flags_t flags) {

    pa_source_output *o;
    pa_resampler *resampler = NULL;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX];

    pa_assert(core);
    pa_assert(data);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], data) < 0)
        return NULL;

    pa_return_null_if_fail(!data->driver || pa_utf8_valid(data->driver));
    pa_return_null_if_fail(!data->name || pa_utf8_valid(data->name));

    if (!data->source)
        data->source = pa_namereg_get(core, NULL, PA_NAMEREG_SOURCE, 1);

    pa_return_null_if_fail(data->source);
    pa_return_null_if_fail(pa_source_get_state(data->source) != PA_SOURCE_UNLINKED);

    if (!data->sample_spec_is_set)
        data->sample_spec = data->source->sample_spec;

    pa_return_null_if_fail(pa_sample_spec_valid(&data->sample_spec));

    if (!data->channel_map_is_set) {
        if (data->source->channel_map.channels == data->sample_spec.channels)
            data->channel_map = data->source->channel_map;
        else
            pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
    }

    pa_return_null_if_fail(pa_channel_map_valid(&data->channel_map));
    pa_return_null_if_fail(data->channel_map.channels == data->sample_spec.channels);

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    pa_return_null_if_fail(data->resample_method < PA_RESAMPLER_MAX);

    if (pa_idxset_size(data->source->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log("Failed to create source output: too many outputs per source.");
        return NULL;
    }

    if ((flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ||
        !pa_sample_spec_equal(&data->sample_spec, &data->source->sample_spec) ||
        !pa_channel_map_equal(&data->channel_map, &data->source->channel_map)) {

        if (!(resampler = pa_resampler_new(
                      core->mempool,
                      &data->source->sample_spec, &data->source->channel_map,
                      &data->sample_spec, &data->channel_map,
                      data->resample_method,
                      !!(flags & PA_SOURCE_OUTPUT_VARIABLE_RATE)))) {
            pa_log_warn("Unsupported resampling operation.");
            return NULL;
        }

        data->resample_method = pa_resampler_get_method(resampler);
    }

    o = pa_msgobject_new(pa_source_output);
    o->parent.parent.free = source_output_free;
    o->parent.process_msg = pa_source_output_process_msg;

    o->core = core;
    o->state = PA_SOURCE_OUTPUT_INIT;
    o->flags = flags;
    o->name = pa_xstrdup(data->name);
    o->driver = pa_xstrdup(data->driver);
    o->module = data->module;
    o->source = data->source;
    o->client = data->client;

    o->resample_method = data->resample_method;
    o->sample_spec = data->sample_spec;
    o->channel_map = data->channel_map;

    o->push = NULL;
    o->kill = NULL;
    o->get_latency = NULL;
    o->detach = NULL;
    o->attach = NULL;
    o->suspend = NULL;
    o->userdata = NULL;

    o->thread_info.state = o->state;
    o->thread_info.attached = FALSE;
    o->thread_info.sample_spec = o->sample_spec;
    o->thread_info.resampler = resampler;

    pa_assert_se(pa_idxset_put(core->source_outputs, o, &o->index) == 0);
    pa_assert_se(pa_idxset_put(o->source->outputs, pa_source_output_ref(o), NULL) == 0);

    pa_log_info("Created output %u \"%s\" on %s with sample spec %s",
                o->index,
                o->name,
                o->source->name,
                pa_sample_spec_snprint(st, sizeof(st), &o->sample_spec));

    /* Don't forget to call pa_source_output_put! */

    return o;
}

static int source_output_set_state(pa_source_output *o, pa_source_output_state_t state) {
    pa_assert(o);

    if (o->state == state)
        return 0;

    if (pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL) < 0)
        return -1;

    if (o->state == PA_SOURCE_OUTPUT_CORKED && state != PA_SOURCE_OUTPUT_CORKED)
        pa_assert_se(o->source->n_corked -- >= 1);
    else if (o->state != PA_SOURCE_OUTPUT_CORKED && state == PA_SOURCE_OUTPUT_CORKED)
        o->source->n_corked++;

    pa_source_update_status(o->source);

    o->state = state;

    if (state != PA_SOURCE_OUTPUT_UNLINKED)
        pa_hook_fire(&o->source->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED], o);

    return 0;
}

void pa_source_output_unlink(pa_source_output*o) {
    pa_bool_t linked;
    pa_assert(o);

    /* See pa_sink_unlink() for a couple of comments how this function
     * works */

    pa_source_output_ref(o);

    linked = PA_SOURCE_OUTPUT_LINKED(o->state);

    if (linked)
        pa_hook_fire(&o->source->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK], o);

    pa_idxset_remove_by_data(o->source->core->source_outputs, o, NULL);
    if (pa_idxset_remove_by_data(o->source->outputs, o, NULL))
        pa_source_output_unref(o);

    if (linked) {
        pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_REMOVE_OUTPUT, o, 0, NULL);
        source_output_set_state(o, PA_SOURCE_OUTPUT_UNLINKED);
        pa_source_update_status(o->source);
    } else
        o->state = PA_SOURCE_OUTPUT_UNLINKED;

    o->push = NULL;
    o->kill = NULL;
    o->get_latency = NULL;
    o->attach = NULL;
    o->detach = NULL;
    o->suspend = NULL;

    if (linked) {
        pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_REMOVE, o->index);
        pa_hook_fire(&o->source->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST], o);
    }

    o->source = NULL;
    pa_source_output_unref(o);
}

static void source_output_free(pa_object* mo) {
    pa_source_output *o = PA_SOURCE_OUTPUT(mo);

    pa_assert(pa_source_output_refcnt(o) == 0);

    if (PA_SOURCE_OUTPUT_LINKED(o->state))
        pa_source_output_unlink(o);

    pa_log_info("Freeing output %u \"%s\"", o->index, o->name);

    pa_assert(!o->thread_info.attached);

    if (o->thread_info.resampler)
        pa_resampler_free(o->thread_info.resampler);

    pa_xfree(o->name);
    pa_xfree(o->driver);
    pa_xfree(o);
}

void pa_source_output_put(pa_source_output *o) {
    pa_source_output_assert_ref(o);

    pa_assert(o->state == PA_SOURCE_OUTPUT_INIT);
    pa_assert(o->push);

    o->thread_info.state = o->state = o->flags & PA_SOURCE_OUTPUT_START_CORKED ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING;

    if (o->state == PA_SOURCE_OUTPUT_CORKED)
        o->source->n_corked++;

    pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_ADD_OUTPUT, o, 0, NULL);
    pa_source_update_status(o->source);

    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW, o->index);

    pa_hook_fire(&o->source->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT], o);
}

void pa_source_output_kill(pa_source_output*o) {
    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_LINKED(o->state));

    if (o->kill)
        o->kill(o);
}

pa_usec_t pa_source_output_get_latency(pa_source_output *o) {
    pa_usec_t r = 0;

    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_LINKED(o->state));

    if (pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY, &r, 0, NULL) < 0)
        r = 0;

    if (o->get_latency)
        r += o->get_latency(o);

    return r;
}

/* Called from thread context */
void pa_source_output_push(pa_source_output *o, const pa_memchunk *chunk) {
    pa_memchunk rchunk;

    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_LINKED(o->thread_info.state));
    pa_assert(chunk);
    pa_assert(chunk->length);

    if (!o->push || o->state == PA_SOURCE_OUTPUT_CORKED)
        return;

    pa_assert(o->state == PA_SOURCE_OUTPUT_RUNNING);

    if (!o->thread_info.resampler) {
        o->push(o, chunk);
        return;
    }

    pa_resampler_run(o->thread_info.resampler, chunk, &rchunk);
    if (!rchunk.length)
        return;

    pa_assert(rchunk.memblock);
    o->push(o, &rchunk);
    pa_memblock_unref(rchunk.memblock);
}

void pa_source_output_cork(pa_source_output *o, pa_bool_t b) {
    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_LINKED(o->state));

    source_output_set_state(o, b ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING);
}

int pa_source_output_set_rate(pa_source_output *o, uint32_t rate) {
    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_LINKED(o->state));
    pa_return_val_if_fail(o->thread_info.resampler, -1);

    if (o->sample_spec.rate == rate)
        return 0;

    o->sample_spec.rate = rate;

    pa_asyncmsgq_post(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_RATE, PA_UINT_TO_PTR(rate), 0, NULL, NULL);

    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    return 0;
}

void pa_source_output_set_name(pa_source_output *o, const char *name) {
    pa_source_output_assert_ref(o);

    if (!o->name && !name)
        return;

    if (o->name && name && !strcmp(o->name, name))
        return;

    pa_xfree(o->name);
    o->name = pa_xstrdup(name);

    if (PA_SOURCE_OUTPUT_LINKED(o->state)) {
        pa_hook_fire(&o->source->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NAME_CHANGED], o);
        pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    }
}

pa_resample_method_t pa_source_output_get_resample_method(pa_source_output *o) {
    pa_source_output_assert_ref(o);

    return o->resample_method;
}

int pa_source_output_move_to(pa_source_output *o, pa_source *dest) {
    pa_source *origin;
    pa_resampler *new_resampler = NULL;

    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_LINKED(o->state));
    pa_source_assert_ref(dest);

    origin = o->source;

    if (dest == origin)
        return 0;

    if (o->flags & PA_SOURCE_OUTPUT_DONT_MOVE)
        return -1;

    if (pa_idxset_size(dest->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log_warn("Failed to move source output: too many outputs per source.");
        return -1;
    }

    if (o->thread_info.resampler &&
        pa_sample_spec_equal(&origin->sample_spec, &dest->sample_spec) &&
        pa_channel_map_equal(&origin->channel_map, &dest->channel_map))

        /* Try to reuse the old resampler if possible */
        new_resampler = o->thread_info.resampler;

    else if ((o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ||
             !pa_sample_spec_equal(&o->sample_spec, &dest->sample_spec) ||
             !pa_channel_map_equal(&o->channel_map, &dest->channel_map)) {

        /* Okey, we need a new resampler for the new source */

        if (!(new_resampler = pa_resampler_new(
                      dest->core->mempool,
                      &dest->sample_spec, &dest->channel_map,
                      &o->sample_spec, &o->channel_map,
                      o->resample_method,
                      !!(o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE)))) {
            pa_log_warn("Unsupported resampling operation.");
            return -1;
        }
    }

    pa_hook_fire(&o->source->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE], o);

    /* Okey, let's move it */
    pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_REMOVE_OUTPUT, o, 0, NULL);

    pa_idxset_remove_by_data(origin->outputs, o, NULL);
    pa_idxset_put(dest->outputs, o, NULL);
    o->source = dest;

    if (pa_source_output_get_state(o) == PA_SOURCE_OUTPUT_CORKED) {
        pa_assert_se(origin->n_corked-- >= 1);
        dest->n_corked++;
    }

    /* Replace resampler */
    if (new_resampler != o->thread_info.resampler) {
        if (o->thread_info.resampler)
            pa_resampler_free(o->thread_info.resampler);
        o->thread_info.resampler = new_resampler;
    }

    pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_ADD_OUTPUT, o, 0, NULL);

    pa_source_update_status(origin);
    pa_source_update_status(dest);

    pa_hook_fire(&o->source->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_POST], o);

    pa_log_debug("Successfully moved source output %i from %s to %s.", o->index, origin->name, dest->name);

    /* Notify everyone */
    pa_subscription_post(o->source->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);

    return 0;
}

/* Called from thread context */
int pa_source_output_process_msg(pa_msgobject *mo, int code, void *userdata, int64_t offset, pa_memchunk* chunk) {
    pa_source_output *o = PA_SOURCE_OUTPUT(mo);

    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_LINKED(o->thread_info.state));

    switch (code) {

        case PA_SOURCE_OUTPUT_MESSAGE_SET_RATE: {

            o->thread_info.sample_spec.rate = PA_PTR_TO_UINT(userdata);
            pa_resampler_set_output_rate(o->thread_info.resampler, PA_PTR_TO_UINT(userdata));

            return 0;
        }

        case PA_SOURCE_OUTPUT_MESSAGE_SET_STATE: {
            o->thread_info.state = PA_PTR_TO_UINT(userdata);

            return 0;
        }
    }

    return -1;
}
