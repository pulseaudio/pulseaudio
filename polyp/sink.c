/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "sink.h"
#include "sink-input.h"
#include "namereg.h"
#include "util.h"
#include "sample-util.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "log.h"

#define MAX_MIX_CHANNELS 32

struct pa_sink* pa_sink_new(struct pa_core *core, const char *name, int fail, const struct pa_sample_spec *spec) {
    struct pa_sink *s;
    char *n = NULL;
    char st[256];
    int r;
    assert(core && name && *name && spec);

    s = pa_xmalloc(sizeof(struct pa_sink));

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SINK, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->name = pa_xstrdup(name);
    s->description = NULL;

    s->ref = 1;
    s->state = PA_SINK_RUNNING;
    
    s->owner = NULL;
    s->core = core;
    s->sample_spec = *spec;
    s->inputs = pa_idxset_new(NULL, NULL);

    n = pa_sprintf_malloc("%s_monitor", name);
    s->monitor_source = pa_source_new(core, n, 0, spec);
    assert(s->monitor_source);
    pa_xfree(n);
    s->monitor_source->monitor_of = s;
    s->monitor_source->description = pa_sprintf_malloc("Monitor source of sink '%s'", s->name);
    
    s->volume = PA_VOLUME_NORM;

    s->notify = NULL;
    s->get_latency = NULL;
    s->userdata = NULL;

    r = pa_idxset_put(core->sinks, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);
    
    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log(__FILE__": created %u \"%s\" with sample spec \"%s\"\n", s->index, s->name, st);

    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    
    return s;
}

void pa_sink_disconnect(struct pa_sink* s) {
    struct pa_sink_input *i, *j = NULL;
    assert(s && s->state == PA_SINK_RUNNING);

    pa_namereg_unregister(s->core, s->name);
    
    while ((i = pa_idxset_first(s->inputs, NULL))) {
        assert(i != j);
        pa_sink_input_kill(i);
        j = i;
    }

    pa_source_disconnect(s->monitor_source);

    pa_idxset_remove_by_data(s->core->sinks, s, NULL);
    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);

    s->notify = NULL;
    s->get_latency = NULL;
    
    s->state = PA_SINK_DISCONNECTED;
}

static void sink_free(struct pa_sink *s) {
    assert(s && s->ref == 0);
    
    if (s->state != PA_SINK_DISCONNECTED)
        pa_sink_disconnect(s);

    pa_log(__FILE__": freed %u \"%s\"\n", s->index, s->name);

    pa_source_unref(s->monitor_source);
    s->monitor_source = NULL;
    
    pa_idxset_free(s->inputs, NULL, NULL);

    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s);
}

void pa_sink_unref(struct pa_sink*s) {
    assert(s && s->ref >= 1);

    if (!(--s->ref))
        sink_free(s);
}

struct pa_sink* pa_sink_ref(struct pa_sink *s) {
    assert(s && s->ref >= 1);
    s->ref++;
    return s;
}

void pa_sink_notify(struct pa_sink*s) {
    assert(s && s->ref >= 1);

    if (s->notify)
        s->notify(s);
}

static unsigned fill_mix_info(struct pa_sink *s, struct pa_mix_info *info, unsigned maxinfo) {
    uint32_t index = PA_IDXSET_INVALID;
    struct pa_sink_input *i;
    unsigned n = 0;
    
    assert(s && s->ref >= 1 && info);

    for (i = pa_idxset_first(s->inputs, &index); maxinfo > 0 && i; i = pa_idxset_next(s->inputs, &index)) {
        pa_sink_input_ref(i);

        if (pa_sink_input_peek(i, &info->chunk) < 0) {
            pa_sink_input_unref(i);
            continue;
        }

        info->volume = i->volume;
        info->userdata = i;
        
        assert(info->chunk.memblock && info->chunk.memblock->data && info->chunk.length);
        
        info++;
        maxinfo--;
        n++;
    }

    return n;
}

static void inputs_drop(struct pa_sink *s, struct pa_mix_info *info, unsigned maxinfo, size_t length) {
    assert(s && s->ref >= 1 && info);

    for (; maxinfo > 0; maxinfo--, info++) {
        struct pa_sink_input *i = info->userdata;
        assert(i && info->chunk.memblock);
        
        pa_sink_input_drop(i, &info->chunk, length);
        pa_memblock_unref(info->chunk.memblock);

        pa_sink_input_unref(i);
        info->userdata = NULL;
    }
}
        
int pa_sink_render(struct pa_sink*s, size_t length, struct pa_memchunk *result) {
    struct pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t l;
    int r = -1;
    assert(s && s->ref >= 1 && length && result);

    pa_sink_ref(s);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        goto finish;

    if (n == 1) {
        uint32_t volume = PA_VOLUME_NORM;
        struct pa_sink_input *i = info[0].userdata;
        assert(i);
        *result = info[0].chunk;
        pa_memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

        l = result->length;

        if (s->volume != PA_VOLUME_NORM || info[0].volume != PA_VOLUME_NORM)
            volume = pa_volume_multiply(s->volume, info[0].volume);
        
        if (volume != PA_VOLUME_NORM) {
            pa_memchunk_make_writable(result, s->core->memblock_stat);
            pa_volume_memchunk(result, &s->sample_spec, volume);
        }
    } else {
        result->memblock = pa_memblock_new(length, s->core->memblock_stat);
        assert(result->memblock);

        result->length = l = pa_mix(info, n, result->memblock->data, length, &s->sample_spec, s->volume);
        result->index = 0;
        
        assert(l);
    }

    inputs_drop(s, info, n, l);

    assert(s->monitor_source);
    pa_source_post(s->monitor_source, result);

    r = 0;

finish:
    pa_sink_unref(s);

    return r;
}

int pa_sink_render_into(struct pa_sink*s, struct pa_memchunk *target) {
    struct pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t l;
    int r = -1;
    assert(s && s->ref >= 1 && target && target->length && target->memblock && target->memblock->data);

    pa_sink_ref(s);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        goto finish;

    if (n == 1) {
        uint32_t volume = PA_VOLUME_NORM;
        struct pa_sink_info *i = info[0].userdata;
        assert(i);

        l = target->length;
        if (l > info[0].chunk.length)
            l = info[0].chunk.length;
        
        memcpy((uint8_t*) target->memblock->data+target->index, (uint8_t*) info[0].chunk.memblock->data + info[0].chunk.index, l);
        target->length = l;

        if (s->volume != PA_VOLUME_NORM || info[0].volume != PA_VOLUME_NORM)
            volume = pa_volume_multiply(s->volume, info[0].volume);

        if (volume != PA_VOLUME_NORM)
            pa_volume_memchunk(target, &s->sample_spec, volume);
    } else
        target->length = l = pa_mix(info, n, (uint8_t*) target->memblock->data+target->index, target->length, &s->sample_spec, s->volume);
    
    assert(l);
    inputs_drop(s, info, n, l);

    assert(s->monitor_source);
    pa_source_post(s->monitor_source, target);

    r = 0;

finish:
    pa_sink_unref(s);
    
    return r;
}

void pa_sink_render_into_full(struct pa_sink *s, struct pa_memchunk *target) {
    struct pa_memchunk chunk;
    size_t l, d;
    assert(s && s->ref >= 1 && target && target->memblock && target->length && target->memblock->data);

    pa_sink_ref(s);
    
    l = target->length;
    d = 0;
    while (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        
        if (pa_sink_render_into(s, &chunk) < 0)
            break;

        d += chunk.length;
        l -= chunk.length;
    }

    if (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        pa_silence_memchunk(&chunk, &s->sample_spec);
    }

    pa_sink_unref(s);
}

void pa_sink_render_full(struct pa_sink *s, size_t length, struct pa_memchunk *result) {
    assert(s && s->ref >= 1 && length && result);

    /*** This needs optimization ***/
    
    result->memblock = pa_memblock_new(result->length = length, s->core->memblock_stat);
    result->index = 0;

    pa_sink_render_into_full(s, result);
}

pa_usec_t pa_sink_get_latency(struct pa_sink *s) {
    assert(s && s->ref >= 1);

    if (!s->get_latency)
        return 0;

    return s->get_latency(s);
}

void pa_sink_set_owner(struct pa_sink *s, struct pa_module *m) {
    assert(s && s->ref >= 1);
           
    s->owner = m;

    if (s->monitor_source)
        pa_source_set_owner(s->monitor_source, m);
}

void pa_sink_set_volume(struct pa_sink *s, pa_volume_t volume) {
    assert(s && s->ref >= 1);
    
    if (s->volume != volume) {
        s->volume = volume;
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    }
}
