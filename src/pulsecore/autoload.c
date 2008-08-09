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

#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sound-file.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/core-subscribe.h>

#include "autoload.h"

static void entry_free(pa_autoload_entry *e) {
    pa_assert(e);
    pa_subscription_post(e->core, PA_SUBSCRIPTION_EVENT_AUTOLOAD|PA_SUBSCRIPTION_EVENT_REMOVE, PA_INVALID_INDEX);
    pa_xfree(e->name);
    pa_xfree(e->module);
    pa_xfree(e->argument);
    pa_xfree(e);
}

static void entry_remove_and_free(pa_autoload_entry *e) {
    pa_assert(e);
    pa_assert(e->core);

    pa_idxset_remove_by_data(e->core->autoload_idxset, e, NULL);
    pa_hashmap_remove(e->core->autoload_hashmap, e->name);
    entry_free(e);
}

static pa_autoload_entry* entry_new(pa_core *c, const char *name) {
    pa_autoload_entry *e = NULL;

    pa_core_assert_ref(c);
    pa_assert(name);

    if (c->autoload_hashmap && (e = pa_hashmap_get(c->autoload_hashmap, name)))
        return NULL;

    e = pa_xnew(pa_autoload_entry, 1);
    e->core = c;
    e->name = pa_xstrdup(name);
    e->module = e->argument = NULL;
    e->in_action = 0;

    if (!c->autoload_hashmap)
        c->autoload_hashmap = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    pa_assert(c->autoload_hashmap);

    pa_hashmap_put(c->autoload_hashmap, e->name, e);

    if (!c->autoload_idxset)
        c->autoload_idxset = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    pa_idxset_put(c->autoload_idxset, e, &e->index);

    pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_AUTOLOAD|PA_SUBSCRIPTION_EVENT_NEW, e->index);

    return e;
}

int pa_autoload_add(pa_core *c, const char*name, pa_namereg_type_t type, const char*module, const char *argument, uint32_t *idx) {
    pa_autoload_entry *e = NULL;

    pa_assert(c);
    pa_assert(name);
    pa_assert(module);
    pa_assert(type == PA_NAMEREG_SINK || type == PA_NAMEREG_SOURCE);

    if (!(e = entry_new(c, name)))
        return -1;

    e->module = pa_xstrdup(module);
    e->argument = pa_xstrdup(argument);
    e->type = type;

    if (idx)
        *idx = e->index;

    return 0;
}

int pa_autoload_remove_by_name(pa_core *c, const char*name, pa_namereg_type_t type) {
    pa_autoload_entry *e;

    pa_assert(c);
    pa_assert(name);
    pa_assert(type == PA_NAMEREG_SINK || type == PA_NAMEREG_SOURCE);

    if (!c->autoload_hashmap || !(e = pa_hashmap_get(c->autoload_hashmap, name)) || e->type != type)
        return -1;

    entry_remove_and_free(e);
    return 0;
}

int pa_autoload_remove_by_index(pa_core *c, uint32_t idx) {
    pa_autoload_entry *e;

    pa_assert(c);
    pa_assert(idx != PA_IDXSET_INVALID);

    if (!c->autoload_idxset || !(e = pa_idxset_get_by_index(c->autoload_idxset, idx)))
        return -1;

    entry_remove_and_free(e);
    return 0;
}

void pa_autoload_request(pa_core *c, const char *name, pa_namereg_type_t type) {
    pa_autoload_entry *e;
    pa_module *m;

    pa_assert(c);
    pa_assert(name);

    if (!c->autoload_hashmap || !(e = pa_hashmap_get(c->autoload_hashmap, name)) || (e->type != type))
        return;

    if (e->in_action)
        return;

    e->in_action = 1;

    if (type == PA_NAMEREG_SINK || type == PA_NAMEREG_SOURCE) {
        if ((m = pa_module_load(c, e->module, e->argument)))
            m->auto_unload = 1;
    }

    e->in_action = 0;
}

static void free_func(void *p, void *userdata) {
    pa_autoload_entry *e = p;
    pa_idxset_remove_by_data(e->core->autoload_idxset, e, NULL);
    entry_free(e);
}

void pa_autoload_free(pa_core *c) {

    if (c->autoload_hashmap) {
        pa_hashmap_free(c->autoload_hashmap, free_func, NULL);
        c->autoload_hashmap = NULL;
    }

    if (c->autoload_idxset) {
        pa_idxset_free(c->autoload_idxset, NULL, NULL);
        c->autoload_idxset = NULL;
    }
}

const pa_autoload_entry* pa_autoload_get_by_name(pa_core *c, const char*name, pa_namereg_type_t type) {
    pa_autoload_entry *e;

    pa_core_assert_ref(c);
    pa_assert(name);

    if (!c->autoload_hashmap || !(e = pa_hashmap_get(c->autoload_hashmap, name)) || e->type != type)
        return NULL;

    return e;
}

const pa_autoload_entry* pa_autoload_get_by_index(pa_core *c, uint32_t idx) {
    pa_autoload_entry *e;

    pa_core_assert_ref(c);
    pa_assert(idx != PA_IDXSET_INVALID);

    if (!c->autoload_idxset || !(e = pa_idxset_get_by_index(c->autoload_idxset, idx)))
        return NULL;

    return e;
}
