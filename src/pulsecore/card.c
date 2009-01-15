/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

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
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/namereg.h>

#include "card.h"

pa_card_config *pa_card_config_new(const char *name) {
    pa_card_config *c;

    pa_assert(name);

    c = pa_xnew0(pa_card_config, 1);
    c->name = pa_xstrdup(name);

    return c;
}

void pa_card_config_free(pa_card_config *c) {
    pa_assert(c);

    pa_xfree(c->name);
    pa_xfree(c);
}

pa_card_new_data* pa_card_new_data_init(pa_card_new_data *data) {
    pa_assert(data);

    memset(data, 0, sizeof(*data));
    data->proplist = pa_proplist_new();

    return data;
}

void pa_card_new_data_set_name(pa_card_new_data *data, const char *name) {
    pa_assert(data);

    pa_xfree(data->name);
    data->name = pa_xstrdup(name);
}

void pa_card_new_data_done(pa_card_new_data *data) {

    pa_assert(data);

    pa_proplist_free(data->proplist);

    if (data->configs) {
        pa_card_config *c;

        while ((c = pa_hashmap_steal_first(data->configs)))
            pa_card_config_free(c);

        pa_hashmap_free(data->configs, NULL, NULL);
    }

    pa_xfree(data->name);
}

pa_card *pa_card_new(pa_core *core, pa_card_new_data *data) {
    pa_card *c;
    const char *name;

    pa_core_assert_ref(core);
    pa_assert(data);
    pa_assert(data->name);

    c = pa_xnew(pa_card, 1);

    if (!(name = pa_namereg_register(core, data->name, PA_NAMEREG_CARD, c, data->namereg_fail))) {
        pa_xfree(c);
        return NULL;
    }

    pa_card_new_data_set_name(data, name);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_CARD_NEW], data) < 0) {
        pa_xfree(c);
        pa_namereg_unregister(core, name);
        return NULL;
    }

    c->core = core;
    c->name = pa_xstrdup(data->name);
    c->proplist = pa_proplist_copy(data->proplist);
    c->driver = pa_xstrdup(data->driver);
    c->module = data->module;

    c->sinks = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    c->sources = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    c->configs = data->configs;
    data->configs = NULL;
    c->active_config = data->active_config;
    data->active_config = NULL;

    c->userdata = NULL;
    c->set_config = NULL;

    pa_assert_se(pa_idxset_put(core->cards, c, &c->index) >= 0);

    pa_log_info("Created %u \"%s\"", c->index, c->name);
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_NEW, c->index);

    pa_hook_fire(&core->hooks[PA_CORE_HOOK_CARD_PUT], c);
    return c;
}

void pa_card_free(pa_card *c) {
    pa_core *core;
    pa_card_config *config;

    pa_assert(c);
    pa_assert(c->core);

    core = c->core;

    pa_hook_fire(&core->hooks[PA_CORE_HOOK_CARD_UNLINK], c);

    pa_namereg_unregister(core, c->name);

    pa_idxset_remove_by_data(c->core->cards, c, NULL);

    pa_log_info("Freed %u \"%s\"", c->index, c->name);

    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_REMOVE, c->index);

    pa_idxset_free(c->sinks, NULL, NULL);
    pa_idxset_free(c->sources, NULL, NULL);

    while ((config = pa_hashmap_steal_first(c->configs)))
        pa_card_config_free(config);

    pa_hashmap_free(c->configs, NULL, NULL);

    pa_proplist_free(c->proplist);
    pa_xfree(c->driver);
    pa_xfree(c->name);
    pa_xfree(c);

    pa_core_check_idle(core);
}

int pa_card_set_config(pa_card *c, const char *name) {
    pa_card_config *config;
    pa_assert(c);

    if (!c->set_config) {
        pa_log_warn("set_config() operation not implemented for card %u", c->index);
        return -1;
    }

    if (!c->configs)
        return -1;

    if (!(config = pa_hashmap_get(c->configs, name)))
        return -1;

    if (c->set_config(c, config) < 0)
        return -1;

    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE, c->index);

    return 0;
}
