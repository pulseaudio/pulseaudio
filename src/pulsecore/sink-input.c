/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include "sink-input.h"

#define CONVERT_BUFFER_LENGTH 4096
#define MOVE_BUFFER_LENGTH (1024*1024)
#define SILENCE_BUFFER_LENGTH (64*1024)

#define CHECK_VALIDITY_RETURN_NULL(condition) \
do {\
if (!(condition)) \
    return NULL; \
} while (0)

pa_sink_input* pa_sink_input_new(
        pa_sink *s,
        const char *driver,
        const char *name,
        const pa_sample_spec *spec,
        const pa_channel_map *map,
        const pa_cvolume *volume, 
        int variable_rate,
        int resample_method) {
    
    pa_sink_input *i;
    pa_resampler *resampler = NULL;
    int r;
    char st[256];
    pa_channel_map tmap;
    pa_cvolume tvol;

    assert(s);
    assert(spec);
    assert(s->state == PA_SINK_RUNNING);

    CHECK_VALIDITY_RETURN_NULL(pa_sample_spec_valid(spec));

    if (!map)
        map = pa_channel_map_init_auto(&tmap, spec->channels, PA_CHANNEL_MAP_DEFAULT);
    if (!volume)
        volume = pa_cvolume_reset(&tvol, spec->channels);

    CHECK_VALIDITY_RETURN_NULL(map && pa_channel_map_valid(map));
    CHECK_VALIDITY_RETURN_NULL(volume && pa_cvolume_valid(volume));
    CHECK_VALIDITY_RETURN_NULL(map->channels == spec->channels);
    CHECK_VALIDITY_RETURN_NULL(volume->channels == spec->channels);
    CHECK_VALIDITY_RETURN_NULL(!driver || pa_utf8_valid(driver));
    CHECK_VALIDITY_RETURN_NULL(pa_utf8_valid(name));
            
    if (pa_idxset_size(s->inputs) >= PA_MAX_INPUTS_PER_SINK) {
        pa_log_warn(__FILE__": Failed to create sink input: too many inputs per sink.");
        return NULL;
    }

    if (resample_method == PA_RESAMPLER_INVALID)
        resample_method = s->core->resample_method;
    
    if (variable_rate || !pa_sample_spec_equal(spec, &s->sample_spec) || !pa_channel_map_equal(map, &s->channel_map))
        if (!(resampler = pa_resampler_new(spec, map, &s->sample_spec, &s->channel_map, s->core->memblock_stat, resample_method))) {
            pa_log_warn(__FILE__": Unsupported resampling operation.");
            return NULL;
        }

    i = pa_xnew(pa_sink_input, 1);
    i->ref = 1;
    i->state = PA_SINK_INPUT_DRAINED;
    i->name = pa_xstrdup(name);
    i->driver = pa_xstrdup(driver);
    i->owner = NULL;
    i->sink = s;
    i->client = NULL;

    i->sample_spec = *spec;
    i->channel_map = *map;
    i->volume = *volume;
        
    i->peek = NULL;
    i->drop = NULL;
    i->kill = NULL;
    i->get_latency = NULL;
    i->underrun = NULL;
    i->userdata = NULL;
    i->move_silence = 0;

    pa_memchunk_reset(&i->resampled_chunk);
    i->resampler = resampler;
    i->resample_method = resample_method;
    i->variable_rate = variable_rate;

    i->silence_memblock = NULL;
    
    assert(s->core);
    r = pa_idxset_put(s->core->sink_inputs, i, &i->index);
    assert(r == 0 && i->index != PA_IDXSET_INVALID);
    r = pa_idxset_put(s->inputs, i, NULL);
    assert(r == 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info(__FILE__": created %u \"%s\" on %u with sample spec \"%s\"", i->index, i->name, s->index, st);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW, i->index);

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

    pa_log_info(__FILE__": freed %u \"%s\"", i->index, i->name); 
    
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
            i->silence_memblock = pa_silence_memblock_new(&i->sink->sample_spec, SILENCE_BUFFER_LENGTH, i->sink->core->memblock_stat);

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
            pa_memchunk_make_writable(&tchunk, i->sink->core->memblock_stat, 0);
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
        pa_log_warn(__FILE__": Failed to move sink input: too many inputs per sink.");
        return -1;
    }

    if (i->resampler &&
        pa_sample_spec_equal(&origin->sample_spec, &dest->sample_spec) &&
        pa_channel_map_equal(&origin->channel_map, &dest->channel_map))

        /* Try to reuse the old resampler if possible */
        new_resampler = i->resampler;
    
    else if (i->variable_rate ||
        !pa_sample_spec_equal(&i->sample_spec, &dest->sample_spec) ||
        !pa_channel_map_equal(&i->channel_map, &dest->channel_map)) {

        /* Okey, we need a new resampler for the new sink */
        
        if (!(new_resampler = pa_resampler_new(
                      &i->sample_spec, &i->channel_map,
                      &dest->sample_spec, &dest->channel_map,
                      dest->core->memblock_stat,
                      i->resample_method))) {
            pa_log_warn(__FILE__": Unsupported resampling operation.");
            return -1;
        }
    }

    if (!immediately) {
        pa_usec_t old_latency, new_latency;
        pa_usec_t silence_usec = 0;

        buffer = pa_memblockq_new(0, MOVE_BUFFER_LENGTH, 0, pa_frame_size(&origin->sample_spec), 0, 0, NULL, NULL);
        
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
                    pa_memchunk_make_writable(&chunk, origin->core->memblock_stat, 0);
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
