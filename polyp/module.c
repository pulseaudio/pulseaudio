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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include "module.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "log.h"

#define PA_SYMBOL_INIT "pa__init"
#define PA_SYMBOL_DONE "pa__done"

#define UNLOAD_POLL_TIME 2

static void timeout_callback(struct pa_mainloop_api *m, struct pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct pa_core *c = userdata;
    struct timeval ntv;
    assert(c && c->mainloop == m && c->module_auto_unload_event == e);

    pa_module_unload_unused(c);

    gettimeofday(&ntv, NULL);
    ntv.tv_sec += UNLOAD_POLL_TIME;
    m->time_restart(e, &ntv);
}

struct pa_module* pa_module_load(struct pa_core *c, const char *name, const char *argument) {
    struct pa_module *m = NULL;
    int r;
    
    assert(c && name);

    if (c->disallow_module_loading)
        goto fail;
    
    m = pa_xmalloc(sizeof(struct pa_module));

    m->name = pa_xstrdup(name);
    m->argument = pa_xstrdup(argument);
    
    if (!(m->dl = lt_dlopenext(name))) {
        pa_log(__FILE__": Failed to open module \"%s\": %s\n", name, lt_dlerror());
        goto fail;
    }

    if (!(m->init = (int (*)(struct pa_core *c, struct pa_module*m)) lt_dlsym(m->dl, PA_SYMBOL_INIT))) {
        pa_log(__FILE__": Failed to load module \"%s\": symbol \""PA_SYMBOL_INIT"\" not found.\n", name);
        goto fail;
    }

    if (!(m->done = (void (*)(struct pa_core *c, struct pa_module*m)) lt_dlsym(m->dl, PA_SYMBOL_DONE))) {
        pa_log(__FILE__": Failed to load module \"%s\": symbol \""PA_SYMBOL_DONE"\" not found.\n", name);
        goto fail;
    }
    
    m->userdata = NULL;
    m->core = c;
    m->n_used = -1;
    m->auto_unload = 0;
    m->unload_requested = 0;

    assert(m->init);
    if (m->init(c, m) < 0) {
        pa_log(__FILE__": Failed to load  module \"%s\" (argument: \"%s\"): initialization failed.\n", name, argument ? argument : "");
        goto fail;
    }

    if (!c->modules)
        c->modules = pa_idxset_new(NULL, NULL);

    if (!c->module_auto_unload_event) {
        struct timeval ntv;
        gettimeofday(&ntv, NULL);
        ntv.tv_sec += UNLOAD_POLL_TIME;
        c->module_auto_unload_event = c->mainloop->time_new(c->mainloop, &ntv, timeout_callback, c);
    }
    assert(c->module_auto_unload_event);
    
    assert(c->modules);
    r = pa_idxset_put(c->modules, m, &m->index);
    assert(r >= 0 && m->index != PA_IDXSET_INVALID);

    pa_log(__FILE__": Loaded \"%s\" (index: #%u; argument: \"%s\").\n", m->name, m->index, m->argument ? m->argument : "");

    pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_NEW, m->index);
    
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

    if (m->core->disallow_module_loading)
        return;

    pa_log(__FILE__": Unloading \"%s\" (index: #%u).\n", m->name, m->index);

    m->done(m->core, m);

    lt_dlclose(m->dl);
    
    pa_log(__FILE__": Unloaded \"%s\" (index: #%u).\n", m->name, m->index);

    pa_subscription_post(m->core, PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_REMOVE, m->index);
    
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

    if (c->module_auto_unload_event) {
        c->mainloop->time_free(c->module_auto_unload_event);
        c->module_auto_unload_event = NULL;
    }

    if (c->module_defer_unload_event) {
        c->mainloop->defer_free(c->module_defer_unload_event);
        c->module_defer_unload_event = NULL;
    }
}

static int unused_callback(void *p, uint32_t index, int *del, void *userdata) {
    struct pa_module *m = p;
    time_t *now = userdata;
    assert(p && del && now);
    
    if (m->n_used == 0 && m->auto_unload && m->last_used_time+m->core->module_idle_time <= *now) {
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

static int unload_callback(void *p, uint32_t index, int *del, void *userdata) {
    struct pa_module *m = p;
    assert(m);

    if (m->unload_requested) {
        pa_module_free(m);
        *del = 1;
    }

    return 0;
}

static void defer_cb(struct pa_mainloop_api*api, struct pa_defer_event *e, void *userdata) {
    struct pa_core *core = userdata;
    api->defer_enable(e, 0);

    if (!core->modules)
        return;

    pa_idxset_foreach(core->modules, unload_callback, NULL);

}

void pa_module_unload_request(struct pa_module *m) {
    assert(m);

    m->unload_requested = 1;

    if (!m->core->module_defer_unload_event)
        m->core->module_defer_unload_event = m->core->mainloop->defer_new(m->core->mainloop, defer_cb, m->core);

    m->core->mainloop->defer_enable(m->core->module_defer_unload_event, 1);
}

void pa_module_set_used(struct pa_module*m, int used) {
    assert(m);

    if (m->n_used != used)
        pa_subscription_post(m->core, PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_CHANGE, m->index);
    
    if (m->n_used != used && used == 0)
        time(&m->last_used_time);

    m->n_used = used;
}

struct pa_modinfo *pa_module_get_info(struct pa_module *m) {
    assert(m);

    return pa_modinfo_get_by_handle(m->dl);
}
