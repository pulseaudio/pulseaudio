/***
  This file is part of PulseAudio.

  Copyright 2008-2009 Joao Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>

#include "module-bluetooth-discover-symdef.h"
#include "bluetooth-util.h"

PA_MODULE_AUTHOR("Joao Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Detect available bluetooth audio devices and load bluetooth audio drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE("async=<Asynchronous initialization?> "
                "sco_sink=<name of sink> "
                "sco_source=<name of source> ");
PA_MODULE_LOAD_ONCE(TRUE);

static const char* const valid_modargs[] = {
    "sco_sink",
    "sco_source",
    "async",
    NULL
};

struct userdata {
    pa_module *module;
    pa_modargs *modargs;
    pa_core *core;
    pa_bluetooth_discovery *discovery;
    pa_hook_slot *slot;
    pa_hashmap *hashmap;
};

struct module_info {
    char *path;
    uint32_t module;
};

static pa_hook_result_t load_module_for_device(pa_bluetooth_discovery *y, const pa_bluetooth_device *d, struct userdata *u) {
    struct module_info *mi;

    pa_assert(u);
    pa_assert(d);

    mi = pa_hashmap_get(u->hashmap, d->path);

    if (!d->dead && d->device_connected > 0 &&
        (d->audio_state >= PA_BT_AUDIO_STATE_CONNECTED ||
         d->audio_source_state >= PA_BT_AUDIO_STATE_CONNECTED ||
         d->hfgw_state >= PA_BT_AUDIO_STATE_CONNECTED)) {

        if (!mi) {
            pa_module *m = NULL;
            char *args;

            /* Oh, awesome, a new device has shown up and been connected! */

            args = pa_sprintf_malloc("address=\"%s\" path=\"%s\"", d->address, d->path);
#if 0
            /* This is in case we have to use hsp immediately, without waiting for .Audio.State = Connected */
            if (d->headset_state >= PA_BT_AUDIO_STATE_CONNECTED && somecondition) {
                char *tmp;
                tmp = pa_sprintf_malloc("%s profile=\"hsp\"", args);
                pa_xfree(args);
                args = tmp;
            }
#endif

            if (pa_modargs_get_value(u->modargs, "sco_sink", NULL) &&
                pa_modargs_get_value(u->modargs, "sco_source", NULL)) {
                char *tmp;

                tmp = pa_sprintf_malloc("%s sco_sink=\"%s\" sco_source=\"%s\"", args,
                                        pa_modargs_get_value(u->modargs, "sco_sink", NULL),
                                        pa_modargs_get_value(u->modargs, "sco_source", NULL));
                pa_xfree(args);
                args = tmp;
            }

            if (d->hfgw_state >= PA_BT_AUDIO_STATE_CONNECTED)
                args = pa_sprintf_malloc("%s profile=\"hfgw\"", args);
            else if (d->audio_source_state >= PA_BT_AUDIO_STATE_CONNECTED)
                args = pa_sprintf_malloc("%s profile=\"a2dp_source\" auto_connect=no", args);

            pa_log_debug("Loading module-bluetooth-device %s", args);
            m = pa_module_load(u->module->core, "module-bluetooth-device", args);
            pa_xfree(args);

            if (m) {
                mi = pa_xnew(struct module_info, 1);
                mi->module = m->index;
                mi->path = pa_xstrdup(d->path);

                pa_hashmap_put(u->hashmap, mi->path, mi);
            } else
                pa_log_debug("Failed to load module for device %s", d->path);
        }

    } else {

        if (mi) {

            /* Hmm, disconnection? Then let's unload our module */

            pa_log_debug("Unloading module for %s", d->path);
            pa_module_unload_request_by_index(u->core, mi->module, TRUE);

            pa_hashmap_remove(u->hashmap, mi->path);
            pa_xfree(mi->path);
            pa_xfree(mi);
        }
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module* m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    pa_bool_t async = FALSE;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "async", &async) < 0) {
        pa_log("Failed to parse async argument.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;
    u->modargs = ma;
    ma = NULL;
    u->hashmap = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    if (!(u->discovery = pa_bluetooth_discovery_get(u->core)))
        goto fail;

    u->slot = pa_hook_connect(pa_bluetooth_discovery_hook(u->discovery), PA_HOOK_NORMAL, (pa_hook_cb_t) load_module_for_device, u);

    if (!async)
        pa_bluetooth_discovery_sync(u->discovery);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module* m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->slot)
        pa_hook_slot_free(u->slot);

    if (u->discovery)
        pa_bluetooth_discovery_unref(u->discovery);

    if (u->hashmap) {
        struct module_info *mi;

        while ((mi = pa_hashmap_steal_first(u->hashmap))) {
            pa_xfree(mi->path);
            pa_xfree(mi);
        }

        pa_hashmap_free(u->hashmap, NULL, NULL);
    }

    if (u->modargs)
        pa_modargs_free(u->modargs);

    pa_xfree(u);
}
