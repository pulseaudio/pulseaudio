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

#include "sink-input.h"

#define CONVERT_BUFFER_LENGTH (PA_PAGE_SIZE)
#define SILENCE_BUFFER_LENGTH (PA_PAGE_SIZE*12)
#define MOVE_BUFFER_LENGTH (PA_PAGE_SIZE*256)

static PA_DEFINE_CHECK_TYPE(pa_sink_input, pa_msgobject);

static void sink_input_free(pa_object *o);

pa_sink_input_new_data* pa_sink_input_new_data_init(pa_sink_input_new_data *data) {
    pa_assert(data);

    memset(data, 0, sizeof(*data));
    data->resample_method = PA_RESAMPLER_INVALID;

    return data;
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

void pa_sink_input_new_data_set_sample_spec(pa_sink_input_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

void pa_sink_input_new_data_set_muted(pa_sink_input_new_data *data, pa_bool_t mute) {
    pa_assert(data);

    data->muted_is_set = TRUE;
    data->muted = !!mute;
}

pa_sink_input* pa_sink_input_new(
        pa_core *core,
        pa_sink_input_new_data *data,
        pa_sink_input_flags_t flags) {

    pa_sink_input *i;
    pa_resampler *resampler = NULL;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX];

    pa_assert(core);
    pa_assert(data);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], data) < 0)
        return NULL;

    pa_return_null_if_fail(!data->driver || pa_utf8_valid(data->driver));
    pa_return_null_if_fail(!data->name || pa_utf8_valid(data->name));

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
            pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
    }

    pa_return_null_if_fail(pa_channel_map_valid(&data->channel_map));
    pa_return_null_if_fail(data->channel_map.channels == data->sample_spec.channels);

    if (!data->volume_is_set)
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);

    pa_return_null_if_fail(pa_cvolume_valid(&data->volume));
    pa_return_null_if_fail(data->volume.channels == data->sample_spec.channels);

    if (!data->muted_is_set)
        data->muted = 0;

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    pa_return_null_if_fail(data->resample_method < PA_RESAMPLER_MAX);

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
                      !!(flags & PA_SINK_INPUT_VARIABLE_RATE)))) {
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
    i->name = pa_xstrdup(data->name);
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

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->attach = NULL;
    i->detach = NULL;
    i->suspend = NULL;
    i->userdata = NULL;

    i->thread_info.state = i->state;
    pa_atomic_store(&i->thread_info.drained, 1);
    i->thread_info.sample_spec = i->sample_spec;
    i->thread_info.silence_memblock = NULL;
    i->thread_info.move_silence = 0;
    pa_memchunk_reset(&i->thread_info.resampled_chunk);
    i->thread_info.resampler = resampler;
    i->thread_info.volume = i->volume;
    i->thread_info.muted = i->muted;
    i->thread_info.attached = FALSE;

    pa_assert_se(pa_idxset_put(core->sink_inputs, pa_sink_input_ref(i), &i->index) == 0);
    pa_assert_se(pa_idxset_put(i->sink->inputs, i, NULL) == 0);

    pa_log_info("Created input %u \"%s\" on %s with sample spec %s",
                i->index,
                i->name,
                i->sink->name,
                pa_sample_spec_snprint(st, sizeof(st), &i->sample_spec));

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

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->attach = NULL;
    i->detach = NULL;
    i->suspend = NULL;

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

    pa_log_info("Freeing output %u \"%s\"", i->index, i->name);

    pa_assert(!i->thread_info.attached);

    if (i->thread_info.resampled_chunk.memblock)
        pa_memblock_unref(i->thread_info.resampled_chunk.memblock);

    if (i->thread_info.resampler)
        pa_resampler_free(i->thread_info.resampler);

    if (i->thread_info.silence_memblock)
        pa_memblock_unref(i->thread_info.silence_memblock);

    pa_xfree(i->name);
    pa_xfree(i->driver);
    pa_xfree(i);
}

void pa_sink_input_put(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    pa_assert(i->state == PA_SINK_INPUT_INIT);
    pa_assert(i->peek);
    pa_assert(i->drop);

    i->thread_info.state = i->state = i->flags & PA_SINK_INPUT_START_CORKED ? PA_SINK_INPUT_CORKED : PA_SINK_INPUT_RUNNING;
    i->thread_info.volume = i->volume;
    i->thread_info.muted = i->muted;

    if (i->state == PA_SINK_INPUT_CORKED)
        i->sink->n_corked++;

    pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_ADD_INPUT, i, 0, NULL);
    pa_sink_update_status(i->sink);

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
int pa_sink_input_peek(pa_sink_input *i, size_t length, pa_memchunk *chunk, pa_cvolume *volume) {
    int ret = -1;
    int do_volume_adj_here;
    int volume_is_norm;
    size_t block_size_max;

    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(length, &i->sink->sample_spec));
    pa_assert(chunk);
    pa_assert(volume);

    if (!i->peek || !i->drop || i->thread_info.state == PA_SINK_INPUT_CORKED)
        goto finish;

    pa_assert(i->thread_info.state == PA_SINK_INPUT_RUNNING || i->thread_info.state == PA_SINK_INPUT_DRAINED);

    /* Default buffer size */
    if (length <= 0)
        length = pa_frame_align(CONVERT_BUFFER_LENGTH, &i->sink->sample_spec);

    /* Make sure the buffer fits in the mempool tile */
    block_size_max = pa_mempool_block_size_max(i->sink->core->mempool);
    if (length > block_size_max)
        length = pa_frame_align(block_size_max, &i->sink->sample_spec);

    if (i->thread_info.move_silence > 0) {
        size_t l;

        /* We have just been moved and shall play some silence for a
         * while until the old sink has drained its playback buffer */

        if (!i->thread_info.silence_memblock)
            i->thread_info.silence_memblock = pa_silence_memblock_new(
                    i->sink->core->mempool,
                    &i->sink->sample_spec,
                    pa_frame_align(SILENCE_BUFFER_LENGTH, &i->sink->sample_spec));

        chunk->memblock = pa_memblock_ref(i->thread_info.silence_memblock);
        chunk->index = 0;
        l = pa_memblock_get_length(chunk->memblock);
        chunk->length = i->thread_info.move_silence < l ? i->thread_info.move_silence : l;

        ret = 0;
        do_volume_adj_here = 1;
        goto finish;
    }

    if (!i->thread_info.resampler) {
        do_volume_adj_here = 0; /* FIXME??? */
        ret = i->peek(i, length, chunk);
        goto finish;
    }

    do_volume_adj_here = !pa_channel_map_equal(&i->channel_map, &i->sink->channel_map);
    volume_is_norm = pa_cvolume_is_norm(&i->thread_info.volume) && !i->thread_info.muted;

    while (!i->thread_info.resampled_chunk.memblock) {
        pa_memchunk tchunk;
        size_t l, rmbs;

        l = pa_resampler_request(i->thread_info.resampler, length);

        if (l <= 0)
            l = pa_frame_align(CONVERT_BUFFER_LENGTH, &i->sample_spec);

        rmbs = pa_resampler_max_block_size(i->thread_info.resampler);
        if (l > rmbs)
            l = rmbs;

        if ((ret = i->peek(i, l, &tchunk)) < 0)
            goto finish;

        pa_assert(tchunk.length > 0);

        if (tchunk.length > l)
            tchunk.length = l;

        i->drop(i, tchunk.length);

        /* It might be necessary to adjust the volume here */
        if (do_volume_adj_here && !volume_is_norm) {
            pa_memchunk_make_writable(&tchunk, 0);

            if (i->thread_info.muted)
                pa_silence_memchunk(&tchunk, &i->thread_info.sample_spec);
            else
                pa_volume_memchunk(&tchunk, &i->thread_info.sample_spec, &i->thread_info.volume);
        }

        pa_resampler_run(i->thread_info.resampler, &tchunk, &i->thread_info.resampled_chunk);
        pa_memblock_unref(tchunk.memblock);
    }

    pa_assert(i->thread_info.resampled_chunk.memblock);
    pa_assert(i->thread_info.resampled_chunk.length > 0);

    *chunk = i->thread_info.resampled_chunk;
    pa_memblock_ref(i->thread_info.resampled_chunk.memblock);

    ret = 0;

finish:

    if (ret >= 0)
        pa_atomic_store(&i->thread_info.drained, 0);
    else if (ret < 0)
        pa_atomic_store(&i->thread_info.drained, 1);

    if (ret >= 0) {
        /* Let's see if we had to apply the volume adjustment
         * ourselves, or if this can be done by the sink for us */

        if (do_volume_adj_here)
            /* We had different channel maps, so we already did the adjustment */
            pa_cvolume_reset(volume, i->sink->sample_spec.channels);
        else if (i->thread_info.muted)
            /* We've both the same channel map, so let's have the sink do the adjustment for us*/
            pa_cvolume_mute(volume, i->sink->sample_spec.channels);
        else
            *volume = i->thread_info.volume;
    }

    return ret;
}

/* Called from thread context */
void pa_sink_input_drop(pa_sink_input *i, size_t length) {
    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));
    pa_assert(pa_frame_aligned(length, &i->sink->sample_spec));
    pa_assert(length > 0);

    if (!i->peek || !i->drop || i->thread_info.state == PA_SINK_INPUT_CORKED)
        return;

    if (i->thread_info.move_silence > 0) {

        if (i->thread_info.move_silence >= length) {
            i->thread_info.move_silence -= length;
            length = 0;
        } else {
            length -= i->thread_info.move_silence;
            i->thread_info.move_silence = 0;
        }

        if (i->thread_info.move_silence <= 0) {
            if (i->thread_info.silence_memblock) {
                pa_memblock_unref(i->thread_info.silence_memblock);
                i->thread_info.silence_memblock = NULL;
            }
        }

        if (length <= 0)
            return;
    }

    if (i->thread_info.resampled_chunk.memblock) {
        size_t l = length;

        if (l > i->thread_info.resampled_chunk.length)
            l = i->thread_info.resampled_chunk.length;

        i->thread_info.resampled_chunk.index += l;
        i->thread_info.resampled_chunk.length -= l;

        if (i->thread_info.resampled_chunk.length <= 0) {
            pa_memblock_unref(i->thread_info.resampled_chunk.memblock);
            pa_memchunk_reset(&i->thread_info.resampled_chunk);
        }

        length -= l;
    }

    if (length > 0) {

        if (i->thread_info.resampler) {
            /* So, we have a resampler. To avoid discontinuities we
             * have to actually read all data that could be read and
             * pass it through the resampler. */

            while (length > 0) {
                pa_memchunk chunk;
                pa_cvolume volume;

                if (pa_sink_input_peek(i, length, &chunk, &volume) >= 0) {
                    size_t l;

                    pa_memblock_unref(chunk.memblock);

                    l = chunk.length;
                    if (l > length)
                        l = length;

                    pa_sink_input_drop(i, l);
                    length -= l;

                } else {
                    size_t l;

                    l = pa_resampler_request(i->thread_info.resampler, length);

                    /* Hmmm, peeking failed, so let's at least drop
                     * the right amount of data */

                    if (l > 0)
                        if (i->drop)
                            i->drop(i, l);

                    break;
                }
            }

        } else {

            /* We have no resampler, hence let's just drop the data */

            if (i->drop)
                i->drop(i, length);
        }
    }
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
    pa_sink_input_assert_ref(i);

    if (!i->name && !name)
        return;

    if (i->name && name && !strcmp(i->name, name))
        return;

    pa_xfree(i->name);
    i->name = pa_xstrdup(name);

    if (PA_SINK_INPUT_LINKED(i->state)) {
        pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_NAME_CHANGED], i);
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
                      !!(i->flags & PA_SINK_INPUT_VARIABLE_RATE)))) {
            pa_log_warn("Unsupported resampling operation.");
            return -1;
        }
    } else
        new_resampler = NULL;

    pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE], i);

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

            info.ghost_sink_input = pa_memblockq_sink_input_new(
                    origin,
                    "Ghost Stream",
                    &origin->sample_spec,
                    &origin->channel_map,
                    NULL,
                    NULL);

            info.ghost_sink_input->thread_info.state = info.ghost_sink_input->state = PA_SINK_INPUT_RUNNING;
            info.ghost_sink_input->thread_info.volume = info.ghost_sink_input->volume;
            info.ghost_sink_input->thread_info.muted = info.ghost_sink_input->muted;

            info.buffer = pa_memblockq_new(0, MOVE_BUFFER_LENGTH, 0, pa_frame_size(&origin->sample_spec), 0, 0, NULL);
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
        if (i->thread_info.resampler)
            pa_resampler_free(i->thread_info.resampler);
        i->thread_info.resampler = new_resampler;

        /* if the resampler changed, the silence memblock is
         * probably invalid now, too */
        if (i->thread_info.silence_memblock) {
            pa_memblock_unref(i->thread_info.silence_memblock);
            i->thread_info.silence_memblock = NULL;
        }
    }

    /* Dump already resampled data */
    if (i->thread_info.resampled_chunk.memblock) {
        /* Hmm, this data has already been added to the ghost queue, presumably, hence let's sleep a little bit longer */
        silence_usec += pa_bytes_to_usec(i->thread_info.resampled_chunk.length, &origin->sample_spec);
        pa_memblock_unref(i->thread_info.resampled_chunk.memblock);
        pa_memchunk_reset(&i->thread_info.resampled_chunk);
    }

    /* Calculate the new sleeping time */
    if (immediately)
        i->thread_info.move_silence = 0;
    else
        i->thread_info.move_silence = pa_usec_to_bytes(
                pa_bytes_to_usec(i->thread_info.move_silence, &origin->sample_spec) +
                silence_usec,
                &dest->sample_spec);

    pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_ADD_INPUT, i, 0, NULL);

    pa_sink_update_status(origin);
    pa_sink_update_status(dest);

    pa_hook_fire(&i->sink->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_POST], i);

    pa_log_debug("Successfully moved sink input %i from %s to %s.", i->index, origin->name, dest->name);

    /* Notify everyone */
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);

    return 0;
}

/* Called from thread context */
int pa_sink_input_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink_input *i = PA_SINK_INPUT(o);

    pa_sink_input_assert_ref(i);
    pa_assert(PA_SINK_INPUT_LINKED(i->thread_info.state));

    switch (code) {
        case PA_SINK_INPUT_MESSAGE_SET_VOLUME:
            i->thread_info.volume = *((pa_cvolume*) userdata);
            return 0;

        case PA_SINK_INPUT_MESSAGE_SET_MUTE:
            i->thread_info.muted = PA_PTR_TO_UINT(userdata);
            return 0;

        case PA_SINK_INPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            if (i->thread_info.resampled_chunk.memblock)
                *r += pa_bytes_to_usec(i->thread_info.resampled_chunk.length, &i->sink->sample_spec);

            if (i->thread_info.move_silence)
                *r += pa_bytes_to_usec(i->thread_info.move_silence, &i->sink->sample_spec);

            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_SET_RATE:

            i->thread_info.sample_spec.rate = PA_PTR_TO_UINT(userdata);
            pa_resampler_set_input_rate(i->thread_info.resampler, PA_PTR_TO_UINT(userdata));

            return 0;

        case PA_SINK_INPUT_MESSAGE_SET_STATE: {
            pa_sink_input *ssync;

            if ((PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_DRAINED || PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_RUNNING) &&
                (i->thread_info.state != PA_SINK_INPUT_DRAINED) && (i->thread_info.state != PA_SINK_INPUT_RUNNING))
                pa_atomic_store(&i->thread_info.drained, 1);

            i->thread_info.state = PA_PTR_TO_UINT(userdata);

            for (ssync = i->thread_info.sync_prev; ssync; ssync = ssync->thread_info.sync_prev) {
                if ((PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_DRAINED || PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_RUNNING) &&
                    (ssync->thread_info.state != PA_SINK_INPUT_DRAINED) && (ssync->thread_info.state != PA_SINK_INPUT_RUNNING))
                    pa_atomic_store(&ssync->thread_info.drained, 1);
                ssync->thread_info.state = PA_PTR_TO_UINT(userdata);
            }

            for (ssync = i->thread_info.sync_next; ssync; ssync = ssync->thread_info.sync_next) {
                if ((PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_DRAINED || PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_RUNNING) &&
                    (ssync->thread_info.state != PA_SINK_INPUT_DRAINED) && (ssync->thread_info.state != PA_SINK_INPUT_RUNNING))
                    pa_atomic_store(&ssync->thread_info.drained, 1);
                ssync->thread_info.state = PA_PTR_TO_UINT(userdata);
            }

            return 0;
        }
    }

    return -1;
}

pa_sink_input_state_t pa_sink_input_get_state(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    if (i->state == PA_SINK_INPUT_RUNNING || i->state == PA_SINK_INPUT_DRAINED)
        return pa_atomic_load(&i->thread_info.drained) ? PA_SINK_INPUT_DRAINED : PA_SINK_INPUT_RUNNING;

    return i->state;
}
