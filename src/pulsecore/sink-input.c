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

#define CONVERT_BUFFER_LENGTH 4096
#define MOVE_BUFFER_LENGTH (1024*1024)
#define SILENCE_BUFFER_LENGTH (64*1024)

static PA_DEFINE_CHECK_TYPE(pa_sink_input, sink_input_check_type, pa_msgobject_check_type);

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

void pa_sink_input_new_data_set_muted(pa_sink_input_new_data *data, int mute) {
    pa_assert(data);

    data->muted_is_set = 1;
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

    if (!(flags & PA_SINK_INPUT_NO_HOOKS))
        if (pa_hook_fire(&core->hook_sink_input_new, data) < 0)
            return NULL;

    pa_return_null_if_fail(!data->driver || pa_utf8_valid(data->driver));
    pa_return_null_if_fail(!data->name || pa_utf8_valid(data->name));

    if (!data->sink)
        data->sink = pa_namereg_get(core, NULL, PA_NAMEREG_SINK, 1);

    pa_return_null_if_fail(data->sink);
    pa_return_null_if_fail(pa_sink_get_state(data->sink) != PA_SINK_DISCONNECTED);

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
                      data->resample_method))) {
            pa_log_warn("Unsupported resampling operation.");
            return NULL;
        }

        data->resample_method = pa_resampler_get_method(resampler);
    }

    i = pa_msgobject_new(pa_sink_input, sink_input_check_type);
    i->parent.parent.free = sink_input_free;
    i->parent.process_msg = pa_sink_input_process_msg;

    i->core = core;
    i->state = PA_SINK_INPUT_RUNNING;
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

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->underrun = NULL;
    i->userdata = NULL;

    i->thread_info.state = i->state;
    pa_atomic_store(&i->thread_info.drained, 1);
    i->thread_info.sample_spec = i->sample_spec;
    i->thread_info.silence_memblock = NULL;
/*     i->thread_info.move_silence = 0; */
    pa_memchunk_reset(&i->thread_info.resampled_chunk);
    i->thread_info.resampler = resampler;
    i->thread_info.volume = i->volume;
    i->thread_info.muted = i->muted;

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

static int sink_input_set_state(pa_sink_input *i, pa_sink_input_state_t state) {
    pa_assert(i);

    if (state == PA_SINK_INPUT_DRAINED)
        state = PA_SINK_INPUT_RUNNING;

    if (i->state == state)
        return 0;

    if (pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), NULL) < 0)
        return -1;

    i->state = state;
    return 0;
}

void pa_sink_input_disconnect(pa_sink_input *i) {
    pa_assert(i);
    pa_return_if_fail(i->state != PA_SINK_INPUT_DISCONNECTED);

    pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_REMOVE_INPUT, i, NULL);
    pa_idxset_remove_by_data(i->sink->core->sink_inputs, i, NULL);
    pa_idxset_remove_by_data(i->sink->inputs, i, NULL);
    pa_sink_input_unref(i);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_REMOVE, i->index);

    sink_input_set_state(i, PA_SINK_INPUT_DISCONNECTED);
    pa_sink_update_status(i->sink);
    
    i->sink = NULL;
    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->underrun = NULL;
}

static void sink_input_free(pa_object *o) {
    pa_sink_input* i = PA_SINK_INPUT(o);

    pa_assert(i);
    pa_assert(pa_sink_input_refcnt(i) == 0);

    if (i->state != PA_SINK_INPUT_DISCONNECTED)
        pa_sink_input_disconnect(i);

    pa_log_info("Freeing output %u \"%s\"", i->index, i->name);

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

    i->thread_info.volume = i->volume;
    i->thread_info.muted = i->muted;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i->sink), PA_SINK_MESSAGE_ADD_INPUT, pa_sink_input_ref(i), NULL, (pa_free_cb_t) pa_sink_input_unref);
    pa_sink_update_status(i->sink);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, i->index);
}

void pa_sink_input_kill(pa_sink_input*i) {
    pa_sink_input_assert_ref(i);

    if (i->kill)
        i->kill(i);
}

pa_usec_t pa_sink_input_get_latency(pa_sink_input *i) {
    pa_usec_t r = 0;

    pa_sink_input_assert_ref(i);

    if (pa_asyncmsgq_send(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_GET_LATENCY, &r, NULL) < 0)
        r = 0;

    if (i->get_latency)
        r += i->get_latency(i);

    return r;
}

int pa_sink_input_peek(pa_sink_input *i, pa_memchunk *chunk, pa_cvolume *volume) {
    int ret = -1;
    int do_volume_adj_here;
    int volume_is_norm;

    pa_sink_input_assert_ref(i);
    pa_assert(chunk);
    pa_assert(volume);

    if (!i->peek || !i->drop || i->thread_info.state == PA_SINK_INPUT_DISCONNECTED || i->thread_info.state == PA_SINK_INPUT_CORKED)
        goto finish;

    pa_assert(i->thread_info.state == PA_SINK_INPUT_RUNNING || i->thread_info.state == PA_SINK_INPUT_DRAINED);

/*     if (i->thread_info.move_silence > 0) { */
/*         size_t l; */

/*         /\* We have just been moved and shall play some silence for a */
/*          * while until the old sink has drained its playback buffer *\/ */

/*         if (!i->thread_info.silence_memblock) */
/*             i->thread_info.silence_memblock = pa_silence_memblock_new(i->sink->core->mempool, &i->sink->sample_spec, SILENCE_BUFFER_LENGTH); */

/*         chunk->memblock = pa_memblock_ref(i->thread_info.silence_memblock); */
/*         chunk->index = 0; */
/*         l = pa_memblock_get_length(chunk->memblock); */
/*         chunk->length = i->move_silence < l ? i->move_silence : l; */

/*         ret = 0; */
/*         do_volume_adj_here = 1; */
/*         goto finish; */
/*     } */

    if (!i->thread_info.resampler) {
        do_volume_adj_here = 0; /* FIXME??? */
        ret = i->peek(i, chunk);
        goto finish;
    }

    do_volume_adj_here = !pa_channel_map_equal(&i->channel_map, &i->sink->channel_map);
    volume_is_norm = pa_cvolume_is_norm(&i->thread_info.volume) && !i->thread_info.muted;

    while (!i->thread_info.resampled_chunk.memblock) {
        pa_memchunk tchunk;
        size_t l;

        if ((ret = i->peek(i, &tchunk)) < 0)
            goto finish;

        pa_assert(tchunk.length > 0);

        l = pa_resampler_request(i->thread_info.resampler, CONVERT_BUFFER_LENGTH);

        if (tchunk.length > l)
            tchunk.length = l;

        i->drop(i, tchunk.length);

        /* It might be necessary to adjust the volume here */
        if (do_volume_adj_here && !volume_is_norm) {
            pa_memchunk_make_writable(&tchunk, 0);
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

    if (ret < 0 && !pa_atomic_load(&i->thread_info.drained) && i->underrun)
        i->underrun(i);

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
        else
            /* We've both the same channel map, so let's have the sink do the adjustment for us*/
            *volume = i->thread_info.volume;
    }

    return ret;
}

void pa_sink_input_drop(pa_sink_input *i, size_t length) {
    pa_sink_input_assert_ref(i);
    pa_assert(length > 0);

/*     if (i->move_silence > 0) { */

/*         if (chunk) { */
/*             size_t l; */

/*             l = pa_memblock_get_length(i->silence_memblock); */

/*             if (chunk->memblock != i->silence_memblock || */
/*                 chunk->index != 0 || */
/*                 (chunk->memblock && (chunk->length != (l < i->move_silence ? l : i->move_silence)))) */
/*                 return; */

/*         } */

/*         pa_assert(i->move_silence >= length); */

/*         i->move_silence -= length; */

/*         if (i->move_silence <= 0) { */
/*             pa_assert(i->silence_memblock); */
/*             pa_memblock_unref(i->silence_memblock); */
/*             i->silence_memblock = NULL; */
/*         } */

/*         return; */
/*     } */

    pa_log("dropping %u", length);
    
    if (i->thread_info.resampled_chunk.memblock) {
        size_t l = length;

        if (l > i->thread_info.resampled_chunk.length)
            l = i->thread_info.resampled_chunk.length;

        pa_log("really dropping %u", l);
        
        i->thread_info.resampled_chunk.index += l;
        i->thread_info.resampled_chunk.length -= l;
        
        if (i->thread_info.resampled_chunk.length <= 0) {
            pa_memblock_unref(i->thread_info.resampled_chunk.memblock);
            pa_memchunk_reset(&i->thread_info.resampled_chunk);
        }

        length -= l;
    }

    pa_log("really remaining %u", length);
    
    if (length > 0) {
        
        if (i->thread_info.resampler) {
            /* So, we have a resampler. To avoid discontinuities we
             * have to actually read all data that could be read and
             * pass it through the resampler. */

            while (length > 0) {
                pa_memchunk chunk;
                pa_cvolume volume;
                
                if (pa_sink_input_peek(i, &chunk, &volume) >= 0) {
                    size_t l = chunk.length;

                    if (l > length)
                        l = length;
                    
                    pa_sink_input_drop(i, l);
                    length -= l;
                    
                } else {
                    /* Hmmm, peeking failed, so let's at least drop
                     * the right amount of data */
                    
                    if (i->drop)
                        i->drop(i, pa_resampler_request(i->thread_info.resampler, length));
                            
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

    if (pa_cvolume_equal(&i->volume, volume))
        return;

    i->volume = *volume;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_VOLUME, pa_xnewdup(struct pa_cvolume, volume, 1), NULL, pa_xfree);
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

const pa_cvolume *pa_sink_input_get_volume(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    return &i->volume;
}

void pa_sink_input_set_mute(pa_sink_input *i, int mute) {
    pa_assert(i);
    pa_sink_input_assert_ref(i);

    if (!i->muted == !mute)
        return;

    i->muted = mute;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_MUTE, PA_UINT_TO_PTR(mute), NULL, NULL);
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

int pa_sink_input_get_mute(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    return !!i->muted;
}

void pa_sink_input_cork(pa_sink_input *i, int b) {
    pa_sink_input_assert_ref(i);

    sink_input_set_state(i, b ? PA_SINK_INPUT_CORKED : PA_SINK_INPUT_RUNNING);
}

int pa_sink_input_set_rate(pa_sink_input *i, uint32_t rate) {
    pa_sink_input_assert_ref(i);
    pa_return_val_if_fail(i->thread_info.resampler, -1);

    if (i->sample_spec.rate == rate)
        return 0;

    i->sample_spec.rate = rate;

    pa_asyncmsgq_post(i->sink->asyncmsgq, PA_MSGOBJECT(i), PA_SINK_INPUT_MESSAGE_SET_RATE, PA_UINT_TO_PTR(rate), NULL, NULL);

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

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

pa_resample_method_t pa_sink_input_get_resample_method(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    return i->resample_method;
}

int pa_sink_input_move_to(pa_sink_input *i, pa_sink *dest, int immediately) {
/*     pa_resampler *new_resampler = NULL; */
/*     pa_memblockq *buffer = NULL; */
/*     pa_sink *origin; */

    pa_sink_input_assert_ref(i);
    pa_sink_assert_ref(dest);

    return -1;

/*     origin = i->sink; */

/*     if (dest == origin) */
/*         return 0; */

/*     if (pa_idxset_size(dest->inputs) >= PA_MAX_INPUTS_PER_SINK) { */
/*         pa_log_warn("Failed to move sink input: too many inputs per sink."); */
/*         return -1; */
/*     } */

/*     if (i->resampler && */
/*         pa_sample_spec_equal(&origin->sample_spec, &dest->sample_spec) && */
/*         pa_channel_map_equal(&origin->channel_map, &dest->channel_map)) */

/*         /\* Try to reuse the old resampler if possible *\/ */
/*         new_resampler = i->resampler; */

/*     else if ((i->flags & PA_SINK_INPUT_VARIABLE_RATE) || */
/*         !pa_sample_spec_equal(&i->sample_spec, &dest->sample_spec) || */
/*         !pa_channel_map_equal(&i->channel_map, &dest->channel_map)) { */

/*         /\* Okey, we need a new resampler for the new sink *\/ */

/*         if (!(new_resampler = pa_resampler_new( */
/*                       dest->core->mempool, */
/*                       &i->sample_spec, &i->channel_map, */
/*                       &dest->sample_spec, &dest->channel_map, */
/*                       i->resample_method))) { */
/*             pa_log_warn("Unsupported resampling operation."); */
/*             return -1; */
/*         } */
/*     } */

/*     if (!immediately) { */
/*         pa_usec_t old_latency, new_latency; */
/*         pa_usec_t silence_usec = 0; */

/*         buffer = pa_memblockq_new(0, MOVE_BUFFER_LENGTH, 0, pa_frame_size(&origin->sample_spec), 0, 0, NULL); */

/*         /\* Let's do a little bit of Voodoo for compensating latency */
/*          * differences *\/ */

/*         old_latency = pa_sink_get_latency(origin); */
/*         new_latency = pa_sink_get_latency(dest); */

/*         /\* The already resampled data should go to the old sink *\/ */

/*         if (old_latency >= new_latency) { */

/*             /\* The latency of the old sink is larger than the latency */
/*              * of the new sink. Therefore to compensate for the */
/*              * difference we to play silence on the new one for a */
/*              * while *\/ */

/*             silence_usec = old_latency - new_latency; */

/*         } else { */
/*             size_t l; */
/*             int volume_is_norm; */

/*             /\* The latency of new sink is larger than the latency of */
/*              * the old sink. Therefore we have to precompute a little */
/*              * and make sure that this is still played on the old */
/*              * sink, until we can play the first sample on the new */
/*              * sink.*\/ */

/*             l = pa_usec_to_bytes(new_latency - old_latency, &origin->sample_spec); */

/*             volume_is_norm = pa_cvolume_is_norm(&i->volume); */

/*             while (l > 0) { */
/*                 pa_memchunk chunk; */
/*                 pa_cvolume volume; */
/*                 size_t n; */

/*                 if (pa_sink_input_peek(i, &chunk, &volume) < 0) */
/*                     break; */

/*                 n = chunk.length > l ? l : chunk.length; */
/*                 pa_sink_input_drop(i, &chunk, n); */
/*                 chunk.length = n; */

/*                 if (!volume_is_norm) { */
/*                     pa_memchunk_make_writable(&chunk, 0); */
/*                     pa_volume_memchunk(&chunk, &origin->sample_spec, &volume); */
/*                 } */

/*                 if (pa_memblockq_push(buffer, &chunk) < 0) { */
/*                     pa_memblock_unref(chunk.memblock); */
/*                     break; */
/*                 } */

/*                 pa_memblock_unref(chunk.memblock); */
/*                 l -= n; */
/*             } */
/*         } */

/*         if (i->resampled_chunk.memblock) { */

/*             /\* There is still some data left in the already resampled */
/*              * memory block. Hence, let's output it on the old sink */
/*              * and sleep so long on the new sink *\/ */

/*             pa_memblockq_push(buffer, &i->resampled_chunk); */
/*             silence_usec += pa_bytes_to_usec(i->resampled_chunk.length, &origin->sample_spec); */
/*         } */

/*         /\* Calculate the new sleeping time *\/ */
/*         i->move_silence = pa_usec_to_bytes( */
/*                 pa_bytes_to_usec(i->move_silence, &i->sample_spec) + */
/*                 silence_usec, */
/*                 &i->sample_spec); */
/*     } */

/*     /\* Okey, let's move it *\/ */
/*     pa_idxset_remove_by_data(origin->inputs, i, NULL); */
/*     pa_idxset_put(dest->inputs, i, NULL); */
/*     i->sink = dest; */

/*     /\* Replace resampler *\/ */
/*     if (new_resampler != i->resampler) { */
/*         if (i->resampler) */
/*             pa_resampler_free(i->resampler); */
/*         i->resampler = new_resampler; */

/*         /\* if the resampler changed, the silence memblock is */
/*          * probably invalid now, too *\/ */
/*         if (i->silence_memblock) { */
/*             pa_memblock_unref(i->silence_memblock); */
/*             i->silence_memblock = NULL; */
/*         } */
/*     } */

/*     /\* Dump already resampled data *\/ */
/*     if (i->resampled_chunk.memblock) { */
/*         pa_memblock_unref(i->resampled_chunk.memblock); */
/*         i->resampled_chunk.memblock = NULL; */
/*         i->resampled_chunk.index = i->resampled_chunk.length = 0; */
/*     } */

/*     /\* Notify everyone *\/ */
/*     pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index); */
/*     pa_sink_notify(i->sink); */

/*     /\* Ok, now let's feed the precomputed buffer to the old sink *\/ */
/*     if (buffer) */
/*         pa_play_memblockq(origin, "Ghost Stream", &origin->sample_spec, &origin->channel_map, buffer, NULL); */

/*     return 0; */
}

int pa_sink_input_process_msg(pa_msgobject *o, int code, void *userdata, pa_memchunk *chunk) {
    pa_sink_input *i = PA_SINK_INPUT(o);

    pa_sink_input_assert_ref(i);

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

/*             if (i->move_silence) */
/*                 r += pa_bytes_to_usec(i->move_silence, &i->sink->sample_spec); */

            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_SET_RATE: {

            i->thread_info.sample_spec.rate = PA_PTR_TO_UINT(userdata);
            pa_resampler_set_input_rate(i->thread_info.resampler, PA_PTR_TO_UINT(userdata));

            return 0;
        }

        case PA_SINK_INPUT_MESSAGE_SET_STATE: {
            if ((PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_DRAINED || PA_PTR_TO_UINT(userdata) == PA_SINK_INPUT_RUNNING) &&
                (i->thread_info.state != PA_SINK_INPUT_DRAINED) && (i->thread_info.state != PA_SINK_INPUT_RUNNING))
                pa_atomic_store(&i->thread_info.drained, 1);
            
            i->thread_info.state = PA_PTR_TO_UINT(userdata);

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
