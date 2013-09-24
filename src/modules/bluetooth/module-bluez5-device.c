/***
  This file is part of PulseAudio.

  Copyright 2008-2013 João Paulo Rechi Vita
  Copyright 2011-2013 BMW Car IT GmbH.

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

#include <pulsecore/module.h>
#include <pulsecore/modargs.h>

#include "bluez5-util.h"

#include "module-bluez5-device-symdef.h"

PA_MODULE_AUTHOR("João Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("BlueZ 5 Bluetooth audio sink and source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE("path=<device object path>");

static const char* const valid_modargs[] = {
    "path",
    NULL
};

struct userdata {
    pa_module *module;
    pa_core *core;

    pa_bluetooth_discovery *discovery;
    pa_bluetooth_device *device;
};

int pa__init(pa_module* m) {
    struct userdata *u;
    const char *path;
    pa_modargs *ma;

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->core = m->core;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        goto fail;
    }

    if (!(path = pa_modargs_get_value(ma, "path", NULL))) {
        pa_log_error("Failed to get device path from module arguments");
        goto fail;
    }

    if (!(u->discovery = pa_bluetooth_discovery_get(m->core)))
        goto fail;

    if (!(u->device = pa_bluetooth_discovery_get_device_by_path(u->discovery, path))) {
        pa_log_error("%s is unknown", path);
        goto fail;
    }

    pa_modargs_free(ma);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->discovery)
        pa_bluetooth_discovery_unref(u->discovery);

    pa_xfree(u);
}
