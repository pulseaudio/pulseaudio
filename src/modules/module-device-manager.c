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
};

#define ENTRY_VERSION 1

struct entry {
    uint8_t version;
    char description[PA_NAME_MAX];
} PA_GCC_PACKED;

enum {
    SUBCOMMAND_TEST,
    SUBCOMMAND_READ,
    SUBCOMMAND_WRITE,
    SUBCOMMAND_DELETE,
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
    if (u->save_time_event)
        return;

    u->save_time_event = pa_core_rttime_new(u->core, pa_rtclock_now() + SAVE_INTERVAL, save_time_callback, u);
}

static pa_bool_t entries_equal(const struct entry *a, const struct entry *b) {
    if (strncmp(a->description, b->description, sizeof(a->description)))
        return FALSE;

    return TRUE;
}

static void subscribe_callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    struct entry entry, *old;
    char *name;
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

        if ((old = read_entry(u, name)))
            entry = *old;

        pa_strlcpy(entry.description, pa_strnull(pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_DESCRIPTION)), sizeof(entry.description));

    } else {
        pa_source *source;

        pa_assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE);

        if (!(source = pa_idxset_get_by_index(c->sources, idx)))
            return;

        name = pa_sprintf_malloc("source:%s", source->name);

        if ((old = read_entry(u, name)))
            entry = *old;

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

    pa_log_info("Storing device description for %s.", name);

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
            pa_log_info("Restoring description for sink %s.", name);
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
            pa_log_info("Restoring description for sink %s.", name);
            pa_proplist_sets(new_data->proplist, PA_PROP_DEVICE_DESCRIPTION, e->description);
        }

        pa_xfree(e);
    }

    pa_xfree(name);

    return PA_HOOK_OK;
}

static char *get_name(const char *key, const char *prefix) {
  char *t;

  if (strncmp(key, prefix, sizeof(prefix)))
    return NULL;

  t = pa_xstrdup(key + sizeof(prefix));
  return t;
}

static void apply_entry(struct userdata *u, const char *name, struct entry *e) {
  pa_sink *sink;
  pa_source *source;
  uint32_t idx;

  pa_assert(u);
  pa_assert(name);
  pa_assert(e);

  for (sink = pa_idxset_first(u->core->sinks, &idx); sink; sink = pa_idxset_next(u->core->sinks, &idx)) {
    char *n;

    if (!(n = get_name(name, "sink")))
      continue;

    if (!pa_streq(sink->name, n)) {
      pa_xfree(n);
      continue;
    }
    pa_xfree(n);

    pa_log_info("Restoring description for sink %s.", sink->name);
    pa_proplist_sets(sink->proplist, PA_PROP_DEVICE_DESCRIPTION, e->description);
  }

  for (source = pa_idxset_first(u->core->sources, &idx); source; source = pa_idxset_next(u->core->sources, &idx)) {
    char *n;

    if (!(n = get_name(name, "source")))
      continue;

    if (!pa_streq(source->name, n)) {
      pa_xfree(n);
      continue;
    }
    pa_xfree(n);

    pa_log_info("Restoring description for source %s.", source->name);
    pa_proplist_sets(source->proplist, PA_PROP_DEVICE_DESCRIPTION, e->description);
  }
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

    case SUBCOMMAND_WRITE: {
      uint32_t mode;
      pa_bool_t apply_immediately = FALSE;

      if (pa_tagstruct_getu32(t, &mode) < 0 ||
        pa_tagstruct_get_boolean(t, &apply_immediately) < 0)
        goto fail;

      if (mode != PA_UPDATE_MERGE &&
        mode != PA_UPDATE_REPLACE &&
        mode != PA_UPDATE_SET)
        goto fail;

      if (mode == PA_UPDATE_SET)
        pa_database_clear(u->database);

      while (!pa_tagstruct_eof(t)) {
        const char *name, *description;
        struct entry entry;
        pa_datum key, data;

        pa_zero(entry);
        entry.version = ENTRY_VERSION;

        if (pa_tagstruct_gets(t, &name) < 0 ||
          pa_tagstruct_gets(t, &description) < 0)
          goto fail;

        if (!name || !*name)
          goto fail;

        pa_strlcpy(entry.description, description, sizeof(entry.description));

        key.data = (char*) name;
        key.size = strlen(name);

        data.data = &entry;
        data.size = sizeof(entry);

        if (pa_database_set(u->database, &key, &data, mode == PA_UPDATE_REPLACE) == 0)
          if (apply_immediately)
            apply_entry(u, name, &entry);
      }

      trigger_save(u);

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

        pa_database_unset(u->database, &key);
      }

      trigger_save(u);

      break;

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
