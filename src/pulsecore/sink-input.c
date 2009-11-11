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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/sample-util.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/play-memblockq.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "sink-input.h"

#define MEMBLOCKQ_MAXLENGTH (32*1024*1024)
#define CONVERT_BUFFER_LENGTH (PA_PAGE_SIZE)

PA_DEFINE_PUBLIC_CLASS(pa_sink_input, pa_msgobject);

static void sink_input_free(pa_object *o);
static void set_real_ratio(pa_sink_input *i, const pa_cvolume *v);

pa_sink_input_new_data* pa_sink_input_new_data_init(pa_sink_input_new_data *data) {
    pa_assert(data);

    pa_zero(*data);
    data->resample_method = PA_RESAMPLER_INVALID;
    data->proplist = pa_proplist_new();

    return data;
}

void pa_sink_input_new_data_set_sample_spec(pa_sink_input_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

void pa_sink_input_new_data_set_channel_map(pa_sink_input_new_data *data, const pa_channel_map *map) {
    pa_assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_sink_input_new_data_set_volume(pa_sink_input_new_data *data, const pa_cvolume *volume) {
    pa_assert(data);

    if ((data->volume_is_set = !!volume))
        data->volume = *volume;
}

void pa_sink_input_new_data_apply_volume_factor(pa_sink_input_new_data *data, const pa_cvolume *volume_factor) {
    pa_assert(data);
    pa_assert(volume_factor);

    if (data->volume_factor_is_set)
        pa_sw_cvolume_multiply(&data->volume_factor, &data->volume_factor, volume_factor);
    else {
        data->volume_factor_is_set = TRUE;
        data->volume_factor = *volume_factor;
    }
}

void pa_sink_input_new_data_apply_volume_factor_sink(pa_sink_input_new_data *data, const pa_cvolume *volume_factor) {
    pa_assert(data);
    pa_assert(volume_factor);

    if (data->volume_factor_sink_is_set)
        pa_sw_cvolume_multiply(&data->volume_factor_sink, &data->volume_factor_sink, volume_factor);
    else {
        data->volume_factor_sink_is_set = TRUE;
        data->volume_factor_sink = *volume_factor;
    }
}

void pa_sink_input_new_data_set_muted(pa_sink_input_new_data *data, pa_bool_t mute) {
    pa_assert(data);

    data->muted_is_set = TRUE;
    data->muted = !!mute;
}

void pa_sink_input_new_data_done(pa_sink_input_new_data *data) {
    pa_assert(data);

    pa_proplist_free(data->proplist);
}

/* Called from main context */
static void reset_callbacks(pa_sink_input *i) {
    pa_assert(i);

    i->pop = NULL;
    i->process_rewind = NULL;
    i->update_max_rewind = NULL;
    i->update_max_request = NULL;
    i->update_sink_requested_latency = NULL;
    i->update_sink_latency_range = NULL;
    i->update_sink_fixed_latency = NULL;
    i->attach = NULL;
    i->detach = NULL;
    i->suspend = NULL;
    i->suspend_within_thread = NULL;
    i->moving = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->state_change = NULL;
    i->may_move_to = NULL;
    i->send_event = NULL;
    i->volume_changed = NULL;
    i->mute_changed = NULL;
}

/* Called from main context */
int pa_sink_input_new(
        pa_sink_input **_i,
        pa_core *core,
        pa_sink_input_new_data *data) {

    pa_sink_input *i;
    pa_resampler *resampler = NULL;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_channel_map original_cm;
    int r;
    char *pt;

    pa_assert(_i);
    pa_assert(core);
    pa_assert(data);
    pa_assert_ctl_context();

    if (data->client)
        pa_proplist_update(data->proplist, PA_UPDATE_MERGE, data->client->proplist);

    if ((r = pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], data)) < 0)
        return r;

    pa_return_val_if_fail(!data->driver || pa_utf8_valid(data->driver), -PA_ERR_INVALID);

    if (!data->sink) {
        data->sink = pa_namereg_get(core, NULL, PA_NAMEREG_SINK);
        data->save_sink = FALSE;
    }

    pa_return_val_if_fail(data->sink, -PA_ERR_NOENTITY);
    pa_return_val_if_fail(PA_SINK_IS_LINKED(pa_sink_get_state(data->sink)), -PA_ERR_BADSTATE);
    pa_return_val_if_fail(!data->sync_base || (data->sync_base->sink == data->sink && pa_sink_input_get_state(data->sync_base) == PA_SINK_INPUT_CORKED), -PA_ERR_INVALID);

    if (!data->sample_spec_is_set)
        data->sample_spec = data->sink->sample_spec;

    pa_return_val_if_fail(pa_sample_spec_valid(&data->sample_spec), -PA_ERR_INVALID);

    if (!data->channel_map_is_set) {
        if (pa_channel_map_compatible(&data->sink->channel_map, &data->sample_spec))
            data->channel_map = data->sink->channel_map;
        else
            pa_channel_map_init_extend(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
    }

    pa_return_val_if_fail(pa_channel_map_compatible(&data->channel_map, &data->sample_spec), -PA_ERR_INVALID);

    if (!data->volume_is_set) {
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);
        data->volume_is_absolute = FALSE;
        data->save_volume = FALSE;
    }

    pa_return_val_if_fail(pa_cvolume_compatible(&data->volume, &data->sample_spec), -PA_ERR_INVALID);

    if (!data->volume_factor_is_set)
        pa_cvolume_reset(&data->volume_factor, data->sample_spec.channels);

    pa_return_val_if_fail(pa_cvolume_compatible(&data->volume_factor, &data->sample_spec), -PA_ERR_INVALID);

    if (!data->volume_factor_sink_is_set)
        pa_cvolume_reset(&data->volume_factor_sink, data->sink->sample_spec.channels);

    pa_return_val_if_fail(pa_cvolume_compatible(&data->volume_factor_sink, &data->sink->sample_spec), -PA_ERR_INVALID);

    if (!data->muted_is_set)
        data->muted = FALSE;

    if (data->flags & PA_SINK_INPUT_FIX_FORMAT)
        data->sample_spec.format = data->sink->sample_spec.format;

    if (data->flags & PA_SINK_INPUT_FIX_RATE)
        data->sample_spec.rate = data->sink->sample_spec.rate;

    original_cm = data->channel_map;

    if (data->flags & PA_SINK_INPUT_FIX_CHANNELS) {
        data->sample_spec.channels = data->sink->sample_spec.channels;
        data->channel_map = data->sink->channel_map;
    }

    pa_assert(pa_sample_spec_valid(&data->sample_spec));
    pa_assert(pa_channel_map_valid(&data->channel_map));

    /* Due to the fixing of the sample spec the volume might not match anymore */
    pa_cvolume_remap(&data->volume, &original_cm, &data->channel_map);

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    pa_return_val_if_fail(data->resample_method < PA_RESAMPLER_MAX, -PA_ERR_INVALID);

    if ((r = pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], data)) < 0)
        return r;

    if ((data->flags & PA_SINK_INPUT_NO_CREATE_ON_SUSPEND) &&
        pa_sink_get_state(data->sink) == PA_SINK_SUSPENDED) {
        pa_log_warn("Failed to create sink input: sink is suspended.");
        return -PA_ERR_BADSTATE;
    }

    if (pa_idxset_size(data->sink->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn("Failed to create sink input: too many inputs per sink.");
        return -PA_ERR_TOOLARGE;
    }

    if ((data->flags & PA_SINK_INPUT_VARIABLE_RATE) ||
        !pa_sample_spec_equal(&data->sample_spec, &data->sink->sample_spec) ||
        !pa_channel_map_equal(&data->channel_map, &data->sink->channel_map)) {

        if (!(resampler = pa_resampler_new(
                      core->mempool,
                      &data->sample_spec, &data->channel_map,
                      &data->sink->sample_spec, &data->sink->channel_map,
                      data->resample_method,
                      ((data->flags & PA_SINK_INPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                      ((data->flags & PA_SINK_INPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                      (core->disable_remixing || (data->flags & PA_SINK_INPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0) |
                      (core->disable_lfe_remixing ? PA_RESAMPLER_NO_LFE : 0)))) {
            pa_log_warn("Unsupported resampling operation.");
            return -PA_ERR_NOTSUPPORTED;
        }
    }

    i = pa_msgobject_new(pa_sink_input);
    i->parent.parent.free = sink_input_free;
    i->parent.process_msg = pa_sink_input_process_msg;

    i->core = core;
    i->state = PA_SINK_INPUT_INIT;
    i->flags = data->flags;
    i->proplist = pa_proplist_copy(data->proplist);
    i->driver = pa_xstrdup(pa_path_get_filename(data->driver));
    i->module = data->module;
    i->sink = data->sink;
    i->client = data->client;

    i->requested_resample_method = data->resample_method;
    i->actual_resample_method = resampler ? pa_resampler_get_method(resampler) : PA_RESAMPLER_INVALID;
    i->sample_spec = data->sample_spec;
    i->channel_map = data->channel_map;

    if ((i->sink->flags & PA_SINK_FLAT_VOLUME) && !data->volume_is_absolute) {
        pa_cvolume remapped;

        /* When the 'absolute' bool is not set then we'll treat the volume
         * as relative to the sink volume even in flat volume mode */
        remapped = data->sink->reference_volume;
        pa_cvolume_remap(&remapped, &data->sink->channel_map, &data->channel_map);
        pa_sw_cvolume_multiply(&i->volume, &data->volume, &remapped);
    } else
        i->volume = data->volume;

    i->volume_factor = data->volume_factor;
    i->volume_factor_sink = data->volume_factor_sink;
    i->real_ratio = i->reference_ratio = data->volume;
    pa_cvolume_reset(&i->soft_volume, i->sample_spec.channels);
    pa_cvolume_reset(&i->real_ratio, i->sample_spec.channels);
    i->save_volume = data->save_volume;
    i->save_sink = data->save_sink;
    i->save_muted = data->save_muted;

    i->muted = data->muted;

    if (data->sync_base) {
        i->sync_next = data->sync_base->sync_next;
        i->sync_prev = data->sync_base;

        if (data->sync_base->sync_next)
            data->sync_base->sync_next->sync_prev = i;
        data->sync_base->sync_next = i;
    } else
        i->sync_next = i->sync_prev = NULL;

    i->direct_outputs = pa_idxset_new(NULL, NULL);

    reset_callbacks(i);
    i->userdata = NULL;

    i->thread_info.state = i->state;
    i->thread_info.attached = FALSE;
    pa_atomic_store(&i->thread_info.drained, 1);
    i->thread_info.sample_spec = i->sample_spec;
    i->thread_info.resampler = resampler;
    i->thread_info.soft_volume = i->soft_volume;
    i->thread_info.muted = i->muted;
    i->thread_info.requested_sink_latency = (pa_usec_t) -1;
    i->thread_info.rewrite_nbytes = 0;
    i->thread_info.rewrite_flush = FALSE;
    i->thread_info.dont_rewind_render = FALSE;
    i->thread_info.underrun_for = (uint64_t) -1;
    i->thread_info.playing_for = 0;
    i->thread_info.direct_outputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    i->thread_info.render_memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            0,
            pa_frame_size(&i->sink->sample_spec),
            0,
            1,
            0,
            &i->sink->silence);

    pa_assert_se(pa_idxset_put(core->sink_inputs, i, &i->index) == 0);
    pa_assert_se(pa_idxset_put(i->sink->inputs, pa_sink_input_ref(i), NULL) == 0);

    if (i->client)
        pa_assert_se(pa_idxset_put(i->client->sink_inputs, i, NULL) >= 0);

    pt = pa_proplist_to_string_sep(i->proplist, "\n    ");
    pa_log_info("Created input %u \"%s\" on %s with sample spec %s and channel map %s\n    %s",
                i->index,
                pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME)),
                i->sink->name,
                pa_sample_spec_snprint(st, sizeof(st), &i->sample_spec),
                pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map),
                pt);
    pa_xfree(pt);

    /* Don't forget to call pa_sink_input_put! */

    *_i = i;
    return 0;
}

/* Called from main context */
static void update_n_corked(pa_sink_input *i, pa_sink_input_state_t state) {
    pa_assert(i);
    pa_assert_ctl_context();

    if (!i->sink)
        return;

    if (i->state == PA_SINK_INPUT_CORKED && state != PA_SINK_INPUT_CORKED)
        pa_assert_se(i->sink->n_corked -- >= 1);
    else if (i->state != PA_SINK_INPUT_CORKED && state == PA_SINK_INPUT_CORKED)
        i->sink->n_corked++;
}

/* Called from main context */
static void sink_input_set_state(pa_sink_input *i, pa_sink_input_state_t state) {
    pa_sink_input *ssync;
    pa_assert(i);
    pa_assert_ctl_context();

    if (state == PA_SINK_INPUT_DRAINED)
        state = PA_SINK_INPUT_RUNNING;

    if (i->state == state)
        return;

    pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL) == 0);

    update_n_corked(i, state);
    i->state = state;

    for (ssync = i->sync_prev; ssync; ssync = ssync->sync_prev) {
        update_n_corked(ssync, state);
        ssync->state = state;
    }
    for (ssync = i->sync_next; ssync; ssync = ssync->sync_next) {
        update_n_corked(ssync, state);
        ssync->state = state;
    }

    if (state != PA_SINK_INPUT_UNLINKED) {
        pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], i);

        for (ssync = i->sync_prev; ssync; ssync = ssync->sync_prev)
            pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], ssync);

        for (ssync = i->sync_next; ssync; ssync = ssync->sync_next)
            pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], ssync);
    }

    pa_sink_update_status(i->sink);
}

/* Called from main context */
void pa_sink_input_unlink(pa_sink_input *i) {
    pa_bool_t linked;
    pa_source_output *o, *p =  NULL;

    pa_assert(i);
    pa_assert_ctl_context();

    /* See pa_sink_unlink() for a couple of comments how this function
     * works */

    pa_sink_input_ref(i);

    linked = PA_SINK_INPUT_IS_LINKED(i->state);

    if (linked)
        pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], i);

    if (i->sync_prev)
        i->sync_prev->sync_next = i->sync_next;
    if (i->sync_next)
        i->sync_next->sync_prev = i->sync_prev;

    i->sync_prev = i->sync_next = NULL;

    pa_idxset_remove_by_data(i->core->sink_inputs, i, NULL);

    if (i->sink)
        if (pa_idxset_remove_by_data(i->sink->inputs, i, NULL))
            pa_sink_input_unref(i);

    if (i->client)
        pa_idxset_remove_by_data(i->client->sink_inputs, i, NULL);

    while ((o = pa_idxset_first(i->direct_outputs, NULL))) {
        pa_assert(o != p);
        pa_source_output_kill(o);
        p = o;
    }

    update_n_corked(i, PA_SINK_INPUT_UNLINKED);
    i->state = PA_SINK_INPUT_UNLINKED;

    if (linked && i->sink) {
        /* We might need to update the sink's volume if we are in flat volume mode. */
        if (i->sink->flags & PA_SINK_FLAT_VOLUME)
            pa_sink_set_volume(i->sink, NULL, FALSE, FALSE);

        if (i->sink->asyncmsgq)
            pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_REMOVE_INPUT, i, 0, NULL) == 0);
    }

    reset_callbacks(i);

    if (linked) {
        pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_REMOVE, i->index);
        pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK_POST], i);
    }

    if (i->sink) {
        pa_sink_update_status(i->sink);
        i->sink = NULL;
    }

    pa_core_maybe_vacuum(i->core);

    pa_sink_input_unref(i);
}

/* Called from main context */
static void sink_input_free(pa_object *o) {
    pa_sink_input* i = PA_SINK_INPUT(o);

    pa_assert(i);
    pa_assert_ctl_context();
    pa_assert(pa_sink_input_refcnt(i) == 0);

    if (PA_SINK_INPUT_IS_LINKED(i->state))
        pa_sink_input_unlink(i);

    pa_log_info("Freeing input %u \"%s\"", i->index, pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME)));

    /* Side note: this function must be able to destruct properly any
     * kind of sink input in any state, even those which are
     * "half-moved" or are connected to sinks that have no asyncmsgq
     * and are hence half-destructed themselves! */

    if (i->thread_info.render_memblockq)
        pa_memblockq_free(i->thread_info.render_memblockq);

    if (i->thread_info.resampler)
        pa_resampler_free(i->thread_info.resampler);

    if (i->proplist)
        pa_proplist_free(i->proplist);

    if (i->direct_outputs)
        pa_idxset_free(i->direct_outputs, NULL, NULL);

    if (i->thread_info.direct_outputs)
        pa_hashmap_free(i->thread_info.direct_outputs, NULL, NULL);

    pa_xfree(i->driver);
    pa_xfree(i);
}

/* Called from main context */
void pa_sink_input_put(pa_sink_input *i) {
    pa_sink_input_state_t state;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();

    pa_assert(i->state == PA_SINK_INPUT_INIT);

    /* The following fields must be initialized properly */
    pa_assert(i->pop);
    pa_assert(i->process_rewind);
    pa_assert(i->kill);

    state = i->flags & PA_SINK_INPUT_START_CORKED ? PA_SINK_INPUT_CORKED : PA_SINK_INPUT_RUNNING;

    update_n_corked(i, state);
    i->state = state;

    /* We might need to update the sink's volume if we are in flat volume mode. */
    if (i->sink->flags & PA_SINK_FLAT_VOLUME)
        pa_sink_set_volume(i->sink, NULL, FALSE, i->save_volume);
    else
        set_real_ratio(i, &i->volume);

    i->thread_info.soft_volume = i->soft_volume;
    i->thread_info.muted = i->muted;

    pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_ADD_INPUT, i, 0, NULL) == 0);

    pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, i->index);
    pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], i);

    pa_sink_update_status(i->sink);
}

/* Called from main context */
void pa_sink_input_kill(pa_sink_input*i) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));

    i->kill(i);
}

/* Called from main context */
pa_usec_t pa_sink_input_get_latency(pa_sink_input *i, pa_usec_t *sink_latency) {
    pa_usec_t r[2] = { 0, 0 };

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));

    pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_GET_LATENCY, r, 0, NULL) == 0);

    if (i->get_latency)
        r[0] += i->get_latency(i);

    if (sink_latency)
        *sink_latency = r[1];

    return r[0];
}

/* Called from thread context */
void pa_sink_input_peek(pa_sink_input *i, size_t slength /* in sink frames */, pa_memchunk *chunk, pa_cvolume *volume) {
    pa_bool_t do_volume_adj_here, need_volume_factor_sink;
    pa_bool_t volume_is_norm;
    size_t block_size_max_sink, block_size_max_sink_input;
    size_t ilength;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(slength, &i->sink->sample_spec));
    pa_assert(chunk);
    pa_assert(volume);

/*     pa_log_debug("peek"); */

    pa_assert(i->thread_info.state == PA_SINK_INPUT_RUNNING ||
              i->thread_info.state == PA_SINK_INPUT_CORKED ||
              i->thread_info.state == PA_SINK_INPUT_DRAINED);

    block_size_max_sink_input = i->thread_info.resampler ?
        pa_resampler_max_block_size(i->thread_info.resampler) :
        pa_frame_align(pa_mempool_block_size_max(i->core->mempool), &i->sample_spec);

    block_size_max_sink = pa_frame_align(pa_mempool_block_size_max(i->core->mempool), &i->sink->sample_spec);

    /* Default buffer size */
    if (slength <= 0)
        slength = pa_frame_align(CONVERT_BUFFER_LENGTH, &i->sink->sample_spec);

    if (slength > block_size_max_sink)
        slength = block_size_max_sink;

    if (i->thread_info.resampler) {
        ilength = pa_resampler_request(i->thread_info.resampler, slength);

        if (ilength <= 0)
            ilength = pa_frame_align(CONVERT_BUFFER_LENGTH, &i->sample_spec);
    } else
        ilength = slength;

    if (ilength > block_size_max_sink_input)
        ilength = block_size_max_sink_input;

    /* If the channel maps of the sink and this stream differ, we need
     * to adjust the volume *before* we resample. Otherwise we can do
     * it after and leave it for the sink code */

    do_volume_adj_here = !pa_channel_map_equal(&i->channel_map, &i->sink->channel_map);
    volume_is_norm = pa_cvolume_is_norm(&i->thread_info.soft_volume) && !i->thread_info.muted;
    need_volume_factor_sink = !pa_cvolume_is_norm(&i->volume_factor_sink);

    while (!pa_memblockq_is_readable(i->thread_info.render_memblockq)) {
        pa_memchunk tchunk;

        /* There's nothing in our render queue. We need to fill it up
         * with data from the implementor. */

        if (i->thread_info.state == PA_SINK_INPUT_CORKED ||
            i->pop(i, ilength, &tchunk) < 0) {

            /* OK, we're corked or the implementor didn't give us any
             * data, so let's just hand out silence */
            pa_atomic_store(&i->thread_info.drained, 1);

            pa_memblockq_seek(i->thread_info.render_memblockq, (int64_t) slength, PA_SEEK_RELATIVE, TRUE);
            i->thread_info.playing_for = 0;
            if (i->thread_info.underrun_for != (uint64_t) -1)
                i->thread_info.underrun_for += ilength;
            break;
        }

        pa_atomic_store(&i->thread_info.drained, 0);

        pa_assert(tchunk.length > 0);
        pa_assert(tchunk.memblock);

        i->thread_info.underrun_for = 0;
        i->thread_info.playing_for += tchunk.length;

        while (tchunk.length > 0) {
            pa_memchunk wchunk;
            pa_bool_t nvfs = need_volume_factor_sink;

            wchunk = tchunk;
            pa_memblock_ref(wchunk.memblock);

            if (wchunk.length > block_size_max_sink_input)
                wchunk.length = block_size_max_sink_input;

            /* It might be necessary to adjust the volume here */
            if (do_volume_adj_here && !volume_is_norm) {
                pa_memchunk_make_writable(&wchunk, 0);

                if (i->thread_info.muted) {
                    pa_silence_memchunk(&wchunk, &i->thread_info.sample_spec);
                    nvfs = FALSE;

                } else if (!i->thread_info.resampler && nvfs) {
                    pa_cvolume v;

                    /* If we don't need a resampler we can merge the
                     * post and the pre volume adjustment into one */

                    pa_sw_cvolume_multiply(&v, &i->thread_info.soft_volume, &i->volume_factor_sink);
                    pa_volume_memchunk(&wchunk, &i->thread_info.sample_spec, &v);
                    nvfs = FALSE;

                } else
                    pa_volume_memchunk(&wchunk, &i->thread_info.sample_spec, &i->thread_info.soft_volume);
            }

            if (!i->thread_info.resampler) {

                if (nvfs) {
                    pa_memchunk_make_writable(&wchunk, 0);
                    pa_volume_memchunk(&wchunk, &i->sink->sample_spec, &i->volume_factor_sink);
                }

                pa_memblockq_push_align(i->thread_info.render_memblockq, &wchunk);
            } else {
                pa_memchunk rchunk;
                pa_resampler_run(i->thread_info.resampler, &wchunk, &rchunk);

/*                 pa_log_debug("pushing %lu", (unsigned long) rchunk.length); */

                if (rchunk.memblock) {

                    if (nvfs) {
                        pa_memchunk_make_writable(&rchunk, 0);
                        pa_volume_memchunk(&rchunk, &i->sink->sample_spec, &i->volume_factor_sink);
                    }

                    pa_memblockq_push_align(i->thread_info.render_memblockq, &rchunk);
                    pa_memblock_unref(rchunk.memblock);
                }
            }

            pa_memblock_unref(wchunk.memblock);

            tchunk.index += wchunk.length;
            tchunk.length -= wchunk.length;
        }

        pa_memblock_unref(tchunk.memblock);
    }

    pa_assert_se(pa_memblockq_peek(i->thread_info.render_memblockq, chunk) >= 0);

    pa_assert(chunk->length > 0);
    pa_assert(chunk->memblock);

/*     pa_log_debug("peeking %lu", (unsigned long) chunk->length); */

    if (chunk->length > block_size_max_sink)
        chunk->length = block_size_max_sink;

    /* Let's see if we had to apply the volume adjustment ourselves,
     * or if this can be done by the sink for us */

    if (do_volume_adj_here)
        /* We had different channel maps, so we already did the adjustment */
        pa_cvolume_reset(volume, i->sink->sample_spec.channels);
    else if (i->thread_info.muted)
        /* We've both the same channel map, so let's have the sink do the adjustment for us*/
        pa_cvolume_mute(volume, i->sink->sample_spec.channels);
    else
        *volume = i->thread_info.soft_volume;
}

/* Called from thread context */
void pa_sink_input_drop(pa_sink_input *i, size_t nbytes /* in sink sample spec */) {

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &i->sink->sample_spec));
    pa_assert(nbytes > 0);

/*     pa_log_debug("dropping %lu", (unsigned long) nbytes); */

    pa_memblockq_drop(i->thread_info.render_memblockq, nbytes);
}

/* Called from thread context */
void pa_sink_input_process_rewind(pa_sink_input *i, size_t nbytes /* in sink sample spec */) {
    size_t lbq;
    pa_bool_t called = FALSE;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &i->sink->sample_spec));

/*     pa_log_debug("rewind(%lu, %lu)", (unsigned long) nbytes, (unsigned long) i->thread_info.rewrite_nbytes); */

    lbq = pa_memblockq_get_length(i->thread_info.render_memblockq);

    if (nbytes > 0 && !i->thread_info.dont_rewind_render) {
        pa_log_debug("Have to rewind %lu bytes on render memblockq.", (unsigned long) nbytes);
        pa_memblockq_rewind(i->thread_info.render_memblockq, nbytes);
    }

    if (i->thread_info.rewrite_nbytes == (size_t) -1) {

        /* We were asked to drop all buffered data, and rerequest new
         * data from implementor the next time push() is called */

        pa_memblockq_flush_write(i->thread_info.render_memblockq);

    } else if (i->thread_info.rewrite_nbytes > 0) {
        size_t max_rewrite, amount;

        /* Calculate how much make sense to rewrite at most */
        max_rewrite = nbytes + lbq;

        /* Transform into local domain */
        if (i->thread_info.resampler)
            max_rewrite = pa_resampler_request(i->thread_info.resampler, max_rewrite);

        /* Calculate how much of the rewinded data should actually be rewritten */
        amount = PA_MIN(i->thread_info.rewrite_nbytes, max_rewrite);

        if (amount > 0) {
            pa_log_debug("Have to rewind %lu bytes on implementor.", (unsigned long) amount);

            /* Tell the implementor */
            if (i->process_rewind)
                i->process_rewind(i, amount);
            called = TRUE;

            /* Convert back to to sink domain */
            if (i->thread_info.resampler)
                amount = pa_resampler_result(i->thread_info.resampler, amount);

            if (amount > 0)
                /* Ok, now update the write pointer */
                pa_memblockq_seek(i->thread_info.render_memblockq, - ((int64_t) amount), PA_SEEK_RELATIVE, TRUE);

            if (i->thread_info.rewrite_flush)
                pa_memblockq_silence(i->thread_info.render_memblockq);

            /* And reset the resampler */
            if (i->thread_info.resampler)
                pa_resampler_reset(i->thread_info.resampler);
        }
    }

    if (!called)
        if (i->process_rewind)
            i->process_rewind(i, 0);

    i->thread_info.rewrite_nbytes = 0;
    i->thread_info.rewrite_flush = FALSE;
    i->thread_info.dont_rewind_render = FALSE;
}

/* Called from thread context */
size_t pa_sink_input_get_max_rewind(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);

    return i->thread_info.resampler ? pa_resampler_request(i->thread_info.resampler, i->sink->thread_info.max_rewind) : i->sink->thread_info.max_rewind;
}

/* Called from thread context */
size_t pa_sink_input_get_max_request(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);

    /* We're not verifying the status here, to allow this to be called
     * in the state change handler between _INIT and _RUNNING */

    return i->thread_info.resampler ? pa_resampler_request(i->thread_info.resampler, i->sink->thread_info.max_request) : i->sink->thread_info.max_request;
}

/* Called from thread context */
void pa_sink_input_update_max_rewind(pa_sink_input *i, size_t nbytes  /* in the sink's sample spec */) {
    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &i->sink->sample_spec));

    pa_memblockq_set_maxrewind(i->thread_info.render_memblockq, nbytes);

    if (i->update_max_rewind)
        i->update_max_rewind(i, i->thread_info.resampler ? pa_resampler_request(i->thread_info.resampler, nbytes) : nbytes);
}

/* Called from thread context */
void pa_sink_input_update_max_request(pa_sink_input *i, size_t nbytes  /* in the sink's sample spec */) {
    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &i->sink->sample_spec));

    if (i->update_max_request)
        i->update_max_request(i, i->thread_info.resampler ? pa_resampler_request(i->thread_info.resampler, nbytes) : nbytes);
}

/* Called from thread context */
pa_usec_t pa_sink_input_set_requested_latency_within_thread(pa_sink_input *i, pa_usec_t usec) {
    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);

    if (!(i->sink->flags & PA_SINK_DYNAMIC_LATENCY))
        usec = i->sink->thread_info.fixed_latency;

    if (usec != (pa_usec_t) -1)
        usec = PA_CLAMP(usec, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);

    i->thread_info.requested_sink_latency = usec;
    pa_sink_invalidate_requested_latency(i->sink, TRUE);

    return usec;
}

/* Called from main context */
pa_usec_t pa_sink_input_set_requested_latency(pa_sink_input *i, pa_usec_t usec) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();

    if (PA_SINK_INPUT_IS_LINKED(i->state) && i->sink) {
        pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);
        return usec;
    }

    /* If this sink input is not realized yet or we are being moved,
     * we have to touch the thread info data directly */

    if (i->sink) {
        if (!(i->sink->flags & PA_SINK_DYNAMIC_LATENCY))
            usec = pa_sink_get_fixed_latency(i->sink);

        if (usec != (pa_usec_t) -1) {
            pa_usec_t min_latency, max_latency;
            pa_sink_get_latency_range(i->sink, &min_latency, &max_latency);
            usec =  PA_CLAMP(usec, min_latency, max_latency);
        }
    }

    i->thread_info.requested_sink_latency = usec;

    return usec;
}

/* Called from main context */
pa_usec_t pa_sink_input_get_requested_latency(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();

    if (PA_SINK_INPUT_IS_LINKED(i->state) && i->sink) {
        pa_usec_t usec = 0;
        pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_GET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);
        return usec;
    }

    /* If this sink input is not realized yet or we are being moved,
     * we have to touch the thread info data directly */

    return i->thread_info.requested_sink_latency;
}

/* Called from main context */
static void set_real_ratio(pa_sink_input *i, const pa_cvolume *v) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_assert(!v || pa_cvolume_compatible(v, &i->sample_spec));

    /* This basically calculates:
     *
     * i->real_ratio := v
     * i->soft_volume := i->real_ratio * i->volume_factor */

    if (v)
        i->real_ratio = *v;
    else
        pa_cvolume_reset(&i->real_ratio, i->sample_spec.channels);

    pa_sw_cvolume_multiply(&i->soft_volume, &i->real_ratio, &i->volume_factor);
    /* We don't copy the data to the thread_info data. That's left for someone else to do */
}

/* Called from main context */
void pa_sink_input_set_volume(pa_sink_input *i, const pa_cvolume *volume, pa_bool_t save, pa_bool_t absolute) {
    pa_cvolume v;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_assert(volume);
    pa_assert(pa_cvolume_valid(volume));
    pa_assert(volume->channels == 1 || pa_cvolume_compatible(volume, &i->sample_spec));

    if ((i->sink->flags & PA_SINK_FLAT_VOLUME) && !absolute) {
        v = i->sink->reference_volume;
        pa_cvolume_remap(&v, &i->sink->channel_map, &i->channel_map);

        if (pa_cvolume_compatible(volume, &i->sample_spec))
            volume = pa_sw_cvolume_multiply(&v, &v, volume);
        else
            volume = pa_sw_cvolume_multiply_scalar(&v, &v, pa_cvolume_max(volume));
    } else {

        if (!pa_cvolume_compatible(volume, &i->sample_spec)) {
            v = i->volume;
            volume = pa_cvolume_scale(&v, pa_cvolume_max(volume));
        }
    }

    if (pa_cvolume_equal(volume, &i->volume)) {
        i->save_volume = i->save_volume || save;
        return;
    }

    i->volume = *volume;
    i->save_volume = save;

    if (i->sink->flags & PA_SINK_FLAT_VOLUME)
        /* We are in flat volume mode, so let's update all sink input
         * volumes and update the flat volume of the sink */

        pa_sink_set_volume(i->sink, NULL, TRUE, save);

    else {
        /* OK, we are in normal volume mode. The volume only affects
         * ourselves */
        set_real_ratio(i, volume);

        /* Copy the new soft_volume to the thread_info struct */
        pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_SOFT_VOLUME, NULL, 0, NULL) == 0);
    }

    /* The volume changed, let's tell people so */
    if (i->volume_changed)
        i->volume_changed(i);

    pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

/* Called from main context */
pa_cvolume *pa_sink_input_get_volume(pa_sink_input *i, pa_cvolume *volume, pa_bool_t absolute) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));

    if (absolute || !(i->sink->flags & PA_SINK_FLAT_VOLUME))
        *volume = i->volume;
    else
        *volume = i->reference_ratio;

    return volume;
}

/* Called from main context */
void pa_sink_input_set_mute(pa_sink_input *i, pa_bool_t mute, pa_bool_t save) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));

    if (!i->muted == !mute)
        return;

    i->muted = mute;
    i->save_muted = save;

    pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_SOFT_MUTE, NULL, 0, NULL) == 0);

    /* The mute status changed, let's tell people so */
    if (i->mute_changed)
        i->mute_changed(i);

    pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

/* Called from main context */
pa_bool_t pa_sink_input_get_mute(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));

    return i->muted;
}

/* Called from main thread */
void pa_sink_input_update_proplist(pa_sink_input *i, pa_update_mode_t mode, pa_proplist *p) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();

    if (p)
        pa_proplist_update(i->proplist, mode, p);

    if (PA_SINK_IS_LINKED(i->state)) {
        pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], i);
        pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    }
}

/* Called from main context */
void pa_sink_input_cork(pa_sink_input *i, pa_bool_t b) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));

    sink_input_set_state(i, b ? PA_SINK_INPUT_CORKED : PA_SINK_INPUT_RUNNING);
}

/* Called from main context */
int pa_sink_input_set_rate(pa_sink_input *i, uint32_t rate) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_return_val_if_fail(i->thread_info.resampler, -PA_ERR_BADSTATE);

    if (i->sample_spec.rate == rate)
        return 0;

    i->sample_spec.rate = rate;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_RATE, PA_UINT_TO_PTR(rate), 0, NULL, NULL);

    pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    return 0;
}

/* Called from main context */
void pa_sink_input_set_name(pa_sink_input *i, const char *name) {
    const char *old;
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();

    if (!name && !pa_proplist_contains(i->proplist, PA_PROP_MEDIA_NAME))
        return;

    old = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME);

    if (old && name && pa_streq(old, name))
        return;

    if (name)
        pa_proplist_sets(i->proplist, PA_PROP_MEDIA_NAME, name);
    else
        pa_proplist_unset(i->proplist, PA_PROP_MEDIA_NAME);

    if (PA_SINK_INPUT_IS_LINKED(i->state)) {
        pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], i);
        pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    }
}

/* Called from main context */
pa_resample_method_t pa_sink_input_get_resample_method(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();

    return i->actual_resample_method;
}

/* Called from main context */
pa_bool_t pa_sink_input_may_move(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));

    if (i->flags & PA_SINK_INPUT_DONT_MOVE)
        return FALSE;

    if (i->sync_next || i->sync_prev) {
        pa_log_warn("Moving synchronised streams not supported.");
        return FALSE;
    }

    return TRUE;
}

/* Called from main context */
pa_bool_t pa_sink_input_may_move_to(pa_sink_input *i, pa_sink *dest) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_sink_assert_ref(dest);

    if (dest == i->sink)
        return TRUE;

    if (!pa_sink_input_may_move(i))
        return FALSE;

    if (pa_idxset_size(dest->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn("Failed to move sink input: too many inputs per sink.");
        return FALSE;
    }

    if (i->may_move_to)
        if (!i->may_move_to(i, dest))
            return FALSE;

    return TRUE;
}

/* Called from main context */
int pa_sink_input_start_move(pa_sink_input *i) {
    pa_source_output *o, *p = NULL;
    int r;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_assert(i->sink);

    if (!pa_sink_input_may_move(i))
        return -PA_ERR_NOTSUPPORTED;

    if ((r = pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], i)) < 0)
        return r;

    /* Kill directly connected outputs */
    while ((o = pa_idxset_first(i->direct_outputs, NULL))) {
        pa_assert(o != p);
        pa_source_output_kill(o);
        p = o;
    }
    pa_assert(pa_idxset_isempty(i->direct_outputs));

    pa_idxset_remove_by_data(i->sink->inputs, i, NULL);

    if (pa_sink_input_get_state(i) == PA_SINK_INPUT_CORKED)
        pa_assert_se(i->sink->n_corked-- >= 1);

    if (i->sink->flags & PA_SINK_FLAT_VOLUME)
        /* We might need to update the sink's volume if we are in flat
         * volume mode. */
        pa_sink_set_volume(i->sink, NULL, FALSE, FALSE);

    pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_START_MOVE, i, 0, NULL) == 0);

    pa_sink_update_status(i->sink);
    pa_cvolume_remap(&i->volume_factor_sink, &i->sink->channel_map, &i->channel_map);
    i->sink = NULL;

    pa_sink_input_unref(i);

    return 0;
}

/* Called from main context */
int pa_sink_input_finish_move(pa_sink_input *i, pa_sink *dest, pa_bool_t save) {
    pa_resampler *new_resampler;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_assert(!i->sink);
    pa_sink_assert_ref(dest);

    if (!pa_sink_input_may_move_to(i, dest))
        return -PA_ERR_NOTSUPPORTED;

    if (i->thread_info.resampler &&
        pa_sample_spec_equal(pa_resampler_output_sample_spec(i->thread_info.resampler), &dest->sample_spec) &&
        pa_channel_map_equal(pa_resampler_output_channel_map(i->thread_info.resampler), &dest->channel_map))

        /* Try to reuse the old resampler if possible */
        new_resampler = i->thread_info.resampler;

    else if ((i->flags & PA_SINK_INPUT_VARIABLE_RATE) ||
             !pa_sample_spec_equal(&i->sample_spec, &dest->sample_spec) ||
             !pa_channel_map_equal(&i->channel_map, &dest->channel_map)) {

        /* Okey, we need a new resampler for the new sink */

        if (!(new_resampler = pa_resampler_new(
                      i->core->mempool,
                      &i->sample_spec, &i->channel_map,
                      &dest->sample_spec, &dest->channel_map,
                      i->requested_resample_method,
                      ((i->flags & PA_SINK_INPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                      ((i->flags & PA_SINK_INPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                      (i->core->disable_remixing || (i->flags & PA_SINK_INPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0)))) {
            pa_log_warn("Unsupported resampling operation.");
            return -PA_ERR_NOTSUPPORTED;
        }
    } else
        new_resampler = NULL;

    if (i->moving)
        i->moving(i, dest);

    i->sink = dest;
    i->save_sink = save;
    pa_idxset_put(dest->inputs, pa_sink_input_ref(i), NULL);

    pa_cvolume_remap(&i->volume_factor_sink, &i->channel_map, &i->sink->channel_map);

    if (pa_sink_input_get_state(i) == PA_SINK_INPUT_CORKED)
        i->sink->n_corked++;

    /* Replace resampler and render queue */
    if (new_resampler != i->thread_info.resampler) {

        if (i->thread_info.resampler)
            pa_resampler_free(i->thread_info.resampler);
        i->thread_info.resampler = new_resampler;

        pa_memblockq_free(i->thread_info.render_memblockq);

        i->thread_info.render_memblockq = pa_memblockq_new(
                0,
                MEMBLOCKQ_MAXLENGTH,
                0,
                pa_frame_size(&i->sink->sample_spec),
                0,
                1,
                0,
                &i->sink->silence);
    }
    pa_sink_update_status(dest);

    if (i->sink->flags & PA_SINK_FLAT_VOLUME) {
        pa_cvolume remapped;

        /* Make relative volumes absolute */
        remapped = dest->reference_volume;
        pa_cvolume_remap(&remapped, &dest->channel_map, &i->channel_map);
        pa_sw_cvolume_multiply(&i->volume, &i->reference_ratio, &remapped);

        /* We might need to update the sink's volume if we are in flat volume mode. */
        pa_sink_set_volume(i->sink, NULL, FALSE, i->save_volume);
    }

    pa_assert_se(pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_FINISH_MOVE, i, 0, NULL) == 0);

    pa_log_debug("Successfully moved sink input %i to %s.", i->index, dest->name);

    /* Notify everyone */
    pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], i);

    if (i->volume_changed)
        i->volume_changed(i);

    pa_subscription_post(i->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);

    return 0;
}

/* Called from main context */
void pa_sink_input_fail_move(pa_sink_input *i) {

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_assert(!i->sink);

    /* Check if someone wants this sink input? */
    if (pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FAIL], i) == PA_HOOK_STOP)
        return;

    if (i->moving)
        i->moving(i, NULL);

    pa_sink_input_kill(i);
}

/* Called from main context */
int pa_sink_input_move_to(pa_sink_input *i, pa_sink *dest, pa_bool_t save) {
    int r;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(PA_SINK_INPUT_IS_LINKED(i->state));
    pa_assert(i->sink);
    pa_sink_assert_ref(dest);

    if (dest == i->sink)
        return 0;

    if (!pa_sink_input_may_move_to(i, dest))
        return -PA_ERR_NOTSUPPORTED;

    pa_sink_input_ref(i);

    if ((r = pa_sink_input_start_move(i)) < 0) {
        pa_sink_input_unref(i);
        return r;
    }

    if ((r = pa_sink_input_finish_move(i, dest, save)) < 0) {
        pa_sink_input_fail_move(i);
        pa_sink_input_unref(i);
        return r;
    }

    pa_sink_input_unref(i);

    return 0;
}

/* Called from IO thread context */
void pa_sink_input_set_state_within_thread(pa_sink_input *i, pa_sink_input_state_t state) {
    pa_bool_t corking, uncorking;

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);

    if (state == i->thread_info.state)
        return;

    if ((state == PA_SINK_INPUT_DRAINED || state == PA_SINK_INPUT_RUNNING) &&
        !(i->thread_info.state == PA_SINK_INPUT_DRAINED || i->thread_info.state != PA_SINK_INPUT_RUNNING))
        pa_atomic_store(&i->thread_info.drained, 1);

    corking = state == PA_SINK_INPUT_CORKED && i->thread_info.state == PA_SINK_INPUT_RUNNING;
    uncorking = i->thread_info.state == PA_SINK_INPUT_CORKED && state == PA_SINK_INPUT_RUNNING;

    if (i->state_change)
        i->state_change(i, state);

    i->thread_info.state = state;

    if (corking) {

        pa_log_debug("Requesting rewind due to corking");

        /* This will tell the implementing sink input driver to rewind
         * so that the unplayed already mixed data is not lost */
        pa_sink_input_request_rewind(i, 0, TRUE, TRUE, FALSE);

    } else if (uncorking) {

        i->thread_info.underrun_for = (uint64_t) -1;
        i->thread_info.playing_for = 0;

        pa_log_debug("Requesting rewind due to uncorking");

        /* OK, we're being uncorked. Make sure we're not rewound when
         * the hw buffer is remixed and request a remix. */
        pa_sink_input_request_rewind(i, 0, FALSE, TRUE, TRUE);
    }
}

/* Called from thread context, except when it is not. */
int pa_sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink_input *i = PA_SINK_INPUT(o);
    pa_sink_input_assert_ref(i);

    switch (code) {

        case PA_SINK_INPUT_MESSAGE_SET_SOFT_VOLUME:
            if (!pa_cvolume_equal(&i->thread_info.soft_volume, &i->soft_volume)) {
                i->thread_info.soft_volume = i->soft_volume;
                pa_sink_input_request_rewind(i, 0, TRUE, FALSE, FALSE);
            }
            return 0;

        case PA_SINK_INPUT_MESSAGE_SET_SOFT_MUTE:
            if (i->thread_info.muted != i->muted) {
                i->thread_info.muted = i->muted;
                pa_sink_input_request_rewind(i, 0, TRUE, FALSE, FALSE);
            }
            return 0;

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            r[0] += pa_bytes_to_usec(pa_memblockq_get_length(i->thread_info.render_memblockq), &i->sink->sample_spec);
            r[1] += pa_sink_get_latency_within_thread(i->sink);

            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_SET_RATE:

            i->thread_info.sample_spec.rate = PA_PTR_TO_UINT(userdata);
            pa_resampler_set_input_rate(i->thread_info.resampler, PA_PTR_TO_UINT(userdata));

            return 0;

        case PA_SINK_INPUT_MESSAGE_SET_STATE: {
            pa_sink_input *ssync;

            pa_sink_input_set_state_within_thread(i, PA_PTR_TO_UINT(userdata));

            for (ssync = i->thread_info.sync_prev; ssync; ssync = ssync->thread_info.sync_prev)
                pa_sink_input_set_state_within_thread(ssync, PA_PTR_TO_UINT(userdata));

            for (ssync = i->thread_info.sync_next; ssync; ssync = ssync->thread_info.sync_next)
                pa_sink_input_set_state_within_thread(ssync, PA_PTR_TO_UINT(userdata));

            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_SET_REQUESTED_LATENCY: {
            pa_usec_t *usec = userdata;

            *usec = pa_sink_input_set_requested_latency_within_thread(i, *usec);
            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_GET_REQUESTED_LATENCY: {
            pa_usec_t *r = userdata;

            *r = i->thread_info.requested_sink_latency;
            return 0;
        }
    }

    return -PA_ERR_NOTIMPLEMENTED;
}

/* Called from main thread */
pa_sink_input_state_t pa_sink_input_get_state(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();

    if (i->state == PA_SINK_INPUT_RUNNING || i->state == PA_SINK_INPUT_DRAINED)
        return pa_atomic_load(&i->thread_info.drained) ? PA_SINK_INPUT_DRAINED : PA_SINK_INPUT_RUNNING;

    return i->state;
}

/* Called from IO context */
pa_bool_t pa_sink_input_safe_to_remove(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);

    if (PA_SINK_INPUT_IS_LINKED(i->thread_info.state))
        return pa_memblockq_is_empty(i->thread_info.render_memblockq);

    return TRUE;
}

/* Called from IO context */
void pa_sink_input_request_rewind(
        pa_sink_input *i,
        size_t nbytes  /* in our sample spec */,
        pa_bool_t rewrite,
        pa_bool_t flush,
        pa_bool_t dont_rewind_render) {

    size_t lbq;

    /* If 'rewrite' is TRUE the sink is rewound as far as requested
     * and possible and the exact value of this is passed back the
     * implementor via process_rewind(). If 'flush' is also TRUE all
     * already rendered data is also dropped.
     *
     * If 'rewrite' is FALSE the sink is rewound as far as requested
     * and possible and the already rendered data is dropped so that
     * in the next iteration we read new data from the
     * implementor. This implies 'flush' is TRUE.  If
     * dont_rewind_render is TRUE then the render memblockq is not
     * rewound. */

    /* nbytes = 0 means maximum rewind request */

    pa_sink_input_assert_ref(i);
    pa_sink_input_assert_io_context(i);
    pa_assert(rewrite || flush);
    pa_assert(!dont_rewind_render || !rewrite);

    /* We don't take rewind requests while we are corked */
    if (i->thread_info.state == PA_SINK_INPUT_CORKED)
        return;

    nbytes = PA_MAX(i->thread_info.rewrite_nbytes, nbytes);

    /* pa_log_debug("request rewrite %zu", nbytes); */

    /* Calculate how much we can rewind locally without having to
     * touch the sink */
    if (rewrite)
        lbq = pa_memblockq_get_length(i->thread_info.render_memblockq);
    else
        lbq = 0;

    /* Check if rewinding for the maximum is requested, and if so, fix up */
    if (nbytes <= 0) {

        /* Calculate maximum number of bytes that could be rewound in theory */
        nbytes = i->sink->thread_info.max_rewind + lbq;

        /* Transform from sink domain */
        if (i->thread_info.resampler)
            nbytes = pa_resampler_request(i->thread_info.resampler, nbytes);
    }

    /* Remember how much we actually want to rewrite */
    if (i->thread_info.rewrite_nbytes != (size_t) -1) {
        if (rewrite) {
            /* Make sure to not overwrite over underruns */
            if (nbytes > i->thread_info.playing_for)
                nbytes = (size_t) i->thread_info.playing_for;

            i->thread_info.rewrite_nbytes = nbytes;
        } else
            i->thread_info.rewrite_nbytes = (size_t) -1;
    }

    i->thread_info.rewrite_flush =
        i->thread_info.rewrite_flush ||
        (flush && i->thread_info.rewrite_nbytes != 0);

    i->thread_info.dont_rewind_render =
        i->thread_info.dont_rewind_render ||
        dont_rewind_render;

    if (nbytes != (size_t) -1) {

        /* Transform to sink domain */
        if (i->thread_info.resampler)
            nbytes = pa_resampler_result(i->thread_info.resampler, nbytes);

        if (nbytes > lbq)
            pa_sink_request_rewind(i->sink, nbytes - lbq);
        else
            /* This call will make sure process_rewind() is called later */
            pa_sink_request_rewind(i->sink, 0);
    }
}

/* Called from main context */
pa_memchunk* pa_sink_input_get_silence(pa_sink_input *i, pa_memchunk *ret) {
    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(ret);

    /* FIXME: Shouldn't access resampler object from main context! */

    pa_silence_memchunk_get(
                &i->core->silence_cache,
                i->core->mempool,
                ret,
                &i->sample_spec,
                i->thread_info.resampler ? pa_resampler_max_block_size(i->thread_info.resampler) : 0);

    return ret;
}

/* Called from main context */
void pa_sink_input_send_event(pa_sink_input *i, const char *event, pa_proplist *data) {
    pa_proplist *pl = NULL;
    pa_sink_input_send_event_hook_data hook_data;

    pa_sink_input_assert_ref(i);
    pa_assert_ctl_context();
    pa_assert(event);

    if (!i->send_event)
        return;

    if (!data)
        data = pl = pa_proplist_new();

    hook_data.sink_input = i;
    hook_data.data = data;
    hook_data.event = event;

    if (pa_hook_fire(&i->core->hooks[PA_CORE_HOOK_SINK_INPUT_SEND_EVENT], &hook_data) < 0)
        goto finish;

    i->send_event(i, event, data);

finish:
    if (pl)
        pa_proplist_free(pl);
}
