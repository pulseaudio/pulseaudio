/***
  This file is part of PulseAudio.

  Copyright 2008 Joao Paulo Rechi Vita

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
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/macro.h>
#include <pulsecore/llist.h>
#include <pulsecore/core-util.h>
#include <modules/dbus-util.h>

#include "module-bluetooth-discover-symdef.h"
#include "bluetooth-util.h"

PA_MODULE_AUTHOR("Joao Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Detect available bluetooth audio devices and load bluetooth audio drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE("sco_sink=<name of sink> "
                "sco_source=<name of source>"
                "async=<Asynchronous initialization?>");

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
    pa_dbus_connection *connection;
    pa_bluetooth_discovery *discovery;
};

static void load_module_for_device(struct userdata *u, pa_bluetooth_device *d, pa_bool_t good) {
    pa_assert(u);
    pa_assert(d);

    if (good &&
        d->device_connected > 0 &&
        (d->audio_sink_connected > 0 || d->headset_connected > 0)) {

        if (((uint32_t) PA_PTR_TO_UINT(d->data))-1 == PA_INVALID_INDEX) {
            pa_module *m = NULL;
            char *args;

            /* Oh, awesome, a new device has shown up and been connected! */

            args = pa_sprintf_malloc("address=\"%s\" path=\"%s\"", d->address, d->path);

            if (pa_modargs_get_value(u->modargs, "sco_sink", NULL) &&
                pa_modargs_get_value(u->modargs, "sco_source", NULL)) {
                char *tmp;

                tmp = pa_sprintf_malloc("%s sco_sink=\"%s\" sco_source=\"%s\"", args,
                                        pa_modargs_get_value(u->modargs, "sco_sink", NULL),
                                        pa_modargs_get_value(u->modargs, "sco_source", NULL));
                pa_xfree(args);
                args = tmp;
            }

            pa_log_debug("Loading module-bluetooth-device %s", args);
            m = pa_module_load(u->module->core, "module-bluetooth-device", args);
            pa_xfree(args);

            if (m)
                d->data = PA_UINT_TO_PTR((uint32_t) (m->index+1));
            else
                pa_log_debug("Failed to load module for device %s", d->path);
        }

    } else {

        if (((uint32_t) PA_PTR_TO_UINT(d->data))-1 != PA_INVALID_INDEX) {

            /* Hmm, disconnection? Then let's unload our module */

            pa_log_debug("Unloading module for %s", d->path);
            pa_module_unload_request_by_index(u->core, (uint32_t) (PA_PTR_TO_UINT(d->data))-1, TRUE);
            d->data = NULL;
        }
    }
}

static int setup_dbus(struct userdata *u) {
    DBusError err;

    dbus_error_init(&err);

    u->connection = pa_dbus_bus_get(u->core, DBUS_BUS_SYSTEM, &err);

    if (dbus_error_is_set(&err) || !u->connection) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        dbus_error_free(&err);
        return -1;
    }

    return 0;
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

    if (setup_dbus(u) < 0)
        goto fail;

    if (!(u->discovery = pa_bluetooth_discovery_new(pa_dbus_connection_get(u->connection), load_module_for_device, u)))
        goto fail;

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

    if (u->discovery)
        pa_bluetooth_discovery_free(u->discovery);

    if (u->connection)
        pa_dbus_connection_unref(u->connection);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    pa_xfree(u);
}
