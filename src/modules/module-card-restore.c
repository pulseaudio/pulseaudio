/***
  This file is part of PulseAudio.

  Copyright 2006-2008 Lennart Poettering

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

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <gdbm.h>

#include <pulse/xmalloc.h>
#include <pulse/volume.h>
#include <pulse/timeval.h>
#include <pulse/util.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/card.h>
#include <pulsecore/namereg.h>

#include "module-card-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore profile of cards");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

#define SAVE_INTERVAL 10

static const char* const valid_modargs[] = {
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_subscription *subscription;
    pa_hook_slot *card_new_hook_slot;
    pa_time_event *save_time_event;
    GDBM_FILE gdbm_file;
};

#define ENTRY_VERSION 1

struct entry {
    uint8_t version;
    char profile[PA_NAME_MAX];
} PA_GCC_PACKED ;

static void save_time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(tv);
    pa_assert(u);

    pa_assert(e == u->save_time_event);
    u->core->mainloop->time_free(u->save_time_event);
    u->save_time_event = NULL;

    gdbm_sync(u->gdbm_file);
    pa_log_info("Synced.");
}

static struct entry* read_entry(struct userdata *u, const char *name) {
    datum key, data;
    struct entry *e;

    pa_assert(u);
    pa_assert(name);

    key.dptr = (char*) name;
    key.dsize = (int) strlen(name);

    data = gdbm_fetch(u->gdbm_file, key);

    if (!data.dptr)
        goto fail;

    if (data.dsize != sizeof(struct entry)) {
        pa_log_debug("Database contains entry for card %s of wrong size %lu != %lu. Probably due to upgrade, ignoring.", name, (unsigned long) data.dsize, (unsigned long) sizeof(struct entry));
        goto fail;
    }

    e = (struct entry*) data.dptr;

    if (e->version != ENTRY_VERSION) {
        pa_log_debug("Version of database entry for card %s doesn't match our version. Probably due to upgrade, ignoring.", name);
        goto fail;
    }

    if (!memchr(e->profile, 0, sizeof(e->profile))) {
        pa_log_warn("Database contains entry for card %s with missing NUL byte in profile name", name);
        goto fail;
    }

    return e;

fail:

    pa_xfree(data.dptr);
    return NULL;
}

static void trigger_save(struct userdata *u) {
    struct timeval tv;

    if (u->save_time_event)
        return;

    pa_gettimeofday(&tv);
    tv.tv_sec += SAVE_INTERVAL;
    u->save_time_event = u->core->mainloop->time_new(u->core->mainloop, &tv, save_time_callback, u);
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry entry, *old;
    datum key, data;
    pa_card *card;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    memset(&entry, 0, sizeof(entry));
    entry.version = ENTRY_VERSION;

    if (!(card = pa_idxset_get_by_index(c->cards, idx)))
        return;

    pa_strlcpy(entry.profile, card->active_profile ? card->active_profile->name : "", sizeof(entry.profile));

    if ((old = read_entry(u, card->name))) {

        if (strncmp(old->profile, entry.profile, sizeof(entry.profile)) == 0) {
            pa_xfree(old);
            return;
        }

        pa_xfree(old);
    }

    key.dptr = card->name;
    key.dsize = (int) strlen(card->name);

    data.dptr = (void*) &entry;
    data.dsize = sizeof(entry);

    pa_log_info("Storing profile for card %s.", card->name);

    gdbm_store(u->gdbm_file, key, data, GDBM_REPLACE);

    trigger_save(u);
}

static pa_hook_result_t card_new_hook_callback(pa_core *c, pa_card_new_data *new_data, struct userdata *u) {
    struct entry *e;

    pa_assert(new_data);

    if ((e = read_entry(u, new_data->name)) && e->profile[0]) {

        if (!new_data->active_profile) {
            pa_card_new_data_set_profile(new_data, e->profile);
            pa_log_info("Restoring profile for card %s.", new_data->name);
        } else
            pa_log_debug("Not restoring profile for card %s, because already set.", new_data->name);

        pa_xfree(e);
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    char *fname, *fn;
    pa_card *card;
    uint32_t idx;
    int gdbm_cache_size;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->save_time_event = NULL;
    u->gdbm_file = NULL;

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_CARD, subscribe_callback, u);

    u->card_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_CARD_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) card_new_hook_callback, u);

    /* We include the host identifier in the file name because gdbm
     * files are CPU dependant, and we don't want things to go wrong
     * if we are on a multiarch system. */

    fn = pa_sprintf_malloc("card-database."CANONICAL_HOST".gdbm");
    fname = pa_state_path(fn, TRUE);
    pa_xfree(fn);

    if (!fname)
        goto fail;

    if (!(u->gdbm_file = gdbm_open(fname, 0, GDBM_WRCREAT|GDBM_NOLOCK, 0600, NULL))) {
        pa_log("Failed to open volume database '%s': %s", fname, gdbm_strerror(gdbm_errno));
        pa_xfree(fname);
        goto fail;
    }

    /* By default the cache of gdbm is rather large, let's reduce it a bit to save memory */
    gdbm_cache_size = 10;
    gdbm_setopt(u->gdbm_file, GDBM_CACHESIZE, &gdbm_cache_size, sizeof(gdbm_cache_size));

    pa_log_info("Sucessfully opened database file '%s'.", fname);
    pa_xfree(fname);

    for (card = pa_idxset_first(m->core->cards, &idx); card; card = pa_idxset_next(m->core->cards, &idx))
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_NEW, card->index, u);

    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return  -1;
}

void pa__done(pa_module*m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
        pa_subscription_free(u->subscription);

    if (u->card_new_hook_slot)
        pa_hook_slot_free(u->card_new_hook_slot);

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    if (u->gdbm_file)
        gdbm_close(u->gdbm_file);

    pa_xfree(u);
}
