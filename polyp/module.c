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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include "module.h"
#include "xmalloc.h"

#define UNLOAD_POLL_TIME 10

static void timeout_callback(struct pa_mainloop_api *m, void *id, const struct timeval *tv, void *userdata) {
    struct pa_core *c = userdata;
    struct timeval ntv;
    assert(c && c->mainloop == m && c->auto_unload_mainloop_source == id);

    pa_module_unload_unused(c);

    gettimeofday(&ntv, NULL);
    ntv.tv_sec += UNLOAD_POLL_TIME;
    m->enable_time(m, id, &ntv);
}

struct pa_module* pa_module_load(struct pa_core *c, const char *name, const char *argument) {
    struct pa_module *m = NULL;
    int r;
    
    assert(c && name);

    m = pa_xmalloc(sizeof(struct pa_module));

    m->name = pa_xstrdup(name);
    m->argument = pa_xstrdup(argument);
    
    if (!(m->dl = lt_dlopenext(name)))
        goto fail;

    if (!(m->init = lt_dlsym(m->dl, "pa_module_init")))
        goto fail;

    if (!(m->done = lt_dlsym(m->dl, "pa_module_done")))
        goto fail;
    
    m->userdata = NULL;
    m->core = c;
    m->n_used = -1;
    m->auto_unload = 0;

    assert(m->init);
    if (m->init(c, m) < 0)
        goto fail;

    if (!c->modules)
        c->modules = pa_idxset_new(NULL, NULL);

    if (!c->auto_unload_mainloop_source) {
        struct timeval ntv;
        gettimeofday(&ntv, NULL);
        ntv.tv_sec += UNLOAD_POLL_TIME;
        c->auto_unload_mainloop_source = c->mainloop->source_time(c->mainloop, &ntv, timeout_callback, c);
    }
    assert(c->auto_unload_mainloop_source);
    
    assert(c->modules);
    r = pa_idxset_put(c->modules, m, &m->index);
    assert(r >= 0 && m->index != PA_IDXSET_INVALID);

    fprintf(stderr, "module: loaded %u \"%s\" with argument \"%s\".\n", m->index, m->name, m->argument);
    
    return m;
    
fail:
    if (m) {
        pa_xfree(m->argument);
        pa_xfree(m->name);
        
        if (m->dl)
            lt_dlclose(m->dl);

        pa_xfree(m);
    }

    return NULL;
}

static void pa_module_free(struct pa_module *m) {
    assert(m && m->done && m->core);
    m->done(m->core, m);

    lt_dlclose(m->dl);
    
    fprintf(stderr, "module: unloaded %u \"%s\".\n", m->index, m->name);

    pa_xfree(m->name);
    pa_xfree(m->argument);
    pa_xfree(m);
}


void pa_module_unload(struct pa_core *c, struct pa_module *m) {
    assert(c && m);

    assert(c->modules);
    if (!(m = pa_idxset_remove_by_data(c->modules, m, NULL)))
        return;

    pa_module_free(m);
}

void pa_module_unload_by_index(struct pa_core *c, uint32_t index) {
    struct pa_module *m;
    assert(c && index != PA_IDXSET_INVALID);

    assert(c->modules);
    if (!(m = pa_idxset_remove_by_index(c->modules, index)))
        return;

    pa_module_free(m);
}

static void free_callback(void *p, void *userdata) {
    struct pa_module *m = p;
    assert(m);
    pa_module_free(m);
}

void pa_module_unload_all(struct pa_core *c) {
    assert(c);

    if (!c->modules)
        return;

    pa_idxset_free(c->modules, free_callback, NULL);
    c->modules = NULL;

    if (c->auto_unload_mainloop_source)
        c->mainloop->cancel_time(c->mainloop, c->auto_unload_mainloop_source);
    c->auto_unload_mainloop_source = NULL;
}

static int unused_callback(void *p, uint32_t index, int *del, void *userdata) {
    struct pa_module *m = p;
    time_t *now = userdata;
    assert(p && del && now);
    
    if (m->n_used == 0 && m->auto_unload && m->last_used_time+m->core->auto_unload_time <= *now) {
        pa_module_free(m);
        *del = 1;
    }

    return 0;
}

void pa_module_unload_unused(struct pa_core *c) {
    time_t now;
    assert(c);

    if (!c->modules)
        return;
    
    time(&now);
    pa_idxset_foreach(c->modules, unused_callback, &now);
}

struct once_info {
    struct pa_core *core;
    uint32_t index;
};
    
static void module_unload_once_callback(void *userdata) {
    struct once_info *i = userdata;
    assert(i);
    pa_module_unload_by_index(i->core, i->index);
    pa_xfree(i);
}

void pa_module_unload_request(struct pa_core *c, struct pa_module *m) {
    struct once_info *i;
    assert(c && m);

    i = pa_xmalloc(sizeof(struct once_info));
    i->core = c;
    i->index = m->index;
    pa_mainloop_api_once(c->mainloop, module_unload_once_callback, i);
}

void pa_module_set_used(struct pa_module*m, int used) {
    assert(m);

    if (m->n_used != used && used == 0)
        time(&m->last_used_time);

    m->n_used = used;
}

