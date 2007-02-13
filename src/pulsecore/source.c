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

#include <pulsecore/source-output.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/sample-util.h>

#include "source.h"

#define CHECK_VALIDITY_RETURN_NULL(condition) \
do {\
if (!(condition)) \
    return NULL; \
} while (0)

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
    pa_channel_map tmap;

    assert(core);
    assert(name);
    assert(spec);

    CHECK_VALIDITY_RETURN_NULL(pa_sample_spec_valid(spec));

    if (!map)
        map = pa_channel_map_init_auto(&tmap, spec->channels, PA_CHANNEL_MAP_DEFAULT);

    CHECK_VALIDITY_RETURN_NULL(map && pa_channel_map_valid(map));
    CHECK_VALIDITY_RETURN_NULL(map->channels == spec->channels);
    CHECK_VALIDITY_RETURN_NULL(!driver || pa_utf8_valid(driver));
    CHECK_VALIDITY_RETURN_NULL(pa_utf8_valid(name) && *name);

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
    s->channel_map = *map;

    s->outputs = pa_idxset_new(NULL, NULL);
    s->monitor_of = NULL;

    pa_cvolume_reset(&s->sw_volume, spec->channels);
    pa_cvolume_reset(&s->hw_volume, spec->channels);
    s->sw_muted = 0;
    s->hw_muted = 0;

    s->is_hardware = 0;

    s->get_latency = NULL;
    s->notify = NULL;
    s->set_hw_volume = NULL;
    s->get_hw_volume = NULL;
    s->set_hw_mute = NULL;
    s->get_hw_mute = NULL;
    s->userdata = NULL;

    r = pa_idxset_put(core->sources, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    pa_log_info("created %u \"%s\" with sample spec \"%s\"", s->index, s->name, st);

    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_NEW, s->index);

    return s;
}

void pa_source_disconnect(pa_source *s) {
    pa_source_output *o, *j = NULL;

    assert(s);
    assert(s->state == PA_SOURCE_RUNNING);

    s->state = PA_SOURCE_DISCONNECTED;
    pa_namereg_unregister(s->core, s->name);

    pa_hook_fire(&s->core->hook_source_disconnect, s);

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
    s->set_hw_mute = NULL;
    s->get_hw_mute = NULL;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
}

static void source_free(pa_source *s) {
    assert(s);
    assert(!s->ref);

    if (s->state != PA_SOURCE_DISCONNECTED)
        pa_source_disconnect(s);

    pa_log_info("freed %u \"%s\"", s->index, s->name);

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

static int do_post(void *p, PA_GCC_UNUSED uint32_t idx, PA_GCC_UNUSED int *del, void*userdata) {
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

    if (s->sw_muted || !pa_cvolume_is_norm(&s->sw_volume)) {
        pa_memchunk vchunk = *chunk;

        pa_memblock_ref(vchunk.memblock);
        pa_memchunk_make_writable(&vchunk, 0);
        if (s->sw_muted)
            pa_silence_memchunk(&vchunk, &s->sample_spec);
        else
            pa_volume_memchunk(&vchunk, &s->sample_spec, &s->sw_volume);
        pa_idxset_foreach(s->outputs, do_post, &vchunk);
        pa_memblock_unref(vchunk.memblock);
    } else
        pa_idxset_foreach(s->outputs, do_post, (void*) chunk);

    pa_source_unref(s);
}

void pa_source_set_owner(pa_source *s, pa_module *m) {
    assert(s);
    assert(s->ref >= 1);

    if (m == s->owner)
        return;

    s->owner = m;
    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
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

void pa_source_set_mute(pa_source *s, pa_mixer_t m, int mute) {
    int *t;

    assert(s);
    assert(s->ref >= 1);

    if (m == PA_MIXER_HARDWARE && s->set_hw_mute)
        t = &s->hw_muted;
    else
        t = &s->sw_muted;

    if (!!*t == !!mute)
        return;

    *t = !!mute;

    if (t == &s->hw_muted)
        if (s->set_hw_mute(s) < 0)
            s->sw_muted = !!mute;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

int pa_source_get_mute(pa_source *s, pa_mixer_t m) {
    assert(s);
    assert(s->ref >= 1);

    if (m == PA_MIXER_HARDWARE && s->set_hw_mute) {

        if (s->get_hw_mute)
            s->get_hw_mute(s);

        return s->hw_muted;
    } else
        return s->sw_muted;
}

void pa_source_set_description(pa_source *s, const char *description) {
    assert(s);
    assert(s->ref >= 1);

    if (!description && !s->description)
        return;

    if (description && s->description && !strcmp(description, s->description))
        return;

    pa_xfree(s->description);
    s->description = pa_xstrdup(description);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

unsigned pa_source_used_by(pa_source *s) {
    assert(s);
    assert(s->ref >= 1);

    return pa_idxset_size(s->outputs);
}
