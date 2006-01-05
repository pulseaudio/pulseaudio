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

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <glob.h>

#include "scache.h"
#include "sink-input.h"
#include "mainloop.h"
#include "sample-util.h"
#include "play-memchunk.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "namereg.h"
#include "sound-file.h"
#include "util.h"
#include "log.h"

#define UNLOAD_POLL_TIME 2

static void timeout_callback(struct pa_mainloop_api *m, struct pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct pa_core *c = userdata;
    struct timeval ntv;
    assert(c && c->mainloop == m && c->scache_auto_unload_event == e);

    pa_scache_unload_unused(c);

    pa_gettimeofday(&ntv);
    ntv.tv_sec += UNLOAD_POLL_TIME;
    m->time_restart(e, &ntv);
}

static void free_entry(struct pa_scache_entry *e) {
    assert(e);
    pa_namereg_unregister(e->core, e->name);
    pa_subscription_post(e->core, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_REMOVE, e->index);
    pa_xfree(e->name);
    pa_xfree(e->filename);
    if (e->memchunk.memblock)
        pa_memblock_unref(e->memchunk.memblock);
    pa_xfree(e);
}

static struct pa_scache_entry* scache_add_item(struct pa_core *c, const char *name) {
    struct pa_scache_entry *e;
    assert(c && name);

    if ((e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 0))) {
        if (e->memchunk.memblock)
            pa_memblock_unref(e->memchunk.memblock);

        pa_xfree(e->filename);
        
        assert(e->core == c);

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);
    } else {
        e = pa_xmalloc(sizeof(struct pa_scache_entry));

        if (!pa_namereg_register(c, name, PA_NAMEREG_SAMPLE, e, 1)) {
            pa_xfree(e);
            return NULL;
        }

        e->name = pa_xstrdup(name);
        e->core = c;

        if (!c->scache) {
            c->scache = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
            assert(c->scache);
        }

        pa_idxset_put(c->scache, e, &e->index);

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_NEW, e->index);
    }

    e->volume = PA_VOLUME_NORM;
    e->last_used_time = 0;
    e->memchunk.memblock = NULL;
    e->memchunk.index = e->memchunk.length = 0;
    e->filename = NULL;
    e->lazy = 0;
    e->last_used_time = 0;

    memset(&e->sample_spec, 0, sizeof(struct pa_sample_spec));

    return e;
}

int pa_scache_add_item(struct pa_core *c, const char *name, struct pa_sample_spec *ss, struct pa_memchunk *chunk, uint32_t *index) {
    struct pa_scache_entry *e;
    assert(c && name);

    if (!(e = scache_add_item(c, name)))
        return -1;

    if (ss)
        e->sample_spec = *ss;

    if (chunk) {
        e->memchunk = *chunk;
        pa_memblock_ref(e->memchunk.memblock);
    }

    if (index)
        *index = e->index;

    return 0;
}

int pa_scache_add_file(struct pa_core *c, const char *name, const char *filename, uint32_t *index) {
    struct pa_sample_spec ss;
    struct pa_memchunk chunk;
    int r;

    if (pa_sound_file_load(filename, &ss, &chunk, c->memblock_stat) < 0)
        return -1;
        
    r = pa_scache_add_item(c, name, &ss, &chunk, index);
    pa_memblock_unref(chunk.memblock);

    return r;
}

int pa_scache_add_file_lazy(struct pa_core *c, const char *name, const char *filename, uint32_t *index) {
    struct pa_scache_entry *e;
    assert(c && name);

    if (!(e = scache_add_item(c, name)))
        return -1;

    e->lazy = 1;
    e->filename = pa_xstrdup(filename);
    
    if (!c->scache_auto_unload_event) {
        struct timeval ntv;
        pa_gettimeofday(&ntv);
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
    char *t;
    assert(c && name && sink);

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 1)))
        return -1;

    if (e->lazy && !e->memchunk.memblock) {
        if (pa_sound_file_load(e->filename, &e->sample_spec, &e->memchunk, c->memblock_stat) < 0)
            return -1;

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);
    }
    
    if (!e->memchunk.memblock)
        return -1;

    t = pa_sprintf_malloc("sample:%s", name);
    if (pa_play_memchunk(sink, t, &e->sample_spec, &e->memchunk, pa_volume_multiply(volume, e->volume)) < 0) {
        free(t);
        return -1;
    }

    free(t);

    if (e->lazy)
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

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 0)))
        return PA_IDXSET_INVALID;

    return e->index;
}

uint32_t pa_scache_total_size(struct pa_core *c) {
    struct pa_scache_entry *e;
    uint32_t index, sum = 0;
    assert(c);

    if (!c->scache || !pa_idxset_ncontents(c->scache))
        return 0;
    
    for (e = pa_idxset_first(c->scache, &index); e; e = pa_idxset_next(c->scache, &index))
        if (e->memchunk.memblock)
            sum += e->memchunk.length;

    return sum;
}

void pa_scache_unload_unused(struct pa_core *c) {
    struct pa_scache_entry *e;
    time_t now;
    uint32_t index;
    assert(c);

    if (!c->scache || !pa_idxset_ncontents(c->scache))
        return;
    
    time(&now);

    for (e = pa_idxset_first(c->scache, &index); e; e = pa_idxset_next(c->scache, &index)) {

        if (!e->lazy || !e->memchunk.memblock)
            continue;

        if (e->last_used_time + c->scache_idle_time > now)
            continue;
        
        pa_memblock_unref(e->memchunk.memblock);
        e->memchunk.memblock = NULL;
        e->memchunk.index = e->memchunk.length = 0;

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);
    }
}

static void add_file(struct pa_core *c, const char *pathname) {
    struct stat st;
    const char *e;

    if (!(e = strrchr(pathname, '/')))
        e = pathname;
    else
        e++;
    
    if (stat(pathname, &st) < 0) {
        pa_log(__FILE__": stat('%s') failed: %s\n", pathname, strerror(errno));
        return;
    }

    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
        pa_scache_add_file_lazy(c, e, pathname, NULL);
}

int pa_scache_add_directory_lazy(struct pa_core *c, const char *pathname) {
    DIR *dir;
    assert(c && pathname);

    /* First try to open this as directory */
    if (!(dir = opendir(pathname))) {
        glob_t p;
        unsigned int i;
        /* If that fails, try to open it as shell glob */

        if (glob(pathname, GLOB_ERR|GLOB_NOSORT, NULL, &p) < 0) {
            pa_log(__FILE__": Failed to open directory: %s\n", strerror(errno));
            return -1;
        }

        for (i = 0; i < p.gl_pathc; i++)
            add_file(c, p.gl_pathv[i]);
        
        globfree(&p);
    } else {
        struct dirent *e;

        while ((e = readdir(dir))) {
            char p[PATH_MAX];

            if (e->d_name[0] == '.')
                continue;

            snprintf(p, sizeof(p), "%s/%s", pathname, e->d_name);
            add_file(c, p);
        }
    }

    closedir(dir);
    return 0;
}
