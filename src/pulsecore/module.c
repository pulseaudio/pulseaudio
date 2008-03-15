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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/ltdl-helper.h>

#include "module.h"

#define PA_SYMBOL_INIT "pa__init"
#define PA_SYMBOL_DONE "pa__done"
#define PA_SYMBOL_LOAD_ONCE "pa__load_once"

#define UNLOAD_POLL_TIME 2

static void timeout_callback(pa_mainloop_api *m, pa_time_event*e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    pa_core *c = PA_CORE(userdata);
    struct timeval ntv;

    pa_core_assert_ref(c);
    pa_assert(c->mainloop == m);
    pa_assert(c->module_auto_unload_event == e);

    pa_module_unload_unused(c);

    pa_gettimeofday(&ntv);
    pa_timeval_add(&ntv, UNLOAD_POLL_TIME*1000000);
    m->time_restart(e, &ntv);
}

pa_module* pa_module_load(pa_core *c, const char *name, const char *argument) {
    pa_module *m = NULL;
    pa_bool_t (*load_once)(void);

    pa_assert(c);
    pa_assert(name);

    if (c->disallow_module_loading)
        goto fail;

    m = pa_xnew(pa_module, 1);
    m->name = pa_xstrdup(name);
    m->argument = pa_xstrdup(argument);

    if (!(m->dl = lt_dlopenext(name))) {
        pa_log("Failed to open module \"%s\": %s", name, lt_dlerror());
        goto fail;
    }

    if ((load_once = (pa_bool_t (*)(void)) pa_load_sym(m->dl, name, PA_SYMBOL_LOAD_ONCE))) {

        if (load_once() && c->modules) {
            pa_module *i;
            uint32_t idx;
            /* OK, the module only wants to be loaded once, let's make sure it is */

            for (i = pa_idxset_first(c->modules, &idx); i; i = pa_idxset_next(c->modules, &idx)) {
                if (strcmp(name, i->name) == 0) {
                    pa_log("Module \"%s\" should be loaded once at most. Refusing to load.", name);
                    goto fail;
                }
            }
        }
    }

    if (!(m->init = (int (*)(pa_module*_m)) pa_load_sym(m->dl, name, PA_SYMBOL_INIT))) {
        pa_log("Failed to load module \"%s\": symbol \""PA_SYMBOL_INIT"\" not found.", name);
        goto fail;
    }

    m->done = (void (*)(pa_module*_m)) pa_load_sym(m->dl, name, PA_SYMBOL_DONE);
    m->userdata = NULL;
    m->core = c;
    m->n_used = -1;
    m->auto_unload = 0;
    m->unload_requested = 0;

    if (m->init(m) < 0) {
        pa_log_error("Failed to load  module \"%s\" (argument: \"%s\"): initialization failed.", name, argument ? argument : "");
        goto fail;
    }

    if (!c->modules)
        c->modules = pa_idxset_new(NULL, NULL);

    if (!c->module_auto_unload_event) {
        struct timeval ntv;
        pa_gettimeofday(&ntv);
        pa_timeval_add(&ntv, UNLOAD_POLL_TIME*1000000);
        c->module_auto_unload_event = c->mainloop->time_new(c->mainloop, &ntv, timeout_callback, c);
    }

    pa_assert_se(pa_idxset_put(c->modules, m, &m->index) >= 0);
    pa_assert(m->index != PA_IDXSET_INVALID);

    pa_log_info("Loaded \"%s\" (index: #%u; argument: \"%s\").", m->name, m->index, m->argument ? m->argument : "");

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

static void pa_module_free(pa_module *m) {
    pa_assert(m);
    pa_assert(m->core);

    if (m->core->disallow_module_loading)
        return;

    pa_log_info("Unloading \"%s\" (index: #%u).", m->name, m->index);

    if (m->done)
        m->done(m);

    lt_dlclose(m->dl);

    pa_log_info("Unloaded \"%s\" (index: #%u).", m->name, m->index);

    pa_subscription_post(m->core, PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_REMOVE, m->index);

    pa_xfree(m->name);
    pa_xfree(m->argument);
    pa_xfree(m);
}

void pa_module_unload(pa_core *c, pa_module *m) {
    pa_assert(c);
    pa_assert(m);

    pa_assert(c->modules);
    if (!(m = pa_idxset_remove_by_data(c->modules, m, NULL)))
        return;

    pa_module_free(m);
}

void pa_module_unload_by_index(pa_core *c, uint32_t idx) {
    pa_module *m;
    pa_assert(c);
    pa_assert(idx != PA_IDXSET_INVALID);

    if (!(m = pa_idxset_remove_by_index(c->modules, idx)))
        return;

    pa_module_free(m);
}

static void free_callback(void *p, PA_GCC_UNUSED void *userdata) {
    pa_module *m = p;
    pa_assert(m);
    pa_module_free(m);
}

void pa_module_unload_all(pa_core *c) {
    pa_module *m;

    pa_assert(c);

    if (!c->modules)
        return;

    while ((m = pa_idxset_first(c->modules, NULL)))
        pa_module_unload(c, m);

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

static int unused_callback(void *p, PA_GCC_UNUSED uint32_t idx, int *del, void *userdata) {
    pa_module *m = p;
    time_t *now = userdata;

    pa_assert(m);
    pa_assert(del);
    pa_assert(now);

    if (m->n_used == 0 && m->auto_unload && m->last_used_time+m->core->module_idle_time <= *now) {
        pa_module_free(m);
        *del = 1;
    }

    return 0;
}

void pa_module_unload_unused(pa_core *c) {
    time_t now;
    pa_assert(c);

    if (!c->modules)
        return;

    time(&now);
    pa_idxset_foreach(c->modules, unused_callback, &now);
}

static int unload_callback(void *p, PA_GCC_UNUSED uint32_t idx, int *del, PA_GCC_UNUSED void *userdata) {
    pa_module *m = p;
    pa_assert(m);

    if (m->unload_requested) {
        pa_module_free(m);
        *del = 1;
    }

    return 0;
}

static void defer_cb(pa_mainloop_api*api, pa_defer_event *e, void *userdata) {
    pa_core *core = PA_CORE(userdata);

    pa_core_assert_ref(core);
    api->defer_enable(e, 0);

    if (!core->modules)
        return;

    pa_idxset_foreach(core->modules, unload_callback, NULL);
}

void pa_module_unload_request(pa_module *m) {
    pa_assert(m);

    m->unload_requested = 1;

    if (!m->core->module_defer_unload_event)
        m->core->module_defer_unload_event = m->core->mainloop->defer_new(m->core->mainloop, defer_cb, m->core);

    m->core->mainloop->defer_enable(m->core->module_defer_unload_event, 1);
}

void pa_module_set_used(pa_module*m, int used) {
    pa_assert(m);

    if (m->n_used != used)
        pa_subscription_post(m->core, PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_CHANGE, m->index);

    if (used == 0 && m->n_used > 0)
        time(&m->last_used_time);

    m->n_used = used;
}

pa_modinfo *pa_module_get_info(pa_module *m) {
    pa_assert(m);

    return pa_modinfo_get_by_handle(m->dl, m->name);
}
