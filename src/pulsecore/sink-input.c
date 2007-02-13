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

#include <pulsecore/sample-util.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/play-memblockq.h>
#include <pulsecore/namereg.h>

#include "sink-input.h"

#define CONVERT_BUFFER_LENGTH 4096
#define MOVE_BUFFER_LENGTH (1024*1024)
#define SILENCE_BUFFER_LENGTH (64*1024)

#define CHECK_VALIDITY_RETURN_NULL(condition) \
do {\
if (!(condition)) \
    return NULL; \
} while (0)

pa_sink_input_new_data* pa_sink_input_new_data_init(pa_sink_input_new_data *data) {
    assert(data);

    memset(data, 0, sizeof(*data));
    data->resample_method = PA_RESAMPLER_INVALID;
    return data;
}

void pa_sink_input_new_data_set_channel_map(pa_sink_input_new_data *data, const pa_channel_map *map) {
    assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_sink_input_new_data_set_volume(pa_sink_input_new_data *data, const pa_cvolume *volume) {
    assert(data);

    if ((data->volume_is_set = !!volume))
        data->volume = *volume;
}

void pa_sink_input_new_data_set_sample_spec(pa_sink_input_new_data *data, const pa_sample_spec *spec) {
    assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

pa_sink_input* pa_sink_input_new(
        pa_core *core,
        pa_sink_input_new_data *data,
        pa_sink_input_flags_t flags) {

    pa_sink_input *i;
    pa_resampler *resampler = NULL;
    int r;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX];

    assert(core);
    assert(data);

    if (!(flags & PA_SINK_INPUT_NO_HOOKS))
        if (pa_hook_fire(&core->hook_sink_input_new, data) < 0)
            return NULL;

    CHECK_VALIDITY_RETURN_NULL(!data->driver || pa_utf8_valid(data->driver));
    CHECK_VALIDITY_RETURN_NULL(!data->name || pa_utf8_valid(data->name));

    if (!data->sink)
        data->sink = pa_namereg_get(core, NULL, PA_NAMEREG_SINK, 1);

    CHECK_VALIDITY_RETURN_NULL(data->sink);
    CHECK_VALIDITY_RETURN_NULL(data->sink->state == PA_SINK_RUNNING);

    if (!data->sample_spec_is_set)
        data->sample_spec = data->sink->sample_spec;

    CHECK_VALIDITY_RETURN_NULL(pa_sample_spec_valid(&data->sample_spec));

    if (!data->channel_map_is_set)
        pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);

    CHECK_VALIDITY_RETURN_NULL(pa_channel_map_valid(&data->channel_map));
    CHECK_VALIDITY_RETURN_NULL(data->channel_map.channels == data->sample_spec.channels);

    if (!data->volume_is_set)
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);

    CHECK_VALIDITY_RETURN_NULL(pa_cvolume_valid(&data->volume));
    CHECK_VALIDITY_RETURN_NULL(data->volume.channels == data->sample_spec.channels);

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    CHECK_VALIDITY_RETURN_NULL(data->resample_method < PA_RESAMPLER_MAX);

    if (pa_idxset_size(data->sink->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn("Failed to create sink input: too many inputs per sink.");
        return NULL;
    }

    if ((flags & PA_SINK_INPUT_VARIABLE_RATE) ||
        !pa_sample_spec_equal(&data->sample_spec, &data->sink->sample_spec) ||
        !pa_channel_map_equal(&data->channel_map, &data->sink->channel_map))

        if (!(resampler = pa_resampler_new(
                      core->mempool,
                      &data->sample_spec, &data->channel_map,
                      &data->sink->sample_spec, &data->sink->channel_map,
                      data->resample_method))) {
            pa_log_warn("Unsupported resampling operation.");
            return NULL;
        }

    i = pa_xnew(pa_sink_input, 1);
    i->ref = 1;
    i->state = PA_SINK_INPUT_DRAINED;
    i->flags = flags;
    i->name = pa_xstrdup(data->name);
    i->driver = pa_xstrdup(data->driver);
    i->module = data->module;
    i->sink = data->sink;
    i->client = data->client;

    i->sample_spec = data->sample_spec;
    i->channel_map = data->channel_map;
    i->volume = data->volume;

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->underrun = NULL;
    i->userdata = NULL;

    i->move_silence = 0;

    pa_memchunk_reset(&i->resampled_chunk);
    i->resampler = resampler;
    i->resample_method = data->resample_method;
    i->silence_memblock = NULL;

    r = pa_idxset_put(core->sink_inputs, i, &i->index);
    assert(r == 0);
    r = pa_idxset_put(i->sink->inputs, i, NULL);
    assert(r == 0);

    pa_log_info("created %u \"%s\" on %s with sample spec %s",
                i->index,
                i->name,
                i->sink->name,
                pa_sample_spec_snprint(st, sizeof(st), &i->sample_spec));

    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, i->index);

    /* We do not call pa_sink_notify() here, because the virtual
     * functions have not yet been initialized */

    return i;
}

void pa_sink_input_disconnect(pa_sink_input *i) {
    assert(i);
    assert(i->state != PA_SINK_INPUT_DISCONNECTED);
    assert(i->sink);
    assert(i->sink->core);

    pa_idxset_remove_by_data(i->sink->core->sink_inputs, i, NULL);
    pa_idxset_remove_by_data(i->sink->inputs, i, NULL);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_REMOVE, i->index);
    i->sink = NULL;

    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->underrun = NULL;

    i->state = PA_SINK_INPUT_DISCONNECTED;
}

static void sink_input_free(pa_sink_input* i) {
    assert(i);

    if (i->state != PA_SINK_INPUT_DISCONNECTED)
        pa_sink_input_disconnect(i);

    pa_log_info("freed %u \"%s\"", i->index, i->name);

    if (i->resampled_chunk.memblock)
        pa_memblock_unref(i->resampled_chunk.memblock);

    if (i->resampler)
        pa_resampler_free(i->resampler);

    if (i->silence_memblock)
        pa_memblock_unref(i->silence_memblock);

    pa_xfree(i->name);
    pa_xfree(i->driver);
    pa_xfree(i);
}

void pa_sink_input_unref(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);

    if (!(--i->ref))
        sink_input_free(i);
}

pa_sink_input* pa_sink_input_ref(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);

    i->ref++;
    return i;
}

void pa_sink_input_kill(pa_sink_input*i) {
    assert(i);
    assert(i->ref >= 1);

    if (i->kill)
        i->kill(i);
}

pa_usec_t pa_sink_input_get_latency(pa_sink_input *i) {
    pa_usec_t r = 0;

    assert(i);
    assert(i->ref >= 1);

    if (i->get_latency)
        r += i->get_latency(i);

    if (i->resampled_chunk.memblock)
        r += pa_bytes_to_usec(i->resampled_chunk.length, &i->sink->sample_spec);

    if (i->move_silence)
        r += pa_bytes_to_usec(i->move_silence, &i->sink->sample_spec);

    return r;
}

int pa_sink_input_peek(pa_sink_input *i, pa_memchunk *chunk, pa_cvolume *volume) {
    int ret = -1;
    int do_volume_adj_here;
    int volume_is_norm;

    assert(i);
    assert(i->ref >= 1);
    assert(chunk);
    assert(volume);

    pa_sink_input_ref(i);

    if (!i->peek || !i->drop || i->state == PA_SINK_INPUT_CORKED)
        goto finish;

    assert(i->state == PA_SINK_INPUT_RUNNING || i->state == PA_SINK_INPUT_DRAINED);

    if (i->move_silence > 0) {

        /* We have just been moved and shall play some silence for a
         * while until the old sink has drained its playback buffer */

        if (!i->silence_memblock)
            i->silence_memblock = pa_silence_memblock_new(i->sink->core->mempool, &i->sink->sample_spec, SILENCE_BUFFER_LENGTH);

        chunk->memblock = pa_memblock_ref(i->silence_memblock);
        chunk->index = 0;
        chunk->length = i->move_silence < chunk->memblock->length ? i->move_silence : chunk->memblock->length;

        ret = 0;
        do_volume_adj_here = 1;
        goto finish;
    }

    if (!i->resampler) {
        do_volume_adj_here = 0;
        ret = i->peek(i, chunk);
        goto finish;
    }

    do_volume_adj_here = !pa_channel_map_equal(&i->channel_map, &i->sink->channel_map);
    volume_is_norm = pa_cvolume_is_norm(&i->volume);

    while (!i->resampled_chunk.memblock) {
        pa_memchunk tchunk;
        size_t l;

        if ((ret = i->peek(i, &tchunk)) < 0)
            goto finish;

        assert(tchunk.length);

        l = pa_resampler_request(i->resampler, CONVERT_BUFFER_LENGTH);

        if (l > tchunk.length)
            l = tchunk.length;

        i->drop(i, &tchunk, l);
        tchunk.length = l;

        /* It might be necessary to adjust the volume here */
        if (do_volume_adj_here && !volume_is_norm) {
            pa_memchunk_make_writable(&tchunk, 0);
            pa_volume_memchunk(&tchunk, &i->sample_spec, &i->volume);
        }

        pa_resampler_run(i->resampler, &tchunk, &i->resampled_chunk);
        pa_memblock_unref(tchunk.memblock);
    }

    assert(i->resampled_chunk.memblock);
    assert(i->resampled_chunk.length);

    *chunk = i->resampled_chunk;
    pa_memblock_ref(i->resampled_chunk.memblock);

    ret = 0;

finish:

    if (ret < 0 && i->state == PA_SINK_INPUT_RUNNING && i->underrun)
        i->underrun(i);

    if (ret >= 0)
        i->state = PA_SINK_INPUT_RUNNING;
    else if (ret < 0 && i->state == PA_SINK_INPUT_RUNNING)
        i->state = PA_SINK_INPUT_DRAINED;

    if (ret >= 0) {
        /* Let's see if we had to apply the volume adjustment
         * ourselves, or if this can be done by the sink for us */

        if (do_volume_adj_here)
            /* We had different channel maps, so we already did the adjustment */
            pa_cvolume_reset(volume, i->sink->sample_spec.channels);
        else
            /* We've both the same channel map, so let's have the sink do the adjustment for us*/
            *volume = i->volume;
    }

    pa_sink_input_unref(i);

    return ret;
}

void pa_sink_input_drop(pa_sink_input *i, const pa_memchunk *chunk, size_t length) {
    assert(i);
    assert(i->ref >= 1);
    assert(length > 0);

    if (i->move_silence > 0) {

        if (chunk) {

            if (chunk->memblock != i->silence_memblock ||
                chunk->index != 0 ||
                (chunk->memblock && (chunk->length != (i->silence_memblock->length < i->move_silence ? i->silence_memblock->length : i->move_silence))))
                return;

        }

        assert(i->move_silence >= length);

        i->move_silence -= length;

        if (i->move_silence <= 0) {
            assert(i->silence_memblock);
            pa_memblock_unref(i->silence_memblock);
            i->silence_memblock = NULL;
        }

        return;
    }

    if (!i->resampler) {
        if (i->drop)
            i->drop(i, chunk, length);
        return;
    }

    assert(i->resampled_chunk.memblock);
    assert(i->resampled_chunk.length >= length);

    i->resampled_chunk.index += length;
    i->resampled_chunk.length -= length;

    if (i->resampled_chunk.length <= 0) {
        pa_memblock_unref(i->resampled_chunk.memblock);
        i->resampled_chunk.memblock = NULL;
        i->resampled_chunk.index = i->resampled_chunk.length = 0;
    }
}

void pa_sink_input_set_volume(pa_sink_input *i, const pa_cvolume *volume) {
    assert(i);
    assert(i->ref >= 1);
    assert(i->sink);
    assert(i->sink->core);

    if (pa_cvolume_equal(&i->volume, volume))
        return;

    i->volume = *volume;
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

const pa_cvolume * pa_sink_input_get_volume(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);

    return &i->volume;
}

void pa_sink_input_cork(pa_sink_input *i, int b) {
    int n;

    assert(i);
    assert(i->ref >= 1);

    assert(i->state != PA_SINK_INPUT_DISCONNECTED);

    n = i->state == PA_SINK_INPUT_CORKED && !b;

    if (b)
        i->state = PA_SINK_INPUT_CORKED;
    else if (i->state == PA_SINK_INPUT_CORKED)
        i->state = PA_SINK_INPUT_DRAINED;

    if (n)
        pa_sink_notify(i->sink);
}

void pa_sink_input_set_rate(pa_sink_input *i, uint32_t rate) {
    assert(i);
    assert(i->resampler);
    assert(i->ref >= 1);

    if (i->sample_spec.rate == rate)
        return;

    i->sample_spec.rate = rate;
    pa_resampler_set_input_rate(i->resampler, rate);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

void pa_sink_input_set_name(pa_sink_input *i, const char *name) {
    assert(i);
    assert(i->ref >= 1);

    if (!i->name && !name)
        return;

    if (i->name && name && !strcmp(i->name, name))
        return;

    pa_xfree(i->name);
    i->name = pa_xstrdup(name);

    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
}

pa_resample_method_t pa_sink_input_get_resample_method(pa_sink_input *i) {
    assert(i);
    assert(i->ref >= 1);

    if (!i->resampler)
        return i->resample_method;

    return pa_resampler_get_method(i->resampler);
}

int pa_sink_input_move_to(pa_sink_input *i, pa_sink *dest, int immediately) {
    pa_resampler *new_resampler = NULL;
    pa_memblockq *buffer = NULL;
    pa_sink *origin;

    assert(i);
    assert(dest);

    origin = i->sink;

    if (dest == origin)
        return 0;

    if (pa_idxset_size(dest->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn("Failed to move sink input: too many inputs per sink.");
        return -1;
    }

    if (i->resampler &&
        pa_sample_spec_equal(&origin->sample_spec, &dest->sample_spec) &&
        pa_channel_map_equal(&origin->channel_map, &dest->channel_map))

        /* Try to reuse the old resampler if possible */
        new_resampler = i->resampler;

    else if ((i->flags & PA_SINK_INPUT_VARIABLE_RATE) ||
        !pa_sample_spec_equal(&i->sample_spec, &dest->sample_spec) ||
        !pa_channel_map_equal(&i->channel_map, &dest->channel_map)) {

        /* Okey, we need a new resampler for the new sink */

        if (!(new_resampler = pa_resampler_new(
                      dest->core->mempool,
                      &i->sample_spec, &i->channel_map,
                      &dest->sample_spec, &dest->channel_map,
                      i->resample_method))) {
            pa_log_warn("Unsupported resampling operation.");
            return -1;
        }
    }

    if (!immediately) {
        pa_usec_t old_latency, new_latency;
        pa_usec_t silence_usec = 0;

        buffer = pa_memblockq_new(0, MOVE_BUFFER_LENGTH, 0, pa_frame_size(&origin->sample_spec), 0, 0, NULL);

        /* Let's do a little bit of Voodoo for compensating latency
         * differences */

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
            size_t l;
            int volume_is_norm;

            /* The latency of new sink is larger than the latency of
             * the old sink. Therefore we have to precompute a little
             * and make sure that this is still played on the old
             * sink, until we can play the first sample on the new
             * sink.*/

            l = pa_usec_to_bytes(new_latency - old_latency, &origin->sample_spec);

            volume_is_norm = pa_cvolume_is_norm(&i->volume);

            while (l > 0) {
                pa_memchunk chunk;
                pa_cvolume volume;
                size_t n;

                if (pa_sink_input_peek(i, &chunk, &volume) < 0)
                    break;

                n = chunk.length > l ? l : chunk.length;
                pa_sink_input_drop(i, &chunk, n);
                chunk.length = n;

                if (!volume_is_norm) {
                    pa_memchunk_make_writable(&chunk, 0);
                    pa_volume_memchunk(&chunk, &origin->sample_spec, &volume);
                }

                if (pa_memblockq_push(buffer, &chunk) < 0) {
                    pa_memblock_unref(chunk.memblock);
                    break;
                }

                pa_memblock_unref(chunk.memblock);
                l -= n;
            }
        }

        if (i->resampled_chunk.memblock) {

            /* There is still some data left in the already resampled
             * memory block. Hence, let's output it on the old sink
             * and sleep so long on the new sink */

            pa_memblockq_push(buffer, &i->resampled_chunk);
            silence_usec += pa_bytes_to_usec(i->resampled_chunk.length, &origin->sample_spec);
        }

        /* Calculate the new sleeping time */
        i->move_silence = pa_usec_to_bytes(
                pa_bytes_to_usec(i->move_silence, &i->sample_spec) +
                silence_usec,
                &i->sample_spec);
    }

    /* Okey, let's move it */
    pa_idxset_remove_by_data(origin->inputs, i, NULL);
    pa_idxset_put(dest->inputs, i, NULL);
    i->sink = dest;

    /* Replace resampler */
    if (new_resampler != i->resampler) {
        if (i->resampler)
            pa_resampler_free(i->resampler);
        i->resampler = new_resampler;

        /* if the resampler changed, the silence memblock is
         * probably invalid now, too */
        if (i->silence_memblock) {
            pa_memblock_unref(i->silence_memblock);
            i->silence_memblock = NULL;
        }
    }

    /* Dump already resampled data */
    if (i->resampled_chunk.memblock) {
        pa_memblock_unref(i->resampled_chunk.memblock);
        i->resampled_chunk.memblock = NULL;
        i->resampled_chunk.index = i->resampled_chunk.length = 0;
    }

    /* Notify everyone */
    pa_subscription_post(i->sink->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE, i->index);
    pa_sink_notify(i->sink);

    /* Ok, no let's feed the precomputed buffer to the old sink */
    if (buffer)
        pa_play_memblockq(origin, "Ghost Stream", &origin->sample_spec, &origin->channel_map, buffer, NULL);

    return 0;
}
