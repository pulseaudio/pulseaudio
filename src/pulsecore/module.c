/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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
#include <pulse/proplist.h>

#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/ltdl-helper.h>
#include <pulsecore/modinfo.h>

#include "module.h"

#define PA_SYMBOL_INIT "pa__init"
#define PA_SYMBOL_DONE "pa__done"
#define PA_SYMBOL_LOAD_ONCE "pa__load_once"
#define PA_SYMBOL_GET_N_USED "pa__get_n_used"
#define PA_SYMBOL_GET_DEPRECATE "pa__get_deprecated"

pa_module* pa_module_load(pa_core *c, const char *name, const char *argument) {
    pa_module *m = NULL;
    pa_bool_t (*load_once)(void);
    const char* (*get_deprecated)(void);
    pa_modinfo *mi;

    pa_assert(c);
    pa_assert(name);

    if (c->disallow_module_loading)
        goto fail;

    m = pa_xnew(pa_module, 1);
    m->name = pa_xstrdup(name);
    m->argument = pa_xstrdup(argument);
    m->load_once = FALSE;
    m->proplist = pa_proplist_new();

    if (!(m->dl = lt_dlopenext(name))) {
        pa_log("Failed to open module \"%s\": %s", name, lt_dlerror());
        goto fail;
    }

    if ((load_once = (pa_bool_t (*)(void)) pa_load_sym(m->dl, name, PA_SYMBOL_LOAD_ONCE))) {

        m->load_once = load_once();

        if (m->load_once) {
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

    if ((get_deprecated = (const char* (*) (void)) pa_load_sym(m->dl, name, PA_SYMBOL_GET_DEPRECATE))) {
        const char *t;

        if ((t = get_deprecated()))
            pa_log_warn("%s is deprecated: %s", name, t);
    }

    if (!(m->init = (int (*)(pa_module*_m)) pa_load_sym(m->dl, name, PA_SYMBOL_INIT))) {
        pa_log("Failed to load module \"%s\": symbol \""PA_SYMBOL_INIT"\" not found.", name);
        goto fail;
    }

    m->done = (void (*)(pa_module*_m)) pa_load_sym(m->dl, name, PA_SYMBOL_DONE);
    m->get_n_used = (int (*)(pa_module*_m)) pa_load_sym(m->dl, name, PA_SYMBOL_GET_N_USED);
    m->userdata = NULL;
    m->core = c;
    m->unload_requested = FALSE;

    if (m->init(m) < 0) {
        pa_log_error("Failed to load  module \"%s\" (argument: \"%s\"): initialization failed.", name, argument ? argument : "");
        goto fail;
    }

    pa_assert_se(pa_idxset_put(c->modules, m, &m->index) >= 0);
    pa_assert(m->index != PA_IDXSET_INVALID);

    pa_log_info("Loaded \"%s\" (index: #%u; argument: \"%s\").", m->name, m->index, m->argument ? m->argument : "");

    pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_NEW, m->index);

    if ((mi = pa_modinfo_get_by_handle(m->dl, name))) {

        if (mi->author && !pa_proplist_contains(m->proplist, PA_PROP_MODULE_AUTHOR))
            pa_proplist_sets(m->proplist, PA_PROP_MODULE_AUTHOR, mi->author);

        if (mi->description && !pa_proplist_contains(m->proplist, PA_PROP_MODULE_DESCRIPTION))
            pa_proplist_sets(m->proplist, PA_PROP_MODULE_DESCRIPTION, mi->description);

        if (mi->version && !pa_proplist_contains(m->proplist, PA_PROP_MODULE_VERSION))
            pa_proplist_sets(m->proplist, PA_PROP_MODULE_VERSION, mi->version);

        pa_modinfo_free(mi);
    }

    return m;

fail:

    if (m) {
        if (m->proplist)
            pa_proplist_free(m->proplist);

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

    pa_log_info("Unloading \"%s\" (index: #%u).", m->name, m->index);

    if (m->done)
        m->done(m);

    if (m->proplist)
        pa_proplist_free(m->proplist);

    lt_dlclose(m->dl);

    pa_log_info("Unloaded \"%s\" (index: #%u).", m->name, m->index);

    pa_subscription_post(m->core, PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_REMOVE, m->index);

    pa_xfree(m->name);
    pa_xfree(m->argument);
    pa_xfree(m);
}

void pa_module_unload(pa_core *c, pa_module *m, pa_bool_t force) {
    pa_assert(c);
    pa_assert(m);

    if (m->core->disallow_module_loading && !force)
        return;

    if (!(m = pa_idxset_remove_by_data(c->modules, m, NULL)))
        return;

    pa_module_free(m);
}

void pa_module_unload_by_index(pa_core *c, uint32_t idx, pa_bool_t force) {
    pa_module *m;
    pa_assert(c);
    pa_assert(idx != PA_IDXSET_INVALID);

    if (c->disallow_module_loading && !force)
        return;

    if (!(m = pa_idxset_remove_by_index(c->modules, idx)))
        return;

    pa_module_free(m);
}

void pa_module_unload_all(pa_core *c) {
    pa_module *m;
    pa_assert(c);

    while ((m = pa_idxset_steal_first(c->modules, NULL)))
        pa_module_free(m);

    if (c->module_defer_unload_event) {
        c->mainloop->defer_free(c->module_defer_unload_event);
        c->module_defer_unload_event = NULL;
    }
}

static void defer_cb(pa_mainloop_api*api, pa_defer_event *e, void *userdata) {
    void *state = NULL;
    pa_core *c = PA_CORE(userdata);
    pa_module *m;

    pa_core_assert_ref(c);
    api->defer_enable(e, 0);

    while ((m = pa_idxset_iterate(c->modules, &state, NULL)))
        if (m->unload_requested)
            pa_module_unload(c, m, TRUE);
}

void pa_module_unload_request(pa_module *m, pa_bool_t force) {
    pa_assert(m);

    if (m->core->disallow_module_loading && !force)
        return;

    m->unload_requested = TRUE;

    if (!m->core->module_defer_unload_event)
        m->core->module_defer_unload_event = m->core->mainloop->defer_new(m->core->mainloop, defer_cb, m->core);

    m->core->mainloop->defer_enable(m->core->module_defer_unload_event, 1);
}

void pa_module_unload_request_by_index(pa_core *c, uint32_t idx, pa_bool_t force) {
    pa_module *m;
    pa_assert(c);

    if (!(m = pa_idxset_get_by_index(c->modules, idx)))
        return;

    pa_module_unload_request(m, force);
}

int pa_module_get_n_used(pa_module*m) {
    pa_assert(m);

    if (!m->get_n_used)
        return -1;

    return m->get_n_used(m);
}
