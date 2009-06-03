/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen

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

#include <pulse/xmalloc.h>

#include <pulsecore/macro.h>
#include <pulsecore/module.h>

#include "module-dbus-protocol-symdef.h"

PA_MODULE_DESCRIPTION("D-Bus interface");
PA_MODULE_USAGE("<no module arguments>");
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_AUTHOR("Tanu Kaskinen");
PA_MODULE_VERSION(PACKAGE_VERSION);

struct userdata {
    pa_module *module;
};

int pa__init(pa_module *m) {
    struct userdata *u = NULL;

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;

    pa_log_notice("Hello, world!");

    return 0;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    pa_xfree(u);
    m->userdata = NULL;
}
