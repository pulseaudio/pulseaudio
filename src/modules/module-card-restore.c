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

#include <pulse/gccmacro.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/card.h>
#include <pulsecore/namereg.h>
#include <pulsecore/database.h>
#include <pulsecore/tagstruct.h>

#include "module-card-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore profile of cards");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

#define SAVE_INTERVAL (10 * PA_USEC_PER_SEC)

static const char* const valid_modargs[] = {
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_subscription *subscription;
    pa_hook_slot *card_new_hook_slot;
    pa_time_event *save_time_event;
    pa_database *database;
};

#define ENTRY_VERSION 2

struct port_info {
    char *name;
    int64_t offset;
};

struct entry {
    uint8_t version;
    char *profile;
    pa_hashmap *ports; /* Port name -> struct port_info */
};

static void save_time_callback(pa_mainloop_api*a, pa_time_event* e, const struct timeval *t, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(a);
    pa_assert(e);
    pa_assert(u);

    pa_assert(e == u->save_time_event);
    u->core->mainloop->time_free(u->save_time_event);
    u->save_time_event = NULL;

    pa_database_sync(u->database);
    pa_log_info("Synced.");
}

static void trigger_save(struct userdata *u) {
    if (u->save_time_event)
        return;

    u->save_time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + SAVE_INTERVAL, save_time_callback, u);
}

static struct entry* entry_new(void) {
    struct entry *r = pa_xnew0(struct entry, 1);
    r->version = ENTRY_VERSION;
    r->ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    return r;
}

static void port_info_free(struct port_info *p_info, void *userdata) {
    pa_assert(p_info);

    pa_xfree(p_info->name);
    pa_xfree(p_info);
}

static void entry_free(struct entry* e) {
    pa_assert(e);

    pa_xfree(e->profile);
    pa_hashmap_free(e->ports, (pa_free2_cb_t) port_info_free, NULL);

    pa_xfree(e);
}

static pa_bool_t entry_write(struct userdata *u, const char *name, const struct entry *e) {
    pa_tagstruct *t;
    pa_datum key, data;
    pa_bool_t r;
    void *state;
    struct port_info *p_info;

    pa_assert(u);
    pa_assert(name);
    pa_assert(e);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu8(t, e->version);
    pa_tagstruct_puts(t, e->profile);
    pa_tagstruct_putu32(t, pa_hashmap_size(e->ports));

    PA_HASHMAP_FOREACH(p_info, e->ports, state) {
        pa_tagstruct_puts(t, p_info->name);
        pa_tagstruct_puts64(t, p_info->offset);
    }

    key.data = (char *) name;
    key.size = strlen(name);

    data.data = (void*)pa_tagstruct_data(t, &data.size);

    r = (pa_database_set(u->database, &key, &data, TRUE) == 0);

    pa_tagstruct_free(t);

    return r;
}

#ifdef ENABLE_LEGACY_DATABASE_ENTRY_FORMAT

#define LEGACY_ENTRY_VERSION 1
static struct entry* legacy_entry_read(struct userdata *u, pa_datum *data) {
    struct legacy_entry {
        uint8_t version;
        char profile[PA_NAME_MAX];
    } PA_GCC_PACKED ;
    struct legacy_entry *le;
    struct entry *e;

    pa_assert(u);
    pa_assert(data);

    if (data->size != sizeof(struct legacy_entry)) {
        pa_log_debug("Size does not match.");
        return NULL;
    }

    le = (struct legacy_entry*)data->data;

    if (le->version != LEGACY_ENTRY_VERSION) {
        pa_log_debug("Version mismatch.");
        return NULL;
    }

    if (!memchr(le->profile, 0, sizeof(le->profile))) {
        pa_log_warn("Profile has missing NUL byte.");
        return NULL;
    }

    e = entry_new();
    e->profile = pa_xstrdup(le->profile);
    return e;
}
#endif

static struct entry* entry_read(struct userdata *u, const char *name) {
    pa_datum key, data;
    struct entry *e = NULL;
    pa_tagstruct *t = NULL;
    const char* profile;

    pa_assert(u);
    pa_assert(name);

    key.data = (char*) name;
    key.size = strlen(name);

    pa_zero(data);

    if (!pa_database_get(u->database, &key, &data))
        goto fail;

    t = pa_tagstruct_new(data.data, data.size);
    e = entry_new();

    if (pa_tagstruct_getu8(t, &e->version) < 0 ||
        e->version > ENTRY_VERSION ||
        pa_tagstruct_gets(t, &profile) < 0) {

        goto fail;
    }

    e->profile = pa_xstrdup(profile);

    if (e->version >= 2) {
        uint32_t port_count = 0;
        const char *port_name = NULL;
        int64_t port_offset = 0;
        struct port_info *p_info;
        unsigned i;

        if (pa_tagstruct_getu32(t, &port_count) < 0)
            goto fail;

        for (i = 0; i < port_count; i++) {
            if (pa_tagstruct_gets(t, &port_name) < 0 ||
                !port_name ||
                pa_hashmap_get(e->ports, port_name) ||
                pa_tagstruct_gets64(t, &port_offset) < 0)
                goto fail;

            p_info = pa_xnew(struct port_info, 1);
            p_info->name = pa_xstrdup(port_name);
            p_info->offset = port_offset;

            pa_assert_se(pa_hashmap_put(e->ports, p_info->name, p_info) >= 0);
        }
    }

    if (!pa_tagstruct_eof(t))
        goto fail;

    pa_tagstruct_free(t);
    pa_datum_free(&data);

    return e;

fail:

    pa_log_debug("Database contains invalid data for key: %s (probably pre-v1.0 data)", name);

    if (e)
        entry_free(e);
    if (t)
        pa_tagstruct_free(t);

#ifdef ENABLE_LEGACY_DATABASE_ENTRY_FORMAT
    pa_log_debug("Attempting to load legacy (pre-v1.0) data for key: %s", name);
    if ((e = legacy_entry_read(u, &data))) {
        pa_log_debug("Success. Saving new format for key: %s", name);
        if (entry_write(u, name, e))
            trigger_save(u);
        pa_datum_free(&data);
        return e;
    } else
        pa_log_debug("Unable to load legacy (pre-v1.0) data for key: %s. Ignoring.", name);
#endif

    pa_datum_free(&data);
    return NULL;
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry *entry, *old;
    void *state;
    pa_card *card;
    pa_device_port *p;
    struct port_info *p_info;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    if (!(card = pa_idxset_get_by_index(c->cards, idx)))
        return;

    entry = entry_new();

    if (card->save_profile)
        entry->profile = pa_xstrdup(card->active_profile->name);

    PA_HASHMAP_FOREACH(p, card->ports, state) {
        p_info = pa_xnew(struct port_info, 1);
        p_info->name = pa_xstrdup(p->name);
        p_info->offset = p->latency_offset;

        pa_assert_se(pa_hashmap_put(entry->ports, p_info->name, p_info) >= 0);
    }

    if ((old = entry_read(u, card->name))) {
        bool dirty = false;

        if (!card->save_profile)
            entry->profile = pa_xstrdup(old->profile);

        if (!pa_streq(old->profile, entry->profile) ||
                pa_hashmap_size(old->ports) != pa_hashmap_size(entry->ports))
            dirty = true;

        else {
            struct port_info *old_p_info;

            PA_HASHMAP_FOREACH(old_p_info, old->ports, state) {
                if ((p_info = pa_hashmap_get(entry->ports, old_p_info->name))) {
                    if (p_info->offset != old_p_info->offset) {
                        dirty = true;
                        break;
                    }

                } else {
                    dirty = true;
                    break;
                }
            }
        }

        if (!dirty) {
            entry_free(old);
            entry_free(entry);
            return;
        }

        entry_free(old);
    }

    if (card->save_profile)
        pa_log_info("Storing profile and port latency offsets for card %s.", card->name);
    else
        pa_log_info("Storing port latency offsets for card %s.", card->name);

    if (entry_write(u, card->name, entry))
        trigger_save(u);

    entry_free(entry);
}

static pa_hook_result_t card_new_hook_callback(pa_core *c, pa_card_new_data *new_data, struct userdata *u) {
    struct entry *e;
    void *state;
    pa_device_port *p;
    struct port_info *p_info;

    pa_assert(new_data);

    if (!(e = entry_read(u, new_data->name)))
        return PA_HOOK_OK;

    if (e->profile[0]) {
        if (!new_data->active_profile) {
            pa_log_info("Restoring profile for card %s.", new_data->name);
            pa_card_new_data_set_profile(new_data, e->profile);
            new_data->save_profile = TRUE;

        } else
            pa_log_debug("Not restoring profile for card %s, because already set.", new_data->name);
    }

    /* Always restore the latency offsets because their
     * initial value is always 0 */

    pa_log_info("Restoring port latency offsets for card %s.", new_data->name);

    PA_HASHMAP_FOREACH(p_info, e->ports, state)
        if ((p = pa_hashmap_get(new_data->ports, p_info->name)))
            p->latency_offset = p_info->offset;

    entry_free(e);

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    char *fname;
    pa_card *card;
    uint32_t idx;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_CARD, subscribe_callback, u);

    u->card_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_CARD_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) card_new_hook_callback, u);

    if (!(fname = pa_state_path("card-database", TRUE)))
        goto fail;

    if (!(u->database = pa_database_open(fname, TRUE))) {
        pa_log("Failed to open volume database '%s': %s", fname, pa_cstrerror(errno));
        pa_xfree(fname);
        goto fail;
    }

    pa_log_info("Successfully opened database file '%s'.", fname);
    pa_xfree(fname);

    PA_IDXSET_FOREACH(card, m->core->cards, idx)
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_NEW, card->index, u);

    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
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

    if (u->database)
        pa_database_close(u->database);

    pa_xfree(u);
}
