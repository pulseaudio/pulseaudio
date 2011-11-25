/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/namereg.h>
#include <pulsecore/device-port.h>

#include "card.h"

pa_card_profile *pa_card_profile_new(const char *name, const char *description, size_t extra) {
    pa_card_profile *c;

    pa_assert(name);

    c = pa_xmalloc(PA_ALIGN(sizeof(pa_card_profile)) + extra);
    c->name = pa_xstrdup(name);
    c->description = pa_xstrdup(description);

    c->priority = 0;
    c->n_sinks = c->n_sources = 0;
    c->max_sink_channels = c->max_source_channels = 0;

    return c;
}

void pa_card_profile_free(pa_card_profile *c) {
    pa_assert(c);

    pa_xfree(c->name);
    pa_xfree(c->description);
    pa_xfree(c);
}

pa_card_new_data* pa_card_new_data_init(pa_card_new_data *data) {
    pa_assert(data);

    memset(data, 0, sizeof(*data));
    data->proplist = pa_proplist_new();
    data->ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    return data;
}

void pa_card_new_data_set_name(pa_card_new_data *data, const char *name) {
    pa_assert(data);

    pa_xfree(data->name);
    data->name = pa_xstrdup(name);
}

void pa_card_new_data_set_profile(pa_card_new_data *data, const char *profile) {
    pa_assert(data);

    pa_xfree(data->active_profile);
    data->active_profile = pa_xstrdup(profile);
}

void pa_card_new_data_done(pa_card_new_data *data) {

    pa_assert(data);

    pa_proplist_free(data->proplist);

    if (data->profiles) {
        pa_card_profile *c;

        while ((c = pa_hashmap_steal_first(data->profiles)))
            pa_card_profile_free(c);

        pa_hashmap_free(data->profiles, NULL, NULL);
    }

    if (data->ports)
        pa_device_port_hashmap_free(data->ports);

    pa_xfree(data->name);
    pa_xfree(data->active_profile);
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
    c->driver = pa_xstrdup(pa_path_get_filename(data->driver));
    c->module = data->module;

    c->sinks = pa_idxset_new(NULL, NULL);
    c->sources = pa_idxset_new(NULL, NULL);

    /* As a minor optimization we just steal the list instead of
     * copying it here */
    c->profiles = data->profiles;
    data->profiles = NULL;
    c->ports = data->ports;
    data->ports = NULL;

    c->active_profile = NULL;
    c->save_profile = FALSE;

    if (data->active_profile && c->profiles)
        if ((c->active_profile = pa_hashmap_get(c->profiles, data->active_profile)))
            c->save_profile = data->save_profile;

    if (!c->active_profile && c->profiles) {
        void *state;
        pa_card_profile *p;

        PA_HASHMAP_FOREACH(p, c->profiles, state)
            if (!c->active_profile || p->priority > c->active_profile->priority)
                c->active_profile = p;
    }

    c->userdata = NULL;
    c->set_profile = NULL;

    pa_device_init_description(c->proplist);
    pa_device_init_icon(c->proplist, TRUE);
    pa_device_init_intended_roles(c->proplist);

    pa_assert_se(pa_idxset_put(core->cards, c, &c->index) >= 0);

    pa_log_info("Created %u \"%s\"", c->index, c->name);
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_NEW, c->index);

    pa_hook_fire(&core->hooks[PA_CORE_HOOK_CARD_PUT], c);
    return c;
}

void pa_card_free(pa_card *c) {
    pa_core *core;

    pa_assert(c);
    pa_assert(c->core);

    core = c->core;

    pa_hook_fire(&core->hooks[PA_CORE_HOOK_CARD_UNLINK], c);

    pa_namereg_unregister(core, c->name);

    pa_idxset_remove_by_data(c->core->cards, c, NULL);

    pa_log_info("Freed %u \"%s\"", c->index, c->name);

    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_REMOVE, c->index);

    pa_assert(pa_idxset_isempty(c->sinks));
    pa_idxset_free(c->sinks, NULL, NULL);
    pa_assert(pa_idxset_isempty(c->sources));
    pa_idxset_free(c->sources, NULL, NULL);

    pa_device_port_hashmap_free(c->ports);

    if (c->profiles) {
        pa_card_profile *p;

        while ((p = pa_hashmap_steal_first(c->profiles)))
            pa_card_profile_free(p);

        pa_hashmap_free(c->profiles, NULL, NULL);
    }

    pa_proplist_free(c->proplist);
    pa_xfree(c->driver);
    pa_xfree(c->name);
    pa_xfree(c);
}

int pa_card_set_profile(pa_card *c, const char *name, pa_bool_t save) {
    pa_card_profile *profile;
    int r;
    pa_assert(c);

    if (!c->set_profile) {
        pa_log_debug("set_profile() operation not implemented for card %u \"%s\"", c->index, c->name);
        return -PA_ERR_NOTIMPLEMENTED;
    }

    if (!c->profiles)
        return -PA_ERR_NOENTITY;

    if (!(profile = pa_hashmap_get(c->profiles, name)))
        return -PA_ERR_NOENTITY;

    if (c->active_profile == profile) {
        c->save_profile = c->save_profile || save;
        return 0;
    }

    if ((r = c->set_profile(c, profile)) < 0)
        return r;

    pa_subscription_post(c->core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE, c->index);

    pa_log_info("Changed profile of card %u \"%s\" to %s", c->index, c->name, profile->name);

    c->active_profile = profile;
    c->save_profile = save;

    pa_hook_fire(&c->core->hooks[PA_CORE_HOOK_CARD_PROFILE_CHANGED], c);

    return 0;
}

int pa_card_suspend(pa_card *c, pa_bool_t suspend, pa_suspend_cause_t cause) {
    pa_sink *sink;
    pa_source *source;
    uint32_t idx;
    int ret = 0;

    pa_assert(c);
    pa_assert(cause != 0);

    for (sink = pa_idxset_first(c->sinks, &idx); sink; sink = pa_idxset_next(c->sinks, &idx)) {
        int r;

        if ((r = pa_sink_suspend(sink, suspend, cause)) < 0)
            ret = r;
    }

    for (source = pa_idxset_first(c->sources, &idx); source; source = pa_idxset_next(c->sources, &idx)) {
        int r;

        if ((r = pa_source_suspend(source, suspend, cause)) < 0)
            ret = r;
    }

    return ret;
}
