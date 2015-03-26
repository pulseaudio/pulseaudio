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
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/macro.h>
#include <pulsecore/core.h>
#include <stream-interaction.h>

#include "module-role-cork-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Mute & cork streams with certain roles while others exist");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "trigger_roles=<Comma separated list of roles which will trigger a cork> "
        "cork_roles=<Comma separated list of roles which will be corked> "
        "global=<Should we operate globally or only inside the same device?>");

static const char* const valid_modargs[] = {
    "trigger_roles",
    "cork_roles",
    "global",
    NULL
};

int pa__init(pa_module *m) {

    pa_assert(m);

    return pa_stream_interaction_init(m, valid_modargs);
}

void pa__done(pa_module *m) {

    pa_assert(m);

    pa_stream_interaction_done(m);
}
