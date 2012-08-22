/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2009 Canonical Ltd
  Copyright (C) 2012 Intel Corporation

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

#include <pulsecore/core.h>
#include <pulsecore/source-output.h>
#include <pulsecore/source.h>
#include <pulsecore/core-util.h>

#include "module-bluetooth-policy-symdef.h"

PA_MODULE_AUTHOR("Frédéric Dalleau");
PA_MODULE_DESCRIPTION("When an A2DP source is added, load module-loopback");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

struct userdata {
     pa_hook_slot *source_put_slot;
};

/* When a source is created, loopback it to default sink */
static pa_hook_result_t source_put_hook_callback(pa_core *c, pa_source *source, void *userdata) {
    const char *s;
    const char *role;
    char *args;

    pa_assert(c);
    pa_assert(source);

    /* Only consider bluetooth sinks and sources */
    s = pa_proplist_gets(source->proplist, PA_PROP_DEVICE_BUS);
    if (!s)
        return PA_HOOK_OK;

    if (!pa_streq(s, "bluetooth"))
        return PA_HOOK_OK;

    /* Restrict to A2DP profile (sink role) */
    s = pa_proplist_gets(source->proplist, "bluetooth.protocol");
    if (!s)
        return PA_HOOK_OK;

    if (pa_streq(s, "a2dp_source"))
        role = "music";
    else {
        pa_log_debug("Profile %s cannot be selected for loopback", s);
        return PA_HOOK_OK;
    }

    /* Load module-loopback */
    args = pa_sprintf_malloc("source=\"%s\" source_dont_move=\"true\" sink_input_properties=\"media.role=%s\"", source->name, role);
    (void) pa_module_load(c, "module-loopback", args);
    pa_xfree(args);

    return PA_HOOK_OK;
}

int pa__init(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    m->userdata = u = pa_xnew(struct userdata, 1);

    u->source_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_NORMAL, (pa_hook_cb_t) source_put_hook_callback, u);

    return 0;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->source_put_slot)
        pa_hook_slot_free(u->source_put_slot);

    pa_xfree(u);
}
