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

#include <pulsecore/sample-util.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/play-memblockq.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "sink-input.h"

#define MEMBLOCKQ_MAXLENGTH (32*1024*1024)
#define CONVERT_BUFFER_LENGTH (PA_PAGE_SIZE)
#define MOVE_BUFFER_LENGTH (PA_PAGE_SIZE*256)

static PA_DEFINE_CHECK_TYPE(pa_sink_input, pa_msgobject);

static void sink_input_free(pa_object *o);

pa_sink_input_new_data* pa_sink_input_new_data_init(pa_sink_input_new_data *data) {
    pa_assert(data);

    memset(data, 0, sizeof(*data));
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

void pa_sink_input_new_data_set_muted(pa_sink_input_new_data *data, pa_bool_t mute) {
    pa_assert(data);

    data->muted_is_set = TRUE;
    data->muted = !!mute;
}

void pa_sink_input_new_data_done(pa_sink_input_new_data *data) {
    pa_assert(data);

    pa_proplist_free(data->proplist);
}

static void reset_callbacks(pa_sink_input *i) {
    pa_assert(i);

    i->pop = NULL;
    i->process_rewind = NULL;
    i->update_max_rewind = NULL;
    i->attach = NULL;
    i->detach = NULL;
    i->suspend = NULL;
    i->moved = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
}

pa_sink_input* pa_sink_input_new(
        pa_core *core,
        pa_sink_input_new_data *data,
        pa_sink_input_flags_t flags) {

    pa_sink_input *i;
    pa_resampler *resampler = NULL;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    pa_assert(core);
    pa_assert(data);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], data) < 0)
        return NULL;

    pa_return_null_if_fail(!data->driver || pa_utf8_valid(data->driver));

    if (!data->sink)
        data->sink = pa_namereg_get(core, NULL, PA_NAMEREG_SINK, 1);

    pa_return_null_if_fail(data->sink);
    pa_return_null_if_fail(pa_sink_get_state(data->sink) != PA_SINK_UNLINKED);
    pa_return_null_if_fail(!data->sync_base || (data->sync_base->sink == data->sink && pa_sink_input_get_state(data->sync_base) == PA_SINK_INPUT_CORKED));

    if (!data->sample_spec_is_set)
        data->sample_spec = data->sink->sample_spec;

    pa_return_null_if_fail(pa_sample_spec_valid(&data->sample_spec));

    if (!data->channel_map_is_set) {
        if (data->sink->channel_map.channels == data->sample_spec.channels)
            data->channel_map = data->sink->channel_map;
        else
            pa_return_null_if_fail(pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT));
    }

    pa_return_null_if_fail(pa_channel_map_valid(&data->channel_map));
    pa_return_null_if_fail(data->channel_map.channels == data->sample_spec.channels);

    if (!data->volume_is_set)
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);

    pa_return_null_if_fail(pa_cvolume_valid(&data->volume));
    pa_return_null_if_fail(data->volume.channels == data->sample_spec.channels);

    if (!data->muted_is_set)
        data->muted = FALSE;

    if (flags & PA_SINK_INPUT_FIX_FORMAT)
        data->sample_spec.format = data->sink->sample_spec.format;

    if (flags & PA_SINK_INPUT_FIX_RATE)
        data->sample_spec.rate = data->sink->sample_spec.rate;

    if (flags & PA_SINK_INPUT_FIX_CHANNELS) {
        data->sample_spec.channels = data->sink->sample_spec.channels;
        data->channel_map = data->sink->channel_map;
    }

    pa_assert(pa_sample_spec_valid(&data->sample_spec));
    pa_assert(pa_channel_map_valid(&data->channel_map));

    /* Due to the fixing of the sample spec the volume might not match anymore */
    if (data->volume.channels != data->sample_spec.channels)
        pa_cvolume_set(&data->volume, data->sample_spec.channels, pa_cvolume_avg(&data->volume));

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    pa_return_null_if_fail(data->resample_method < PA_RESAMPLER_MAX);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], data) < 0)
        return NULL;

    if (pa_idxset_size(data->sink->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn("Failed to create sink input: too many inputs per sink.");
        return NULL;
    }

    if ((flags & PA_SINK_INPUT_VARIABLE_RATE) ||
        !pa_sample_spec_equal(&data->sample_spec, &data->sink->sample_spec) ||
        !pa_channel_map_equal(&data->channel_map, &data->sink->channel_map)) {

        if (!(resampler = pa_resampler_new(
                      core->mempool,
                      &data->sample_spec, &data->channel_map,
                      &data->sink->sample_spec, &data->sink->channel_map,
                      data->resample_method,
                      ((flags & PA_SINK_INPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                      ((flags & PA_SINK_INPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                      (core->disable_remixing || (flags & PA_SINK_INPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0)))) {
            pa_log_warn("Unsupported resampling operation.");
            return NULL;
        }

        data->resample_method = pa_resampler_get_method(resampler);
    }

    i = pa_msgobject_new(pa_sink_input);
    i->parent.parent.free = sink_input_free;
    i->parent.process_msg = pa_sink_input_process_msg;

    i->core = core;
    i->state = PA_SINK_INPUT_INIT;
    i->flags = flags;
    i->proplist = pa_proplist_copy(data->proplist);
    i->driver = pa_xstrdup(data->driver);
    i->module = data->module;
    i->sink = data->sink;
    i->client = data->client;

    i->resample_method = data->resample_method;
    i->sample_spec = data->sample_spec;
    i->channel_map = data->channel_map;

    i->volume = data->volume;
    i->muted = data->muted;

    if (data->sync_base) {
        i->sync_next = data->sync_base->sync_next;
        i->sync_prev = data->sync_base;

        if (data->sync_base->sync_next)
            data->sync_base->sync_next->sync_prev = i;
        data->sync_base->sync_next = i;
    } else
        i->sync_next = i->sync_prev = NULL;

    reset_callbacks(i);
    i->userdata = NULL;

    i->thread_info.state = i->state;
    i->thread_info.attached = FALSE;
    pa_atomic_store(&i->thread_info.drained, 1);
    pa_atomic_store(&i->thread_info.render_memblockq_is_empty, 0);
    i->thread_info.sample_spec = i->sample_spec;
    i->thread_info.resampler = resampler;
    i->thread_info.volume = i->volume;
    i->thread_info.muted = i->muted;
    i->thread_info.requested_sink_latency = 0;
    i->thread_info.rewrite_nbytes = 0;
    i->thread_info.since_underrun = 0;
    i->thread_info.ignore_rewind = FALSE;

    i->thread_info.render_memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            0,
            pa_frame_size(&i->sink->sample_spec),
            0,
            1,
            0,
            &i->sink->silence);

    pa_assert_se(pa_idxset_put(core->sink_inputs, pa_sink_input_ref(i), &i->index) == 0);
    pa_assert_se(pa_idxset_put(i->sink->inputs, i, NULL) == 0);

    pa_log_info("Created input %u \"%s\" on %s with sample spec %s and channel map %s",
                i->index,
                pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME)),
                i->sink->name,
                pa_sample_spec_snprint(st, sizeof(st), &i->sample_spec),
                pa_channel_map_snprint(cm, sizeof(cm), &i->channel_map));

    /* Don't forget to call pa_sink_input_put! */

    return i;
}

static void update_n_corked(pa_sink_input *i, pa_sink_input_state_t state) {
    pa_assert(i);

    if (i->state == PA_SINK_INPUT_CORKED && state != PA_SINK_INPUT_CORKED)
        pa_assert_se(i->sink->n_corked -- >= 1);
    else if (i->state != PA_SINK_INPUT_CORKED && state == PA_SINK_INPUT_CORKED)
        i->sink->n_corked++;

    pa_sink_update_status(i->sink);
}

static int sink_input_set_state(pa_sink_input *i, pa_sink_input_state_t state) {
    pa_sink_input *ssync;
    pa_assert(i);

    if (state == PA_SINK_INPUT_DRAINED)
        state = PA_SINK_INPUT_RUNNING;

    if (i->state == state)
        return 0;

    if (pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL) < 0)
        return -1;

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

    if (state != PA_SINK_INPUT_UNLINKED)
        pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], i);

    return 0;
}

void pa_sink_input_unlink(pa_sink_input *i) {
    pa_bool_t linked;
    pa_assert(i);

    /* See pa_sink_unlink() for a couple of comments how this function
     * works */

    pa_sink_input_ref(i);

    linked = PA_SINK_INPUT_LINKED(i->state);

    if (linked)
        pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], i);

    if (i->sync_prev)
        i->sync_prev->sync_next = i->sync_next;
    if (i->sync_next)
        i->sync_next->sync_prev = i->sync_prev;

    i->sync_prev = i->sync_next = NULL;

    pa_idxset_remove_by_data(i->sink->core->sink_inputs, i, NULL);
    if (pa_idxset_remove_by_data(i->sink->inputs, i, NULL))
        pa_sink_input_unref(i);

    if (linked) {
        pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_REMOVE_INPUT, i, 0, NULL);
        sink_input_set_state(i, PA_SINK_INPUT_UNLINKED);
        pa_sink_update_status(i->sink);
    } else
        i->state = PA_SINK_INPUT_UNLINKED;

    reset_callbacks(i);

    if (linked) {
        pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_REMOVE, i->index);
        pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK_POST], i);
    }

    i->sink = NULL;
    pa_sink_input_unref(i);
}

static void sink_input_free(pa_object *o) {
    pa_sink_input* i = PA_SINK_INPUT(o);

    pa_assert(i);
    pa_assert(pa_sink_input_refcnt(i) == 0);

    if (PA_SINK_INPUT_LINKED(i->state))
        pa_sink_input_unlink(i);

    pa_log_info("Freeing input %u \"%s\"", i->index, pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME)));

    pa_assert(!i->thread_info.attached);

    if (i->thread_info.render_memblockq)
        pa_memblockq_free(i->thread_info.render_memblockq);

    if (i->thread_info.resampler)
        pa_resampler_free(i->thread_info.resampler);

    if (i->proplist)
        pa_proplist_free(i->proplist);

    pa_xfree(i->driver);
    pa_xfree(i);
}

void pa_sink_input_put(pa_sink_input *i) {
    pa_sink_input_state_t state;
    pa_sink_input_assert_ref(i);

    pa_assert(i->state == PA_SINK_INPUT_INIT);
    pa_assert(i->pop);
    pa_assert(i->process_rewind);

    i->thread_info.volume = i->volume;
    i->thread_info.muted = i->muted;

    state = i->flags & PA_SINK_INPUT_START_CORKED ? PA_SINK_INPUT_CORKED : PA_SINK_INPUT_RUNNING;

    update_n_corked(i, state);
    i->thread_info.state = i->state = state;

    pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_ADD_INPUT, i, 0, NULL);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, i->index);
    pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], i);

    /* Please note that if you change something here, you have to
       change something in pa_sink_input_move() with the ghost stream
       registration too. */
}

void pa_sink_input_kill(pa_sink_input*i) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));

    if (i->kill)
        i->kill(i);
}

pa_usec_t pa_sink_input_get_latency(pa_sink_input *i) {
    pa_usec_t r = 0;

    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));

    if (pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_GET_LATENCY, &r, 0, NULL) < 0)
        r = 0;

    if (i->get_latency)
        r += i->get_latency(i);

    return r;
}

/* Called from thread context */
int pa_sink_input_peek(pa_sink_input *i, size_t slength /* in sink frames */, pa_memchunk *chunk, pa_cvolume *volume) {
    pa_bool_t do_volume_adj_here;
    pa_bool_t volume_is_norm;
    size_t block_size_max_sink, block_size_max_sink_input;
    size_t ilength;

    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(slength, &i->sink->sample_spec));
    pa_assert(chunk);
    pa_assert(volume);

/*     pa_log_debug("peek"); */

    if (!i->pop)
        return -1;

    pa_assert(i->thread_info.state == PA_SINK_INPUT_RUNNING ||
              i->thread_info.state == PA_SINK_INPUT_CORKED ||
              i->thread_info.state == PA_SINK_INPUT_DRAINED);

    /* If there's still some rewrite request the handle, but the sink
    didn't do this for us, we do it here. However, since the sink
    apparently doesn't support rewinding, we pass 0 here. This still
    allows rewinding through the render buffer. */
    pa_sink_input_process_rewind(i, 0);

    block_size_max_sink_input = i->thread_info.resampler ?
        pa_resampler_max_block_size(i->thread_info.resampler) :
        pa_frame_align(pa_mempool_block_size_max(i->sink->core->mempool), &i->sample_spec);

    block_size_max_sink = pa_frame_align(pa_mempool_block_size_max(i->sink->core->mempool), &i->sink->sample_spec);

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
    volume_is_norm = pa_cvolume_is_norm(&i->thread_info.volume) && !i->thread_info.muted;

    while (!pa_memblockq_is_readable(i->thread_info.render_memblockq)) {
        pa_memchunk tchunk;

        /* There's nothing in our render queue. We need to fill it up
         * with data from the implementor. */

        if (i->thread_info.state == PA_SINK_INPUT_CORKED ||
            i->pop(i, ilength, &tchunk) < 0) {

            /* OK, we're corked or the implementor didn't give us any
             * data, so let's just hand out silence */
            pa_atomic_store(&i->thread_info.drained, 1);

            pa_memblockq_seek(i->thread_info.render_memblockq, slength, PA_SEEK_RELATIVE_ON_READ);
            i->thread_info.since_underrun = 0;
            break;
        }

        pa_atomic_store(&i->thread_info.drained, 0);

        pa_assert(tchunk.length > 0);
        pa_assert(tchunk.memblock);

        i->thread_info.since_underrun += tchunk.length;

        while (tchunk.length > 0) {
            pa_memchunk wchunk;

            wchunk = tchunk;
            pa_memblock_ref(wchunk.memblock);

            if (wchunk.length > block_size_max_sink_input)
                wchunk.length = block_size_max_sink_input;

            /* It might be necessary to adjust the volume here */
            if (do_volume_adj_here && !volume_is_norm) {
                pa_memchunk_make_writable(&wchunk, 0);

                pa_log_debug("adjusting volume!");

                if (i->thread_info.muted)
                    pa_silence_memchunk(&wchunk, &i->thread_info.sample_spec);
                else
                    pa_volume_memchunk(&wchunk, &i->thread_info.sample_spec, &i->thread_info.volume);
            }

            if (!i->thread_info.resampler)
                pa_memblockq_push_align(i->thread_info.render_memblockq, &wchunk);
            else {
                pa_memchunk rchunk;
                pa_resampler_run(i->thread_info.resampler, &wchunk, &rchunk);

                if (rchunk.memblock) {
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
        *volume = i->thread_info.volume;

    pa_atomic_store(&i->thread_info.render_memblockq_is_empty, pa_memblockq_is_empty(i->thread_info.render_memblockq));

    return 0;
}

/* Called from thread context */
void pa_sink_input_drop(pa_sink_input *i, size_t nbytes /* in sink sample spec */) {
    pa_sink_input_assert_ref(i);

    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &i->sink->sample_spec));
    pa_assert(nbytes > 0);

    /* If there's still some rewrite request the handle, but the sink
    didn't do this for us, we do it here. However, since the sink
    apparently doesn't support rewinding, we pass 0 here. This still
    allows rewinding through the render buffer. */
    if (i->thread_info.rewrite_nbytes > 0)
        pa_sink_input_process_rewind(i, 0);

    pa_memblockq_drop(i->thread_info.render_memblockq, nbytes);

    pa_atomic_store(&i->thread_info.render_memblockq_is_empty, pa_memblockq_is_empty(i->thread_info.render_memblockq));
}

/* Called from thread context */
void pa_sink_input_process_rewind(pa_sink_input *i, size_t nbytes /* in sink sample spec */) {
    pa_sink_input_assert_ref(i);

    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &i->sink->sample_spec));

/*     pa_log_debug("rewind(%lu, %lu)", (unsigned long) nbytes, (unsigned long) i->thread_info.rewrite_nbytes); */

    if (i->thread_info.ignore_rewind) {
        i->thread_info.ignore_rewind = FALSE;
        i->thread_info.rewrite_nbytes = 0;
        return;
    }

    if (nbytes > 0)
        pa_log_debug("Have to rewind %lu bytes on render memblockq.", (unsigned long) nbytes);

    if (i->thread_info.rewrite_nbytes > 0) {
        size_t max_rewrite;

        /* Calculate how much make sense to rewrite at most */
        if ((max_rewrite = nbytes + pa_memblockq_get_length(i->thread_info.render_memblockq)) > 0) {
            size_t amount, r;

            /* Transform into local domain */
            if (i->thread_info.resampler)
                max_rewrite = pa_resampler_request(i->thread_info.resampler, max_rewrite);

            /* Calculate how much of the rewinded data should actually be rewritten */
            amount = PA_MIN(max_rewrite, i->thread_info.rewrite_nbytes);

            /* Convert back to to sink domain */
            r = i->thread_info.resampler ? pa_resampler_result(i->thread_info.resampler, amount) : amount;

            if (r > 0)
                /* Ok, now update the write pointer */
                pa_memblockq_seek(i->thread_info.render_memblockq, -r, PA_SEEK_RELATIVE);

            if (amount) {
                pa_log_debug("Have to rewind %lu bytes on implementor.", (unsigned long) amount);

                /* Tell the implementor */
                if (i->process_rewind)
                    i->process_rewind(i, amount);
            }

            /* And reset the resampler */
            if (i->thread_info.resampler)
                pa_resampler_reset(i->thread_info.resampler);
        }

        i->thread_info.rewrite_nbytes = 0;
    }

    if (nbytes > 0)
        pa_memblockq_rewind(i->thread_info.render_memblockq, nbytes);
}

/* Called from thread context */
void pa_sink_input_update_max_rewind(pa_sink_input *i, size_t nbytes  /* in the sink's sample spec */) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &i->sink->sample_spec));

    pa_memblockq_set_maxrewind(i->thread_info.render_memblockq, nbytes);

    if (i->update_max_rewind)
        i->update_max_rewind(i, i->thread_info.resampler ? pa_resampler_request(i->thread_info.resampler, nbytes) : nbytes);
}

pa_usec_t pa_sink_input_set_requested_latency(pa_sink_input *i, pa_usec_t usec) {
    pa_sink_input_assert_ref(i);

    if (usec > 0) {

        if (i->sink->max_latency > 0 && usec > i->sink->max_latency)
            usec = i->sink->max_latency;

        if (i->sink->min_latency > 0 && usec < i->sink->min_latency)
            usec = i->sink->min_latency;
    }

    if (PA_SINK_INPUT_LINKED(i->state))
        pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_REQUESTED_LATENCY, NULL, (int64_t) usec, NULL, NULL);
    else {
        i->thread_info.requested_sink_latency = usec;
        i->sink->thread_info.requested_latency_valid = FALSE;
    }

    return usec;
}

void pa_sink_input_set_volume(pa_sink_input *i, const pa_cvolume *volume) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));

    if (pa_cvolume_equal(&i->volume, volume))
        return;

    i->volume = *volume;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_VOLUME, pa_xnewdup(struct pa_cvolume, volume, 1), 0, NULL, pa_xfree);
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

const pa_cvolume *pa_sink_input_get_volume(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));

    return &i->volume;
}

void pa_sink_input_set_mute(pa_sink_input *i, pa_bool_t mute) {
    pa_assert(i);
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));

    if (!i->muted == !mute)
        return;

    i->muted = mute;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_MUTE, PA_UINT_TO_PTR(mute), 0, NULL, NULL);
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

int pa_sink_input_get_mute(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));

    return !!i->muted;
}

void pa_sink_input_cork(pa_sink_input *i, pa_bool_t b) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));

    sink_input_set_state(i, b ? PA_SINK_INPUT_CORKED : PA_SINK_INPUT_RUNNING);
}

int pa_sink_input_set_rate(pa_sink_input *i, uint32_t rate) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));
    pa_return_val_if_fail(i->thread_info.resampler, -1);

    if (i->sample_spec.rate == rate)
        return 0;

    i->sample_spec.rate = rate;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_RATE, PA_UINT_TO_PTR(rate), 0, NULL, NULL);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    return 0;
}

void pa_sink_input_set_name(pa_sink_input *i, const char *name) {
    const char *old;
    pa_sink_input_assert_ref(i);

    if (!name && !pa_proplist_contains(i->proplist, PA_PROP_MEDIA_NAME))
        return;

    old = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME);

    if (old && name && !strcmp(old, name))
        return;

    if (name)
        pa_proplist_sets(i->proplist, PA_PROP_MEDIA_NAME, name);
    else
        pa_proplist_unset(i->proplist, PA_PROP_MEDIA_NAME);

    if (PA_SINK_INPUT_LINKED(i->state)) {
        pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], i);
        pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    }
}

pa_resample_method_t pa_sink_input_get_resample_method(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    return i->resample_method;
}

int pa_sink_input_move_to(pa_sink_input *i, pa_sink *dest, int immediately) {
    pa_resampler *new_resampler;
    pa_sink *origin;
    pa_usec_t silence_usec = 0;
    pa_sink_input_move_info info;
    pa_sink_input_move_hook_data hook_data;

    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->state));
    pa_sink_assert_ref(dest);

    origin = i->sink;

    if (dest == origin)
        return 0;

    if (i->flags & PA_SINK_INPUT_DONT_MOVE)
        return -1;

    if (i->sync_next || i->sync_prev) {
        pa_log_warn("Moving synchronised streams not supported.");
        return -1;
    }

    if (pa_idxset_size(dest->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn("Failed to move sink input: too many inputs per sink.");
        return -1;
    }

    if (i->thread_info.resampler &&
        pa_sample_spec_equal(&origin->sample_spec, &dest->sample_spec) &&
        pa_channel_map_equal(&origin->channel_map, &dest->channel_map))

        /* Try to reuse the old resampler if possible */
        new_resampler = i->thread_info.resampler;

    else if ((i->flags & PA_SINK_INPUT_VARIABLE_RATE) ||
             !pa_sample_spec_equal(&i->sample_spec, &dest->sample_spec) ||
             !pa_channel_map_equal(&i->channel_map, &dest->channel_map)) {

        /* Okey, we need a new resampler for the new sink */

        if (!(new_resampler = pa_resampler_new(
                      dest->core->mempool,
                      &i->sample_spec, &i->channel_map,
                      &dest->sample_spec, &dest->channel_map,
                      i->resample_method,
                      ((i->flags & PA_SINK_INPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                      ((i->flags & PA_SINK_INPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                      (i->core->disable_remixing || (i->flags & PA_SINK_INPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0)))) {
            pa_log_warn("Unsupported resampling operation.");
            return -1;
        }
    } else
        new_resampler = NULL;

    hook_data.sink_input = i;
    hook_data.destination = dest;
    pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE], &hook_data);

    memset(&info, 0, sizeof(info));
    info.sink_input = i;

    if (!immediately) {
        pa_usec_t old_latency, new_latency;

        /* Let's do a little bit of Voodoo for compensating latency
         * differences. We assume that the accuracy for our
         * estimations is still good enough, even though we do these
         * operations non-atomic. */

        old_latency = pa_sink_get_latency(origin);
        new_latency = pa_sink_get_latency(dest);

        /* The already resampled data should go to the old sink */

        if (old_latency >= new_latency) {

            /* The latency of the old sink is larger than the latency
             * of the new sink. Therefore to compensate for the
             * difference we to play silence on the new one for a
             * while */

            silence_usec = old_latency - new_latency;

        } else {

            /* The latency of new sink is larger than the latency of
             * the old sink. Therefore we have to precompute a little
             * and make sure that this is still played on the old
             * sink, until we can play the first sample on the new
             * sink.*/

            info.buffer_bytes = pa_usec_to_bytes(new_latency - old_latency, &origin->sample_spec);
        }

        /* Okey, let's move it */

        if (info.buffer_bytes > 0) {
            pa_proplist *p;

            p = pa_proplist_new();
            pa_proplist_sets(p, PA_PROP_MEDIA_NAME, "Ghost For Moved Stream");
            pa_proplist_sets(p, PA_PROP_MEDIA_ROLE, "routing");

            info.ghost_sink_input = pa_memblockq_sink_input_new(
                    origin,
                    &origin->sample_spec,
                    &origin->channel_map,
                    NULL,
                    NULL,
                    p);

            pa_proplist_free(p);

            if (info.ghost_sink_input) {
                info.ghost_sink_input->thread_info.state = info.ghost_sink_input->state = PA_SINK_INPUT_RUNNING;
                info.ghost_sink_input->thread_info.volume = info.ghost_sink_input->volume;
                info.ghost_sink_input->thread_info.muted = info.ghost_sink_input->muted;

                info.buffer = pa_memblockq_new(0, MOVE_BUFFER_LENGTH, 0, pa_frame_size(&origin->sample_spec), 0, 0, 0, NULL);
            }
        }
    }

    pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_REMOVE_INPUT_AND_BUFFER, &info, 0, NULL);

    if (info.ghost_sink_input) {
        /* Basically, do what pa_sink_input_put() does ...*/

        pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, info.ghost_sink_input->index);
        pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], info.ghost_sink_input);
        pa_sink_input_unref(info.ghost_sink_input);
    }

    pa_idxset_remove_by_data(origin->inputs, i, NULL);
    pa_idxset_put(dest->inputs, i, NULL);
    i->sink = dest;

    if (pa_sink_input_get_state(i) == PA_SINK_INPUT_CORKED) {
        pa_assert_se(origin->n_corked-- >= 1);
        dest->n_corked++;
    }

    /* Replace resampler */
    if (new_resampler != i->thread_info.resampler) {
        pa_memchunk silence;

        if (i->thread_info.resampler)
            pa_resampler_free(i->thread_info.resampler);
        i->thread_info.resampler = new_resampler;

        /* if the resampler changed, the silence memblock is
         * probably invalid now, too */

        pa_silence_memchunk_get(
                &i->sink->core->silence_cache,
                i->sink->core->mempool,
                &silence,
                &dest->sample_spec,
                0);

        pa_memblockq_set_silence(i->thread_info.render_memblockq, &silence);
        pa_memblock_unref(silence.memblock);

    }

    pa_memblockq_flush(i->thread_info.render_memblockq);

    /* Calculate the new sleeping time */
    if (!immediately)
        pa_memblockq_seek(i->thread_info.render_memblockq, pa_usec_to_bytes(silence_usec, &dest->sample_spec), PA_SEEK_RELATIVE);

    pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_ADD_INPUT, i, 0, NULL);

    pa_sink_update_status(origin);
    pa_sink_update_status(dest);

    if (i->moved)
        i->moved(i);

    pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_POST], i);

    pa_log_debug("Successfully moved sink input %i from %s to %s.", i->index, origin->name, dest->name);

    /* Notify everyone */
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);

    return 0;
}

static void set_state(pa_sink_input *i, pa_sink_input_state_t state) {
    pa_sink_input_assert_ref(i);

    if ((state == PA_SINK_INPUT_DRAINED || state == PA_SINK_INPUT_RUNNING) &&
        !(i->thread_info.state == PA_SINK_INPUT_DRAINED || i->thread_info.state != PA_SINK_INPUT_RUNNING))
        pa_atomic_store(&i->thread_info.drained, 1);

    if (state == PA_SINK_INPUT_CORKED && i->thread_info.state != PA_SINK_INPUT_CORKED) {

        /* OK, we're corked, so let's make sure we have total silence
         * from now on on this stream */
        pa_memblockq_silence(i->thread_info.render_memblockq);

        /* This will tell the implementing sink input driver to rewind
         * so that the unplayed already mixed data is not lost */
        pa_sink_input_request_rewind(i, 0, FALSE);

    } else if (i->thread_info.state == PA_SINK_INPUT_CORKED && state != PA_SINK_INPUT_CORKED) {

        /* OK, we're being uncorked. Make sure we're not rewound when
         * the hw buffer is remixed and request a remix. */
        i->thread_info.ignore_rewind = TRUE;
        i->thread_info.since_underrun = 0;
        pa_sink_request_rewind(i->sink, 0);
    }

    i->thread_info.state = state;
}

/* Called from thread context */
int pa_sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink_input *i = PA_SINK_INPUT(o);

    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));

    switch (code) {
        case PA_SINK_INPUT_MESSAGE_SET_VOLUME:
            i->thread_info.volume = *((pa_cvolume*) userdata);
            pa_sink_input_request_rewind(i, 0, FALSE);
            return 0;

        case PA_SINK_INPUT_MESSAGE_SET_MUTE:
            i->thread_info.muted = PA_PTR_TO_UINT(userdata);
            pa_sink_input_request_rewind(i, 0, FALSE);
            return 0;

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            *r += pa_bytes_to_usec(pa_memblockq_get_length(i->thread_info.render_memblockq), &i->sink->sample_spec);

            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_SET_RATE:

            i->thread_info.sample_spec.rate = PA_PTR_TO_UINT(userdata);
            pa_resampler_set_input_rate(i->thread_info.resampler, PA_PTR_TO_UINT(userdata));

            return 0;

        case PA_SINK_INPUT_MESSAGE_SET_STATE: {
            pa_sink_input *ssync;

            set_state(i, PA_PTR_TO_UINT(userdata));

            for (ssync = i->thread_info.sync_prev; ssync; ssync = ssync->thread_info.sync_prev)
                set_state(ssync, PA_PTR_TO_UINT(userdata));

            for (ssync = i->thread_info.sync_next; ssync; ssync = ssync->thread_info.sync_next)
                set_state(ssync, PA_PTR_TO_UINT(userdata));

            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_SET_REQUESTED_LATENCY:

            i->thread_info.requested_sink_latency = (pa_usec_t) offset;
            pa_sink_invalidate_requested_latency(i->sink);

            return 0;
    }

    return -1;
}

pa_sink_input_state_t pa_sink_input_get_state(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    if (i->state == PA_SINK_INPUT_RUNNING || i->state == PA_SINK_INPUT_DRAINED)
        return pa_atomic_load(&i->thread_info.drained) ? PA_SINK_INPUT_DRAINED : PA_SINK_INPUT_RUNNING;

    return i->state;
}

pa_bool_t pa_sink_input_safe_to_remove(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    if (i->state == PA_SINK_INPUT_RUNNING || i->state == PA_SINK_INPUT_DRAINED || i->state == PA_SINK_INPUT_CORKED)
        return pa_atomic_load(&i->thread_info.render_memblockq_is_empty);

    return TRUE;
}

void pa_sink_input_request_rewind(pa_sink_input *i, size_t nbytes  /* in our sample spec */, pa_bool_t ignore_underruns) {
    size_t l, lbq;

    pa_sink_input_assert_ref(i);

    /* We don't take rewind requests while we are corked */
    if (i->state == PA_SINK_INPUT_CORKED)
        return;

    lbq = pa_memblockq_get_length(i->thread_info.render_memblockq);

    if (nbytes <= 0) {
        /* Calulate maximum number of bytes that could be rewound in theory */
        nbytes = i->sink->thread_info.max_rewind + lbq;

        /* Transform from sink domain */
        nbytes =
            i->thread_info.resampler ?
            pa_resampler_request(i->thread_info.resampler, nbytes) :
            nbytes;
    }

    /* Increase the number of bytes to rewrite, never decrease */
    if (nbytes > i->thread_info.rewrite_nbytes)
        i->thread_info.rewrite_nbytes = nbytes;

    if (!ignore_underruns) {
        /* Make sure to not overwrite over underruns */
        if ((int64_t) i->thread_info.rewrite_nbytes > i->thread_info.since_underrun)
            i->thread_info.rewrite_nbytes = (size_t) i->thread_info.since_underrun;
    }

    /* Transform to sink domain */
    l = i->thread_info.resampler ?
        pa_resampler_result(i->thread_info.resampler, i->thread_info.rewrite_nbytes) :
        i->thread_info.rewrite_nbytes;

    if (l <= 0)
        return;

    if (l > lbq)
        pa_sink_request_rewind(i->sink, l - lbq);
}

pa_memchunk* pa_sink_input_get_silence(pa_sink_input *i, pa_memchunk *ret) {
    pa_sink_input_assert_ref(i);
    pa_assert(ret);

    pa_silence_memchunk_get(
                &i->sink->core->silence_cache,
                i->sink->core->mempool,
                ret,
                &i->sample_spec,
                i->thread_info.resampler ? pa_resampler_max_block_size(i->thread_info.resampler) : 0);

    return ret;
}
