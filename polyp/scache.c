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

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "scache.h"
#include "sink-input.h"
#include "mainloop.h"
#include "sample-util.h"
#include "play-memchunk.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "namereg.h"

#define UNLOAD_POLL_TIME 2

static void timeout_callback(struct pa_mainloop_api *m, struct pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct pa_core *c = userdata;
    struct timeval ntv;
    assert(c && c->mainloop == m && c->scache_auto_unload_event == e);

    pa_scache_unload_unused(c);

    gettimeofday(&ntv, NULL);
    ntv.tv_sec += UNLOAD_POLL_TIME;
    m->time_restart(e, &ntv);
}

static void free_entry(struct pa_scache_entry *e) {
    assert(e);
    pa_namereg_unregister(e->core, e->name);
    pa_subscription_post(e->core, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_REMOVE, e->index);
    pa_xfree(e->name);
    if (e->memchunk.memblock)
        pa_memblock_unref(e->memchunk.memblock);
    pa_xfree(e);
}

int pa_scache_add_item(struct pa_core *c, const char *name, struct pa_sample_spec *ss, struct pa_memchunk *chunk, uint32_t *index, int auto_unload) {
    struct pa_scache_entry *e;
    int put;
    assert(c && name);

    if ((e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 0))) {
        put = 0;
        if (e->memchunk.memblock)
            pa_memblock_unref(e->memchunk.memblock);
        assert(e->core == c);
    } else {

        put = 1;
        e = pa_xmalloc(sizeof(struct pa_scache_entry));

        if (!pa_namereg_register(c, name, PA_NAMEREG_SAMPLE, e, 1)) {
            pa_xfree(e);
            return -1;
        }
        
        e->name = pa_xstrdup(name);
        e->core = c;
    }

    e->volume = PA_VOLUME_NORM;
    e->auto_unload = auto_unload;
    e->last_used_time = 0;

    if (ss)
        e->sample_spec = *ss;
    else
        memset(&e->sample_spec, 0, sizeof(struct pa_sample_spec));

    if (chunk) {
        e->memchunk = *chunk;
        pa_memblock_ref(e->memchunk.memblock);
    } else {
        e->memchunk.memblock = NULL;
        e->memchunk.index = e->memchunk.length = 0;
    }

    if (put) {
        if (!c->scache) {
            c->scache = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
            assert(c->scache);
        }
        
        pa_idxset_put(c->scache, e, &e->index);

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_NEW, e->index);
    } else
        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);
        
    if (index)
        *index = e->index;

    if (!c->scache_auto_unload_event) {
        struct timeval ntv;
        gettimeofday(&ntv, NULL);
        ntv.tv_sec += UNLOAD_POLL_TIME;
        c->scache_auto_unload_event = c->mainloop->time_new(c->mainloop, &ntv, timeout_callback, c);
    }
                                                            
    return 0;
}

int pa_scache_remove_item(struct pa_core *c, const char *name) {
    struct pa_scache_entry *e;
    assert(c && name);

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 0)))
        return -1;

    if (pa_idxset_remove_by_data(c->scache, e, NULL) != e)
        assert(0);

    free_entry(e);
    return 0;
}

static void free_cb(void *p, void *userdata) {
    struct pa_scache_entry *e = p;
    assert(e);
    free_entry(e);
}

void pa_scache_free(struct pa_core *c) {
    assert(c);

    if (c->scache) {
        pa_idxset_free(c->scache, free_cb, NULL);
        c->scache = NULL;
    }

    if (c->scache_auto_unload_event)
        c->mainloop->time_free(c->scache_auto_unload_event);
}

int pa_scache_play_item(struct pa_core *c, const char *name, struct pa_sink *sink, uint32_t volume) {
    struct pa_scache_entry *e;
    assert(c && name && sink);

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 1)))
        return -1;

    if (!e->memchunk.memblock)
        return -1;

    if (pa_play_memchunk(sink, name, &e->sample_spec, &e->memchunk, pa_volume_multiply(volume, e->volume)) < 0)
        return -1;

    if (e->auto_unload)
        time(&e->last_used_time);
    
    return 0;
}

const char * pa_scache_get_name_by_id(struct pa_core *c, uint32_t id) {
    struct pa_scache_entry *e;
    assert(c && id != PA_IDXSET_INVALID);

    if (!c->scache || !(e = pa_idxset_get_by_index(c->scache, id)))
        return NULL;

    return e->name;
}

uint32_t pa_scache_get_id_by_name(struct pa_core *c, const char *name) {
    struct pa_scache_entry *e;
    assert(c && name);

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 1)))
        return PA_IDXSET_INVALID;

    return e->index;
}

uint32_t pa_scache_total_size(struct pa_core *c) {
    struct pa_scache_entry *e;
    uint32_t index;
    uint32_t sum = 0;

    if (!c->scache)
        return 0;
    
    for (e = pa_idxset_first(c->scache, &index); e; e = pa_idxset_next(c->scache, &index))
        sum += e->memchunk.length;

    return sum;
}

static int unload_func(void *p, uint32_t index, int *del, void *userdata) {
    struct pa_scache_entry *e = p;
    time_t *now = userdata;
    assert(e);

    if (!e->auto_unload)
        return 0;

    if (e->last_used_time + e->core->scache_idle_time > *now)
        return 0;

    free_entry(e);
    *del = 1;
    return 0;
}

void pa_scache_unload_unused(struct pa_core *c) {
    time_t now;
    assert(c);

    if (!c->scache)
        return;
    
    time(&now);

    pa_idxset_foreach(c->scache, unload_func, &now);
}
