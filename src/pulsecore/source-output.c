/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/sample-util.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "source-output.h"

#define MEMBLOCKQ_MAXLENGTH (32*1024*1024)

PA_DEFINE_PUBLIC_CLASS(pa_source_output, pa_msgobject);

static void source_output_free(pa_object* mo);

pa_source_output_new_data* pa_source_output_new_data_init(pa_source_output_new_data *data) {
    pa_assert(data);

    pa_zero(*data);
    data->resample_method = PA_RESAMPLER_INVALID;
    data->proplist = pa_proplist_new();

    return data;
}

void pa_source_output_new_data_set_sample_spec(pa_source_output_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

void pa_source_output_new_data_set_channel_map(pa_source_output_new_data *data, const pa_channel_map *map) {
    pa_assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_source_output_new_data_done(pa_source_output_new_data *data) {
    pa_assert(data);

    pa_proplist_free(data->proplist);
}

/* Called from main context */
static void reset_callbacks(pa_source_output *o) {
    pa_assert(o);

    o->push = NULL;
    o->process_rewind = NULL;
    o->update_max_rewind = NULL;
    o->update_source_requested_latency = NULL;
    o->update_source_latency_range = NULL;
    o->update_source_fixed_latency = NULL;
    o->attach = NULL;
    o->detach = NULL;
    o->suspend = NULL;
    o->suspend_within_thread = NULL;
    o->moving = NULL;
    o->kill = NULL;
    o->get_latency = NULL;
    o->state_change = NULL;
    o->may_move_to = NULL;
    o->send_event = NULL;
}

/* Called from main context */
int pa_source_output_new(
        pa_source_output**_o,
        pa_core *core,
        pa_source_output_new_data *data) {

    pa_source_output *o;
    pa_resampler *resampler = NULL;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    int r;
    char *pt;

    pa_assert(_o);
    pa_assert(core);
    pa_assert(data);
    pa_assert_ctl_context();

    if (data->client)
        pa_proplist_update(data->proplist, PA_UPDATE_MERGE, data->client->proplist);

    if ((r = pa_hook_fire(&core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], data)) < 0)
        return r;

    pa_return_val_if_fail(!data->driver || pa_utf8_valid(data->driver), -PA_ERR_INVALID);

    if (!data->source) {
        data->source = pa_namereg_get(core, NULL, PA_NAMEREG_SOURCE);
        data->save_source = FALSE;
    }

    pa_return_val_if_fail(data->source, -PA_ERR_NOENTITY);
    pa_return_val_if_fail(PA_SOURCE_IS_LINKED(pa_source_get_state(data->source)), -PA_ERR_BADSTATE);
    pa_return_val_if_fail(!data->direct_on_input || data->direct_on_input->sink == data->source->monitor_of, -PA_ERR_INVALID);

    if (!data->sample_spec_is_set)
        data->sample_spec = data->source->sample_spec;

    pa_return_val_if_fail(pa_sample_spec_valid(&data->sample_spec), -PA_ERR_INVALID);

    if (!data->channel_map_is_set) {
        if (pa_channel_map_compatible(&data->source->channel_map, &data->sample_spec))
            data->channel_map = data->source->channel_map;
        else
            pa_channel_map_init_extend(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
    }

    pa_return_val_if_fail(pa_channel_map_valid(&data->channel_map), -PA_ERR_INVALID);
    pa_return_val_if_fail(pa_channel_map_compatible(&data->channel_map, &data->sample_spec), -PA_ERR_INVALID);

    if (data->flags & PA_SOURCE_OUTPUT_FIX_FORMAT)
        data->sample_spec.format = data->source->sample_spec.format;

    if (data->flags & PA_SOURCE_OUTPUT_FIX_RATE)
        data->sample_spec.rate = data->source->sample_spec.rate;

    if (data->flags & PA_SOURCE_OUTPUT_FIX_CHANNELS) {
        data->sample_spec.channels = data->source->sample_spec.channels;
        data->channel_map = data->source->channel_map;
    }

    pa_assert(pa_sample_spec_valid(&data->sample_spec));
    pa_assert(pa_channel_map_valid(&data->channel_map));

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    pa_return_val_if_fail(data->resample_method < PA_RESAMPLER_MAX, -PA_ERR_INVALID);

    if ((r = pa_hook_fire(&core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_FIXATE], data)) < 0)
        return r;

    if ((data->flags & PA_SOURCE_OUTPUT_NO_CREATE_ON_SUSPEND) &&
        pa_source_get_state(data->source) == PA_SOURCE_SUSPENDED) {
        pa_log("Failed to create source output: source is suspended.");
        return -PA_ERR_BADSTATE;
    }

    if (pa_idxset_size(data->source->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log("Failed to create source output: too many outputs per source.");
        return -PA_ERR_TOOLARGE;
    }

    if ((data->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ||
        !pa_sample_spec_equal(&data->sample_spec, &data->source->sample_spec) ||
        !pa_channel_map_equal(&data->channel_map, &data->source->channel_map)) {

        if (!(resampler = pa_resampler_new(
                      core->mempool,
                      &data->source->sample_spec, &data->source->channel_map,
                      &data->sample_spec, &data->channel_map,
                      data->resample_method,
                      ((data->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                      ((data->flags & PA_SOURCE_OUTPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                      (core->disable_remixing || (data->flags & PA_SOURCE_OUTPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0) |
                      (core->disable_lfe_remixing ? PA_RESAMPLER_NO_LFE : 0)))) {
            pa_log_warn("Unsupported resampling operation.");
            return -PA_ERR_NOTSUPPORTED;
        }
    }

    o = pa_msgobject_new(pa_source_output);
    o->parent.parent.free = source_output_free;
    o->parent.process_msg = pa_source_output_process_msg;

    o->core = core;
    o->state = PA_SOURCE_OUTPUT_INIT;
    o->flags = data->flags;
    o->proplist = pa_proplist_copy(data->proplist);
    o->driver = pa_xstrdup(pa_path_get_filename(data->driver));
    o->module = data->module;
    o->source = data->source;
    o->client = data->client;

    o->actual_resample_method = resampler ? pa_resampler_get_method(resampler) : PA_RESAMPLER_INVALID;
    o->requested_resample_method = data->resample_method;
    o->sample_spec = data->sample_spec;
    o->channel_map = data->channel_map;

    o->direct_on_input = data->direct_on_input;

    o->save_source = data->save_source;

    reset_callbacks(o);
    o->userdata = NULL;

    o->thread_info.state = o->state;
    o->thread_info.attached = FALSE;
    o->thread_info.sample_spec = o->sample_spec;
    o->thread_info.resampler = resampler;
    o->thread_info.requested_source_latency = (pa_usec_t) -1;
    o->thread_info.direct_on_input = o->direct_on_input;

    o->thread_info.delay_memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            0,
            pa_frame_size(&o->source->sample_spec),
            0,
            1,
            0,
            &o->source->silence);

    pa_assert_se(pa_idxset_put(core->source_outputs, o, &o->index) == 0);
    pa_assert_se(pa_idxset_put(o->source->outputs, pa_source_output_ref(o), NULL) == 0);

    if (o->client)
        pa_assert_se(pa_idxset_put(o->client->source_outputs, o, NULL) >= 0);

    if (o->direct_on_input)
        pa_assert_se(pa_idxset_put(o->direct_on_input->direct_outputs, o, NULL) == 0);

    pt = pa_proplist_to_string_sep(o->proplist, "\n    ");
    pa_log_info("Created output %u \"%s\" on %s with sample spec %s and channel map %s\n    %s",
                o->index,
                pa_strnull(pa_proplist_gets(o->proplist, PA_PROP_MEDIA_NAME)),
                o->source->name,
                pa_sample_spec_snprint(st, sizeof(st), &o->sample_spec),
                pa_channel_map_snprint(cm, sizeof(cm), &o->channel_map),
                pt);
    pa_xfree(pt);

    /* Don't forget to call pa_source_output_put! */

    *_o = o;
    return 0;
}

/* Called from main context */
static void update_n_corked(pa_source_output *o, pa_source_output_state_t state) {
    pa_assert(o);
    pa_assert_ctl_context();

    if (!o->source)
        return;

    if (o->state == PA_SOURCE_OUTPUT_CORKED && state != PA_SOURCE_OUTPUT_CORKED)
        pa_assert_se(o->source->n_corked -- >= 1);
    else if (o->state != PA_SOURCE_OUTPUT_CORKED && state == PA_SOURCE_OUTPUT_CORKED)
        o->source->n_corked++;
}

/* Called from main context */
static void source_output_set_state(pa_source_output *o, pa_source_output_state_t state) {
    pa_assert(o);
    pa_assert_ctl_context();

    if (o->state == state)
        return;

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL) == 0);

    update_n_corked(o, state);
    o->state = state;

    if (state != PA_SOURCE_OUTPUT_UNLINKED)
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED], o);

    pa_source_update_status(o->source);
}

/* Called from main context */
void pa_source_output_unlink(pa_source_output*o) {
    pa_bool_t linked;
    pa_assert(o);
    pa_assert_ctl_context();

    /* See pa_sink_unlink() for a couple of comments how this function
     * works */

    pa_source_output_ref(o);

    linked = PA_SOURCE_OUTPUT_IS_LINKED(o->state);

    if (linked)
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK], o);

    if (o->direct_on_input)
        pa_idxset_remove_by_data(o->direct_on_input->direct_outputs, o, NULL);

    pa_idxset_remove_by_data(o->core->source_outputs, o, NULL);

    if (o->source)
        if (pa_idxset_remove_by_data(o->source->outputs, o, NULL))
            pa_source_output_unref(o);

    if (o->client)
        pa_idxset_remove_by_data(o->client->source_outputs, o, NULL);

    update_n_corked(o, PA_SOURCE_OUTPUT_UNLINKED);
    o->state = PA_SOURCE_OUTPUT_UNLINKED;

    if (linked && o->source)
        if (o->source->asyncmsgq)
            pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_REMOVE_OUTPUT, o, 0, NULL) == 0);

    reset_callbacks(o);

    if (linked) {
        pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_REMOVE, o->index);
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST], o);
    }

    if (o->source) {
        pa_source_update_status(o->source);
        o->source = NULL;
    }

    pa_core_maybe_vacuum(o->core);

    pa_source_output_unref(o);
}

/* Called from main context */
static void source_output_free(pa_object* mo) {
    pa_source_output *o = PA_SOURCE_OUTPUT(mo);

    pa_assert(o);
    pa_assert_ctl_context();
    pa_assert(pa_source_output_refcnt(o) == 0);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state))
        pa_source_output_unlink(o);

    pa_log_info("Freeing output %u \"%s\"", o->index, pa_strnull(pa_proplist_gets(o->proplist, PA_PROP_MEDIA_NAME)));

    if (o->thread_info.delay_memblockq)
        pa_memblockq_free(o->thread_info.delay_memblockq);

    if (o->thread_info.resampler)
        pa_resampler_free(o->thread_info.resampler);

    if (o->proplist)
        pa_proplist_free(o->proplist);

    pa_xfree(o->driver);
    pa_xfree(o);
}

/* Called from main context */
void pa_source_output_put(pa_source_output *o) {
    pa_source_output_state_t state;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    pa_assert(o->state == PA_SOURCE_OUTPUT_INIT);

    /* The following fields must be initialized properly */
    pa_assert(o->push);
    pa_assert(o->kill);

    state = o->flags & PA_SOURCE_OUTPUT_START_CORKED ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING;

    update_n_corked(o, state);
    o->state = state;

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_ADD_OUTPUT, o, 0, NULL) == 0);

    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW, o->index);
    pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT], o);

    pa_source_update_status(o->source);
}

/* Called from main context */
void pa_source_output_kill(pa_source_output*o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    o->kill(o);
}

/* Called from main context */
pa_usec_t pa_source_output_get_latency(pa_source_output *o, pa_usec_t *source_latency) {
    pa_usec_t r[2] = { 0, 0 };

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY, r, 0, NULL) == 0);

    if (o->get_latency)
        r[0] += o->get_latency(o);

    if (source_latency)
        *source_latency = r[1];

    return r[0];
}

/* Called from thread context */
void pa_source_output_push(pa_source_output *o, const pa_memchunk *chunk) {
    size_t length;
    size_t limit, mbs = 0;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state));
    pa_assert(chunk);
    pa_assert(pa_frame_aligned(chunk->length, &o->source->sample_spec));

    if (!o->push || o->thread_info.state == PA_SOURCE_OUTPUT_CORKED)
        return;

    pa_assert(o->thread_info.state == PA_SOURCE_OUTPUT_RUNNING);

    if (pa_memblockq_push(o->thread_info.delay_memblockq, chunk) < 0) {
        pa_log_debug("Delay queue overflow!");
        pa_memblockq_seek(o->thread_info.delay_memblockq, (int64_t) chunk->length, PA_SEEK_RELATIVE, TRUE);
    }

    limit = o->process_rewind ? 0 : o->source->thread_info.max_rewind;

    if (limit > 0 && o->source->monitor_of) {
        pa_usec_t latency;
        size_t n;

        /* Hmm, check the latency for knowing how much of the buffered
         * data is actually still unplayed and might hence still
         * change. This is suboptimal. Ideally we'd have a call like
         * pa_sink_get_changeable_size() or so that tells us how much
         * of the queued data is actually still changeable. Hence
         * FIXME! */

        latency = pa_sink_get_latency_within_thread(o->source->monitor_of);

        n = pa_usec_to_bytes(latency, &o->source->sample_spec);

        if (n < limit)
            limit = n;
    }

    /* Implement the delay queue */
    while ((length = pa_memblockq_get_length(o->thread_info.delay_memblockq)) > limit) {
        pa_memchunk qchunk;

        length -= limit;

        pa_assert_se(pa_memblockq_peek(o->thread_info.delay_memblockq, &qchunk) >= 0);

        if (qchunk.length > length)
            qchunk.length = length;

        pa_assert(qchunk.length > 0);

        if (!o->thread_info.resampler)
            o->push(o, &qchunk);
        else {
            pa_memchunk rchunk;

            if (mbs == 0)
                mbs = pa_resampler_max_block_size(o->thread_info.resampler);

            if (qchunk.length > mbs)
                qchunk.length = mbs;

            pa_resampler_run(o->thread_info.resampler, &qchunk, &rchunk);

            if (rchunk.length > 0)
                o->push(o, &rchunk);

            if (rchunk.memblock)
                pa_memblock_unref(rchunk.memblock);
        }

        pa_memblock_unref(qchunk.memblock);
        pa_memblockq_drop(o->thread_info.delay_memblockq, qchunk.length);
    }
}

/* Called from thread context */
void pa_source_output_process_rewind(pa_source_output *o, size_t nbytes /* in source sample spec */) {

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &o->source->sample_spec));

    if (nbytes <= 0)
        return;

    if (o->process_rewind) {
        pa_assert(pa_memblockq_get_length(o->thread_info.delay_memblockq) == 0);

        if (o->thread_info.resampler)
            nbytes = pa_resampler_result(o->thread_info.resampler, nbytes);

        pa_log_debug("Have to rewind %lu bytes on implementor.", (unsigned long) nbytes);

        if (nbytes > 0)
            o->process_rewind(o, nbytes);

        if (o->thread_info.resampler)
            pa_resampler_reset(o->thread_info.resampler);

    } else
        pa_memblockq_rewind(o->thread_info.delay_memblockq, nbytes);
}

/* Called from thread context */
size_t pa_source_output_get_max_rewind(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);

    return o->thread_info.resampler ? pa_resampler_request(o->thread_info.resampler, o->source->thread_info.max_rewind) : o->source->thread_info.max_rewind;
}

/* Called from thread context */
void pa_source_output_update_max_rewind(pa_source_output *o, size_t nbytes  /* in the source's sample spec */) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &o->source->sample_spec));

    if (o->update_max_rewind)
        o->update_max_rewind(o, o->thread_info.resampler ? pa_resampler_result(o->thread_info.resampler, nbytes) : nbytes);
}

/* Called from thread context */
pa_usec_t pa_source_output_set_requested_latency_within_thread(pa_source_output *o, pa_usec_t usec) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);

    if (!(o->source->flags & PA_SOURCE_DYNAMIC_LATENCY))
        usec = o->source->thread_info.fixed_latency;

    if (usec != (pa_usec_t) -1)
        usec = PA_CLAMP(usec, o->source->thread_info.min_latency, o->source->thread_info.max_latency);

    o->thread_info.requested_source_latency = usec;
    pa_source_invalidate_requested_latency(o->source, TRUE);

    return usec;
}

/* Called from main context */
pa_usec_t pa_source_output_set_requested_latency(pa_source_output *o, pa_usec_t usec) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state) && o->source) {
        pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);
        return usec;
    }

    /* If this source output is not realized yet or is being moved, we
     * have to touch the thread info data directly */

    if (o->source) {
        if (!(o->source->flags & PA_SOURCE_DYNAMIC_LATENCY))
            usec = pa_source_get_fixed_latency(o->source);

        if (usec != (pa_usec_t) -1) {
            pa_usec_t min_latency, max_latency;
            pa_source_get_latency_range(o->source, &min_latency, &max_latency);
            usec = PA_CLAMP(usec, min_latency, max_latency);
        }
    }

    o->thread_info.requested_source_latency = usec;

    return usec;
}

/* Called from main context */
pa_usec_t pa_source_output_get_requested_latency(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state) && o->source) {
        pa_usec_t usec = 0;
        pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_GET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);
        return usec;
    }

    /* If this source output is not realized yet or is being moved, we
     * have to touch the thread info data directly */

    return o->thread_info.requested_source_latency;
}

/* Called from main context */
void pa_source_output_cork(pa_source_output *o, pa_bool_t b) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    source_output_set_state(o, b ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING);
}

/* Called from main context */
int pa_source_output_set_rate(pa_source_output *o, uint32_t rate) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_return_val_if_fail(o->thread_info.resampler, -PA_ERR_BADSTATE);

    if (o->sample_spec.rate == rate)
        return 0;

    o->sample_spec.rate = rate;

    pa_asyncmsgq_post(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_RATE, PA_UINT_TO_PTR(rate), 0, NULL, NULL);

    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    return 0;
}

/* Called from main context */
void pa_source_output_set_name(pa_source_output *o, const char *name) {
    const char *old;
    pa_assert_ctl_context();
    pa_source_output_assert_ref(o);

    if (!name && !pa_proplist_contains(o->proplist, PA_PROP_MEDIA_NAME))
        return;

    old = pa_proplist_gets(o->proplist, PA_PROP_MEDIA_NAME);

    if (old && name && !strcmp(old, name))
        return;

    if (name)
        pa_proplist_sets(o->proplist, PA_PROP_MEDIA_NAME, name);
    else
        pa_proplist_unset(o->proplist, PA_PROP_MEDIA_NAME);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state)) {
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PROPLIST_CHANGED], o);
        pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    }
}

/* Called from main thread */
void pa_source_output_update_proplist(pa_source_output *o, pa_update_mode_t mode, pa_proplist *p) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    if (p)
        pa_proplist_update(o->proplist, mode, p);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state)) {
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PROPLIST_CHANGED], o);
        pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    }
}

/* Called from main context */
pa_resample_method_t pa_source_output_get_resample_method(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    return o->actual_resample_method;
}

/* Called from main context */
pa_bool_t pa_source_output_may_move(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    if (o->flags & PA_SOURCE_OUTPUT_DONT_MOVE)
        return FALSE;

    if (o->direct_on_input)
        return FALSE;

    return TRUE;
}

/* Called from main context */
pa_bool_t pa_source_output_may_move_to(pa_source_output *o, pa_source *dest) {
    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_source_assert_ref(dest);

    if (dest == o->source)
        return TRUE;

    if (!pa_source_output_may_move(o))
        return FALSE;

    if (pa_idxset_size(dest->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log_warn("Failed to move source output: too many outputs per source.");
        return FALSE;
    }

    if (o->may_move_to)
        if (!o->may_move_to(o, dest))
            return FALSE;

    return TRUE;
}

/* Called from main context */
int pa_source_output_start_move(pa_source_output *o) {
    pa_source *origin;
    int r;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(o->source);

    if (!pa_source_output_may_move(o))
        return -PA_ERR_NOTSUPPORTED;

    if ((r = pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_START], o)) < 0)
        return r;

    origin = o->source;

    pa_idxset_remove_by_data(o->source->outputs, o, NULL);

    if (pa_source_output_get_state(o) == PA_SOURCE_OUTPUT_CORKED)
        pa_assert_se(origin->n_corked-- >= 1);

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_REMOVE_OUTPUT, o, 0, NULL) == 0);

    pa_source_update_status(o->source);
    o->source = NULL;

    pa_source_output_unref(o);

    return 0;
}

/* Called from main context */
int pa_source_output_finish_move(pa_source_output *o, pa_source *dest, pa_bool_t save) {
    pa_resampler *new_resampler;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(!o->source);
    pa_source_assert_ref(dest);

    if (!pa_source_output_may_move_to(o, dest))
        return -1;

    if (o->thread_info.resampler &&
        pa_sample_spec_equal(pa_resampler_input_sample_spec(o->thread_info.resampler), &dest->sample_spec) &&
        pa_channel_map_equal(pa_resampler_input_channel_map(o->thread_info.resampler), &dest->channel_map))

        /* Try to reuse the old resampler if possible */
        new_resampler = o->thread_info.resampler;

    else if ((o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ||
             !pa_sample_spec_equal(&o->sample_spec, &dest->sample_spec) ||
             !pa_channel_map_equal(&o->channel_map, &dest->channel_map)) {

        /* Okey, we need a new resampler for the new source */

        if (!(new_resampler = pa_resampler_new(
                      o->core->mempool,
                      &dest->sample_spec, &dest->channel_map,
                      &o->sample_spec, &o->channel_map,
                      o->requested_resample_method,
                      ((o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                      ((o->flags & PA_SOURCE_OUTPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                      (o->core->disable_remixing || (o->flags & PA_SOURCE_OUTPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0)))) {
            pa_log_warn("Unsupported resampling operation.");
            return -PA_ERR_NOTSUPPORTED;
        }
    } else
        new_resampler = NULL;

    if (o->moving)
        o->moving(o, dest);

    o->source = dest;
    o->save_source = save;
    pa_idxset_put(o->source->outputs, pa_source_output_ref(o), NULL);

    if (pa_source_output_get_state(o) == PA_SOURCE_OUTPUT_CORKED)
        o->source->n_corked++;

    /* Replace resampler */
    if (new_resampler != o->thread_info.resampler) {
        if (o->thread_info.resampler)
            pa_resampler_free(o->thread_info.resampler);
        o->thread_info.resampler = new_resampler;

        pa_memblockq_free(o->thread_info.delay_memblockq);

        o->thread_info.delay_memblockq = pa_memblockq_new(
                0,
                MEMBLOCKQ_MAXLENGTH,
                0,
                pa_frame_size(&o->source->sample_spec),
                0,
                1,
                0,
                &o->source->silence);
    }

    pa_source_update_status(dest);

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_ADD_OUTPUT, o, 0, NULL) == 0);

    pa_log_debug("Successfully moved source output %i to %s.", o->index, dest->name);

    /* Notify everyone */
    pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FINISH], o);
    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);

    return 0;
}

/* Called from main context */
void pa_source_output_fail_move(pa_source_output *o) {

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(!o->source);

    /* Check if someone wants this source output? */
    if (pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FAIL], o) == PA_HOOK_STOP)
        return;

    if (o->moving)
        o->moving(o, NULL);

    pa_source_output_kill(o);
}

/* Called from main context */
int pa_source_output_move_to(pa_source_output *o, pa_source *dest, pa_bool_t save) {
    int r;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(o->source);
    pa_source_assert_ref(dest);

    if (dest == o->source)
        return 0;

    if (!pa_source_output_may_move_to(o, dest))
        return -PA_ERR_NOTSUPPORTED;

    pa_source_output_ref(o);

    if ((r = pa_source_output_start_move(o)) < 0) {
        pa_source_output_unref(o);
        return r;
    }

    if ((r = pa_source_output_finish_move(o, dest, save)) < 0) {
        pa_source_output_fail_move(o);
        pa_source_output_unref(o);
        return r;
    }

    pa_source_output_unref(o);

    return 0;
}

/* Called from IO thread context */
void pa_source_output_set_state_within_thread(pa_source_output *o, pa_source_output_state_t state) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);

    if (state == o->thread_info.state)
        return;

    if (o->state_change)
        o->state_change(o, state);

    o->thread_info.state = state;
}

/* Called from IO thread context, except when it is not */
int pa_source_output_process_msg(pa_msgobject *mo, int code, void *userdata, int64_t offset, pa_memchunk* chunk) {
    pa_source_output *o = PA_SOURCE_OUTPUT(mo);
    pa_source_output_assert_ref(o);

    switch (code) {

        case PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            r[0] += pa_bytes_to_usec(pa_memblockq_get_length(o->thread_info.delay_memblockq), &o->source->sample_spec);
            r[1] += pa_source_get_latency_within_thread(o->source);

            return 0;
        }

        case PA_SOURCE_OUTPUT_MESSAGE_SET_RATE:

            o->thread_info.sample_spec.rate = PA_PTR_TO_UINT(userdata);
            pa_resampler_set_output_rate(o->thread_info.resampler, PA_PTR_TO_UINT(userdata));
            return 0;

        case PA_SOURCE_OUTPUT_MESSAGE_SET_STATE:

            pa_source_output_set_state_within_thread(o, PA_PTR_TO_UINT(userdata));
            return 0;

        case PA_SOURCE_OUTPUT_MESSAGE_SET_REQUESTED_LATENCY: {
            pa_usec_t *usec = userdata;

            *usec = pa_source_output_set_requested_latency_within_thread(o, *usec);

            return 0;
        }

        case PA_SOURCE_OUTPUT_MESSAGE_GET_REQUESTED_LATENCY: {
            pa_usec_t *r = userdata;

            *r = o->thread_info.requested_source_latency;
            return 0;
        }
    }

    return -PA_ERR_NOTIMPLEMENTED;
}

/* Called from main context */
void pa_source_output_send_event(pa_source_output *o, const char *event, pa_proplist *data) {
    pa_proplist *pl = NULL;
    pa_source_output_send_event_hook_data hook_data;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(event);

    if (!o->send_event)
        return;

    if (!data)
        data = pl = pa_proplist_new();

    hook_data.source_output = o;
    hook_data.data = data;
    hook_data.event = event;

    if (pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_SEND_EVENT], &hook_data) < 0)
        goto finish;

    o->send_event(o, event, data);

finish:
    if (pl)
        pa_proplist_free(pl);
}
