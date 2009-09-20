/***
  This file is part of PulseAudio.

  Copyright 2006-2008 Lennart Poettering
  Copyright 2009 Colin Guthrie

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

#include <pulse/xmalloc.h>
#include <pulse/volume.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/rtclock.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/namereg.h>
#include <pulsecore/protocol-native.h>
#include <pulsecore/pstream.h>
#include <pulsecore/pstream-util.h>
#include <pulsecore/database.h>

#include "module-device-manager-symdef.h"

PA_MODULE_AUTHOR("Colin Guthrie");
PA_MODULE_DESCRIPTION("Keep track of devices (and their descriptions) both past and present");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE("This module does not take any arguments");

#define SAVE_INTERVAL (10 * PA_USEC_PER_SEC)

static const char* const valid_modargs[] = {
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_subscription *subscription;
    pa_hook_slot
        *sink_new_hook_slot,
        *source_new_hook_slot,
        *connection_unlink_hook_slot;
    pa_time_event *save_time_event;
    pa_database *database;

    pa_native_protocol *protocol;
    pa_idxset *subscribed;

    pa_bool_t role_device_priority_routing;
    pa_bool_t stream_restore_used;
    pa_bool_t checked_stream_restore;
};

#define ENTRY_VERSION 1

#define NUM_ROLES 9
enum {
    ROLE_NONE,
    ROLE_VIDEO,
    ROLE_MUSIC,
    ROLE_GAME,
    ROLE_EVENT,
    ROLE_PHONE,
    ROLE_ANIMATION,
    ROLE_PRODUCTION,
    ROLE_A11Y,
};

struct entry {
    uint8_t version;
    char description[PA_NAME_MAX];
    uint32_t priority[NUM_ROLES];
} PA_GCC_PACKED;

enum {
    SUBCOMMAND_TEST,
    SUBCOMMAND_READ,
    SUBCOMMAND_RENAME,
    SUBCOMMAND_DELETE,
    SUBCOMMAND_ROLE_DEVICE_PRIORITY_ROUTING,
    SUBCOMMAND_PREFER_DEVICE,
    SUBCOMMAND_DEFER_DEVICE,
    SUBCOMMAND_SUBSCRIBE,
    SUBCOMMAND_EVENT
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

static struct entry* read_entry(struct userdata *u, const char *name) {
    pa_datum key, data;
    struct entry *e;

    pa_assert(u);
    pa_assert(name);

    key.data = (char*) name;
    key.size = strlen(name);

    pa_zero(data);

    if (!pa_database_get(u->database, &key, &data))
        goto fail;

    if (data.size != sizeof(struct entry)) {
        pa_log_debug("Database contains entry for device %s of wrong size %lu != %lu. Probably due to upgrade, ignoring.", name, (unsigned long) data.size, (unsigned long) sizeof(struct entry));
        goto fail;
    }

    e = (struct entry*) data.data;

    if (e->version != ENTRY_VERSION) {
        pa_log_debug("Version of database entry for device %s doesn't match our version. Probably due to upgrade, ignoring.", name);
        goto fail;
    }

    if (!memchr(e->description, 0, sizeof(e->description))) {
        pa_log_warn("Database contains entry for device %s with missing NUL byte in description", name);
        goto fail;
    }

    return e;

fail:

    pa_datum_free(&data);
    return NULL;
}

static void trigger_save(struct userdata *u) {
    pa_native_connection *c;
    uint32_t idx;

    for (c = pa_idxset_first(u->subscribed, &idx); c; c = pa_idxset_next(u->subscribed, &idx)) {
        pa_tagstruct *t;

        t = pa_tagstruct_new(NULL, 0);
        pa_tagstruct_putu32(t, PA_COMMAND_EXTENSION);
        pa_tagstruct_putu32(t, 0);
        pa_tagstruct_putu32(t, u->module->index);
        pa_tagstruct_puts(t, u->module->name);
        pa_tagstruct_putu32(t, SUBCOMMAND_EVENT);

        pa_pstream_send_tagstruct(pa_native_connection_get_pstream(c), t);
    }

    if (u->save_time_event)
        return;

    u->save_time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + SAVE_INTERVAL, save_time_callback, u);
}

static pa_bool_t entries_equal(const struct entry *a, const struct entry *b) {
    if (strncmp(a->description, b->description, sizeof(a->description)))
        return FALSE;

    return TRUE;
}

static inline struct entry *load_or_initialize_entry(struct userdata *u, struct entry *entry, const char *name, const char *prefix) {
    struct entry *old;

    pa_assert(u);
    pa_assert(entry);
    pa_assert(name);
    pa_assert(prefix);

    if ((old = read_entry(u, name)))
        *entry = *old;
    else {
        /* This is a new device, so make sure we write it's priority list correctly */
        uint32_t max_priority[NUM_ROLES];
        pa_datum key;
        pa_bool_t done;

        pa_zero(max_priority);
        done = !pa_database_first(u->database, &key, NULL);

        /* Find all existing devices with the same prefix so we calculate the current max priority for each role */
        while (!done) {
            pa_datum next_key;

            done = !pa_database_next(u->database, &key, &next_key, NULL);

            if (key.size > strlen(prefix) && strncmp(key.data, prefix, strlen(prefix)) == 0) {
                char *name2;
                struct entry *e;

                name2 = pa_xstrndup(key.data, key.size);

                if ((e = read_entry(u, name2))) {
                    for (uint32_t i = 0; i < NUM_ROLES; ++i) {
                        max_priority[i] = PA_MAX(max_priority[i], e->priority[i]);
                    }

                    pa_xfree(e);
                }

                pa_xfree(name2);
            }
            pa_datum_free(&key);
            key = next_key;
        }

        /* Actually initialise our entry now we've calculated it */
        for (uint32_t i = 0; i < NUM_ROLES; ++i) {
            entry->priority[i] = max_priority[i] + 1;
        }
    }

    return old;
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry entry, *old = NULL;
    char *name = NULL;
    pa_datum key, data;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    pa_zero(entry);
    entry.version = ENTRY_VERSION;

    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK) {
        pa_sink *sink;

        if (!(sink = pa_idxset_get_by_index(c->sinks, idx)))
            return;

        name = pa_sprintf_malloc("sink:%s", sink->name);

        old = load_or_initialize_entry(u, &entry, name, "sink:");

        pa_strlcpy(entry.description, pa_strnull(pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_DESCRIPTION)), sizeof(entry.description));

    } else {
        pa_source *source;

        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE);

        if (!(source = pa_idxset_get_by_index(c->sources, idx)))
            return;

        if (source->monitor_of)
            return;

        name = pa_sprintf_malloc("source:%s", source->name);

        old = load_or_initialize_entry(u, &entry, name, "source:");

        pa_strlcpy(entry.description, pa_strnull(pa_proplist_gets(source->proplist, PA_PROP_DEVICE_DESCRIPTION)), sizeof(entry.description));
    }

    if (old) {

        if (entries_equal(old, &entry)) {
            pa_xfree(old);
            pa_xfree(name);
            return;
        }

        pa_xfree(old);
    }

    key.data = name;
    key.size = strlen(name);

    data.data = &entry;
    data.size = sizeof(entry);

    pa_log_info("Storing device %s.", name);

    pa_database_set(u->database, &key, &data, TRUE);

    pa_xfree(name);

    trigger_save(u);
}

static pa_hook_result_t sink_new_hook_callback(pa_core *c, pa_sink_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);

    name = pa_sprintf_malloc("sink:%s", new_data->name);

    if ((e = read_entry(u, name))) {
        if (strncmp(e->description, pa_proplist_gets(new_data->proplist, PA_PROP_DEVICE_DESCRIPTION), sizeof(e->description)) != 0) {
            pa_log_info("Restoring description for sink %s.", new_data->name);
            pa_proplist_sets(new_data->proplist, PA_PROP_DEVICE_DESCRIPTION, e->description);
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_new_hook_callback(pa_core *c, pa_source_new_data *new_data, struct userdata *u) {
    char *name;
    struct entry *e;

    pa_assert(c);
    pa_assert(new_data);
    pa_assert(u);

    name = pa_sprintf_malloc("source:%s", new_data->name);

    if ((e = read_entry(u, name))) {
        if (strncmp(e->description, pa_proplist_gets(new_data->proplist, PA_PROP_DEVICE_DESCRIPTION), sizeof(e->description)) != 0) {
            /* NB, We cannot detect if we are a monitor here... this could mess things up a bit... */
            pa_log_info("Restoring description for source %s.", new_data->name);
            pa_proplist_sets(new_data->proplist, PA_PROP_DEVICE_DESCRIPTION, e->description);
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static char *get_name(const char *key, const char *prefix) {
    char *t;

    if (strncmp(key, prefix, strlen(prefix)))
        return NULL;

    t = pa_xstrdup(key + strlen(prefix));
    return t;
}

static void apply_entry(struct userdata *u, const char *name, struct entry *e) {
    pa_sink *sink;
    pa_source *source;
    uint32_t idx;
    char *n;

    pa_assert(u);
    pa_assert(name);
    pa_assert(e);

    if ((n = get_name(name, "sink:"))) {
        for (sink = pa_idxset_first(u->core->sinks, &idx); sink; sink = pa_idxset_next(u->core->sinks, &idx)) {
            if (!pa_streq(sink->name, n)) {
                continue;
            }

            pa_log_info("Setting description for sink %s.", sink->name);
            pa_sink_set_description(sink, e->description);
        }
        pa_xfree(n);
    }
    else if ((n = get_name(name, "source:"))) {
        for (source = pa_idxset_first(u->core->sources, &idx); source; source = pa_idxset_next(u->core->sources, &idx)) {
            if (!pa_streq(source->name, n)) {
                continue;
            }

            if (source->monitor_of) {
                pa_log_warn("Cowardly refusing to set the description for monitor source %s.", source->name);
                continue;
            }

            pa_log_info("Setting description for source %s.", source->name);
            pa_source_set_description(source, e->description);
        }
        pa_xfree(n);
    }
}


static uint32_t get_role_index(const char* role) {
    pa_assert(role);

    if (strcmp(role, "") == 0)
        return ROLE_NONE;
    if (strcmp(role, "video") == 0)
        return ROLE_VIDEO;
    if (strcmp(role, "music") == 0)
        return ROLE_MUSIC;
    if (strcmp(role, "game") == 0)
        return ROLE_GAME;
    if (strcmp(role, "event") == 0)
        return ROLE_EVENT;
    if (strcmp(role, "phone") == 0)
        return ROLE_PHONE;
    if (strcmp(role, "animation") == 0)
        return ROLE_ANIMATION;
    if (strcmp(role, "production") == 0)
        return ROLE_PRODUCTION;
    if (strcmp(role, "a11y") == 0)
        return ROLE_A11Y;
    return PA_INVALID_INDEX;
}

#define EXT_VERSION 1

static int extension_cb(pa_native_protocol *p, pa_module *m, pa_native_connection *c, uint32_t tag, pa_tagstruct *t) {
  struct userdata *u;
  uint32_t command;
  pa_tagstruct *reply = NULL;

  pa_assert(p);
  pa_assert(m);
  pa_assert(c);
  pa_assert(t);

  u = m->userdata;

  if (pa_tagstruct_getu32(t, &command) < 0)
    goto fail;

  reply = pa_tagstruct_new(NULL, 0);
  pa_tagstruct_putu32(reply, PA_COMMAND_REPLY);
  pa_tagstruct_putu32(reply, tag);

  switch (command) {
    case SUBCOMMAND_TEST: {
      if (!pa_tagstruct_eof(t))
        goto fail;

      pa_tagstruct_putu32(reply, EXT_VERSION);
      break;
    }

    case SUBCOMMAND_READ: {
      pa_datum key;
      pa_bool_t done;

      if (!pa_tagstruct_eof(t))
        goto fail;

      done = !pa_database_first(u->database, &key, NULL);

      while (!done) {
        pa_datum next_key;
        struct entry *e;
        char *name;

        done = !pa_database_next(u->database, &key, &next_key, NULL);

        name = pa_xstrndup(key.data, key.size);
        pa_datum_free(&key);

        if ((e = read_entry(u, name))) {
          pa_tagstruct_puts(reply, name);
          pa_tagstruct_puts(reply, e->description);

          pa_xfree(e);
        }

        pa_xfree(name);

        key = next_key;
      }

      break;
    }

    case SUBCOMMAND_RENAME: {

        struct entry *e;
        const char *device, *description;

        if (pa_tagstruct_gets(t, &device) < 0 ||
          pa_tagstruct_gets(t, &description) < 0)
          goto fail;

        if (!device || !*device || !description || !*description)
          goto fail;

        if ((e = read_entry(u, device)) && ENTRY_VERSION == e->version) {
            pa_datum key, data;

            pa_strlcpy(e->description, description, sizeof(e->description));

            key.data = (char *) device;
            key.size = strlen(device);

            data.data = e;
            data.size = sizeof(*e);

            if (pa_database_set(u->database, &key, &data, FALSE) == 0) {
                apply_entry(u, device, e);

                trigger_save(u);
            }
            else
                pa_log_warn("Could not save device");

            pa_xfree(e);
        }
        else
            pa_log_warn("Could not rename device %s, no entry in database", device);

      break;
    }

    case SUBCOMMAND_DELETE:

      while (!pa_tagstruct_eof(t)) {
        const char *name;
        pa_datum key;

        if (pa_tagstruct_gets(t, &name) < 0)
          goto fail;

        key.data = (char*) name;
        key.size = strlen(name);

        /** @todo: Reindex the priorities */
        pa_database_unset(u->database, &key);
      }

      trigger_save(u);

      break;

    case SUBCOMMAND_ROLE_DEVICE_PRIORITY_ROUTING: {

        pa_bool_t enable;
        uint32_t sridx = PA_INVALID_INDEX;
        uint32_t idx;
        pa_module *module;

        if (pa_tagstruct_get_boolean(t, &enable) < 0)
            goto fail;

        /* If this is the first run, check for stream restore module */
        if (!u->checked_stream_restore) {
            u->checked_stream_restore = TRUE;

            for (module = pa_idxset_first(u->core->modules, &idx); module; module = pa_idxset_next(u->core->modules, &idx)) {
                if (strcmp(module->name, "module-stream-restore") == 0) {
                    pa_log_debug("Detected module-stream-restore is currently in use");
                    u->stream_restore_used = TRUE;
                    sridx = module->index;
                }
            }
        }

        u->role_device_priority_routing = enable;
        if (enable) {
            if (u->stream_restore_used) {
                if (PA_INVALID_INDEX == sridx) {
                    /* As a shortcut on first load, we have sridx filled in, but otherwise we search for it. */
                    for (module = pa_idxset_first(u->core->modules, &idx); module; module = pa_idxset_next(u->core->modules, &idx)) {
                        if (strcmp(module->name, "module-stream-restore") == 0) {
                            sridx = module->index;
                        }
                    }
                }
                if (PA_INVALID_INDEX != sridx) {
                    pa_log_debug("Unloading module-stream-restore to enable role-based device-priority routing");
                    pa_module_unload_request_by_index(u->core, sridx, TRUE);
                }
            }
        } else if (u->stream_restore_used) {
            /* We want to reload module-stream-restore */
            if (!pa_module_load(u->core, "module-stream-restore", ""))
                pa_log_warn("Failed to load module-stream-restore while disabling role-based device-priority routing");
        }

        break;
    }

    case SUBCOMMAND_PREFER_DEVICE:
    case SUBCOMMAND_DEFER_DEVICE: {

        const char *role, *device;
        struct entry *e;
        uint32_t role_index;

        if (pa_tagstruct_gets(t, &role) < 0 ||
            pa_tagstruct_gets(t, &device) < 0)
            goto fail;

        if (!role || !device || !*device)
            goto fail;

        role_index = get_role_index(role);
        if (PA_INVALID_INDEX == role_index)
            goto fail;

        if ((e = read_entry(u, device)) && ENTRY_VERSION == e->version) {
            pa_datum key, data;
            pa_bool_t done;
            char* prefix;
            uint32_t priority;
            pa_bool_t haschanged = FALSE;

            if (strncmp(device, "sink:", 5) == 0)
                prefix = pa_xstrdup("sink:");
            else
                prefix = pa_xstrdup("source:");

            priority = e->priority[role_index];

            /* Now we need to load up all the other entries of this type and shuffle the priroities around */

            done = !pa_database_first(u->database, &key, NULL);

            while (!done && !haschanged) {
                pa_datum next_key;

                done = !pa_database_next(u->database, &key, &next_key, NULL);

                /* Only read devices with the right prefix */
                if (key.size > strlen(prefix) && strncmp(key.data, prefix, strlen(prefix)) == 0) {
                    char *name;
                    struct entry *e2;

                    name = pa_xstrndup(key.data, key.size);

                    if ((e2 = read_entry(u, name))) {
                        if (SUBCOMMAND_PREFER_DEVICE == command) {
                            /* PREFER */
                            if (e2->priority[role_index] == (priority - 1)) {
                                e2->priority[role_index]++;
                                haschanged = TRUE;
                            }
                        } else {
                            /* DEFER */
                            if (e2->priority[role_index] == (priority + 1)) {
                                e2->priority[role_index]--;
                                haschanged = TRUE;
                            }
                        }

                        if (haschanged) {
                            data.data = e2;
                            data.size = sizeof(*e2);

                            if (pa_database_set(u->database, &key, &data, FALSE))
                                pa_log_warn("Could not save device");
                        }

                        pa_xfree(e2);
                    }

                    pa_xfree(name);
                }

                pa_datum_free(&key);
                key = next_key;
            }

            /* Now write out our actual entry */
            if (haschanged) {
                if (SUBCOMMAND_PREFER_DEVICE == command)
                    e->priority[role_index]--;
                else
                    e->priority[role_index]++;

                key.data = (char *) device;
                key.size = strlen(device);

                data.data = e;
                data.size = sizeof(*e);

                if (pa_database_set(u->database, &key, &data, FALSE))
                    pa_log_warn("Could not save device");

                trigger_save(u);
            }

            pa_xfree(e);

            pa_xfree(prefix);
        }
        else
            pa_log_warn("Could not reorder device %s, no entry in database", device);

        break;
    }

    case SUBCOMMAND_SUBSCRIBE: {

      pa_bool_t enabled;

      if (pa_tagstruct_get_boolean(t, &enabled) < 0 ||
        !pa_tagstruct_eof(t))
        goto fail;

      if (enabled)
        pa_idxset_put(u->subscribed, c, NULL);
      else
        pa_idxset_remove_by_data(u->subscribed, c, NULL);

      break;
    }

    default:
      goto fail;
  }

  pa_pstream_send_tagstruct(pa_native_connection_get_pstream(c), reply);
  return 0;

  fail:

  if (reply)
    pa_tagstruct_free(reply);

  return -1;
}

static pa_hook_result_t connection_unlink_hook_cb(pa_native_protocol *p, pa_native_connection *c, struct userdata *u) {
    pa_assert(p);
    pa_assert(c);
    pa_assert(u);

    pa_idxset_remove_by_data(u->subscribed, c, NULL);
    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    char *fname;
    pa_sink *sink;
    pa_source *source;
    uint32_t idx;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->subscribed = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->protocol = pa_native_protocol_get(m->core);
    pa_native_protocol_install_ext(u->protocol, m, extension_cb);

    u->connection_unlink_hook_slot = pa_hook_connect(&pa_native_protocol_hooks(u->protocol)[PA_NATIVE_HOOK_CONNECTION_UNLINK], PA_HOOK_NORMAL, (pa_hook_cb_t) connection_unlink_hook_cb, u);

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SOURCE, subscribe_callback, u);

    u->sink_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) sink_new_hook_callback, u);
    u->source_new_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) source_new_hook_callback, u);

    if (!(fname = pa_state_path("device-manager", TRUE)))
        goto fail;

    if (!(u->database = pa_database_open(fname, TRUE))) {
        pa_log("Failed to open volume database '%s': %s", fname, pa_cstrerror(errno));
        pa_xfree(fname);
        goto fail;
    }

    pa_log_info("Sucessfully opened database file '%s'.", fname);
    pa_xfree(fname);

    for (sink = pa_idxset_first(m->core->sinks, &idx); sink; sink = pa_idxset_next(m->core->sinks, &idx))
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_NEW, sink->index, u);

    for (source = pa_idxset_first(m->core->sources, &idx); source; source = pa_idxset_next(m->core->sources, &idx))
        subscribe_callback(m->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_NEW, source->index, u);

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

    if (u->sink_new_hook_slot)
        pa_hook_slot_free(u->sink_new_hook_slot);
    if (u->source_new_hook_slot)
        pa_hook_slot_free(u->source_new_hook_slot);

    if (u->save_time_event)
        u->core->mainloop->time_free(u->save_time_event);

    if (u->database)
        pa_database_close(u->database);

    if (u->protocol) {
        pa_native_protocol_remove_ext(u->protocol, m);
        pa_native_protocol_unref(u->protocol);
    }

    if (u->subscribed)
        pa_idxset_free(u->subscribed, NULL, NULL);

    pa_xfree(u);
}
