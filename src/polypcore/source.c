/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
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

#include <polypcore/source-output.h>
#include <polypcore/namereg.h>
#include <polypcore/xmalloc.h>
#include <polypcore/core-subscribe.h>
#include <polypcore/log.h>

#include "source.h"

pa_source* pa_source_new(
    pa_core *core,
    const char *driver,
    const char *name,
    int fail,
    const pa_sample_spec *spec,
    const pa_channel_map *map) {
    
    pa_source *s;
    char st[256];
    int r;
    
    assert(core);
    assert(name);
    assert(*name);
    assert(spec);

    s = pa_xnew(pa_source, 1);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SOURCE, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->ref = 1;
    s->core = core;
    s->state = PA_SOURCE_RUNNING;
    s->name = pa_xstrdup(name);
    s->description = NULL;
    s->driver = pa_xstrdup(driver);
    s->owner = NULL;
    
    s->sample_spec = *spec;
    if (map)
        s->channel_map = *map;
    else
        pa_channel_map_init_auto(&s->channel_map, spec->channels);

    s->outputs = pa_idxset_new(NULL, NULL);
    s->monitor_of = NULL;

    pa_cvolume_reset(&s->sw_volume, spec->channels);
    pa_cvolume_reset(&s->hw_volume, spec->channels);

    s->get_latency = NULL;
    s->notify = NULL;
    s->set_hw_volume = NULL;
    s->get_hw_volume = NULL;
    s->userdata = NULL;

    r = pa_idxset_put(core->sources, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info(__FILE__": created %u \"%s\" with sample spec \"%s\"\n", s->index, s->name, st); 

    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    
    return s;
}

void pa_source_disconnect(pa_source *s) {
    pa_source_output *o, *j = NULL;
    
    assert(s);
    assert(s->state == PA_SOURCE_RUNNING);

    pa_namereg_unregister(s->core, s->name);
    
    while ((o = pa_idxset_first(s->outputs, NULL))) {
        assert(o != j);
        pa_source_output_kill(o);
        j = o;
    }

    pa_idxset_remove_by_data(s->core->sources, s, NULL);

    s->get_latency = NULL;
    s->notify = NULL;
    s->get_hw_volume = NULL;
    s->set_hw_volume = NULL;
    
    s->state = PA_SOURCE_DISCONNECTED;
    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
}

static void source_free(pa_source *s) {
    assert(s);
    assert(!s->ref);
    
    if (s->state != PA_SOURCE_DISCONNECTED)
        pa_source_disconnect(s);
    
    pa_log_info(__FILE__": freed %u \"%s\"\n", s->index, s->name); 

    pa_idxset_free(s->outputs, NULL, NULL);

    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s->driver);
    pa_xfree(s);
}

void pa_source_unref(pa_source *s) {
    assert(s);
    assert(s->ref >= 1);

    if (!(--s->ref))
        source_free(s);
}

pa_source* pa_source_ref(pa_source *s) {
    assert(s);
    assert(s->ref >= 1);
    
    s->ref++;
    return s;
}

void pa_source_notify(pa_source*s) {
    assert(s);
    assert(s->ref >= 1);

    if (s->notify)
        s->notify(s);
}

static int do_post(void *p, PA_GCC_UNUSED uint32_t idx, int *del, void*userdata) {
    pa_source_output *o = p;
    const pa_memchunk *chunk = userdata;
    
    assert(o);
    assert(chunk);

    pa_source_output_push(o, chunk);
    return 0;
}

void pa_source_post(pa_source*s, const pa_memchunk *chunk) {
    assert(s);
    assert(s->ref >= 1);
    assert(chunk);

    pa_source_ref(s);

    if (!pa_cvolume_is_norm(&s->sw_volume)) {
        pa_memchunk_make_writable(chunk, s->core->memblock_stat, 0);
        pa_volume_memchunk(chunk, &s->sample_spec, &s->sw_volume);
    }

    pa_idxset_foreach(s->outputs, do_post, (void*) chunk);

    pa_source_unref(s);
}

void pa_source_set_owner(pa_source *s, pa_module *m) {
    assert(s);
    assert(s->ref >= 1);
    
    s->owner = m;
}

pa_usec_t pa_source_get_latency(pa_source *s) {
    assert(s);
    assert(s->ref >= 1);

    if (!s->get_latency)
        return 0;

    return s->get_latency(s);
}

void pa_source_set_volume(pa_source *s, pa_mixer_t m, const pa_cvolume *volume) {
    pa_cvolume *v;
    
    assert(s);
    assert(s->ref >= 1);
    assert(volume);

    if (m == PA_MIXER_HARDWARE && s->set_hw_volume) 
        v = &s->hw_volume;
    else
        v = &s->sw_volume;

    if (pa_cvolume_equal(v, volume))
        return;
        
    *v = *volume;

    if (v == &s->hw_volume)
        if (s->set_hw_volume(s) < 0)
            s->sw_volume =  *volume;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

const pa_cvolume *pa_source_get_volume(pa_source *s, pa_mixer_t m) {
    assert(s);
    assert(s->ref >= 1);

    if (m == PA_MIXER_HARDWARE && s->set_hw_volume) {

        if (s->get_hw_volume)
            s->get_hw_volume(s);
        
        return &s->hw_volume;
    } else
        return &s->sw_volume;
}
