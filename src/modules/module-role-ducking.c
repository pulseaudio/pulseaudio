/***
  This file is part of PulseAudio.

  Copyright 2012 Flavio Ceolin <flavio.ceolin@profusion.mobi>

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

#include "module-role-ducking-symdef.h"

PA_MODULE_AUTHOR("Flavio Ceolin <flavio.ceolin@profusion.mobi>");
PA_MODULE_DESCRIPTION("Apply a ducking effect based on streams roles");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "trigger_roles=<Comma(and slash) separated list of roles which will trigger a ducking. Slash can divide the roles into groups>"
        "ducking_roles=<Comma(and slash) separated list of roles which will be ducked. Slash can divide the roles into groups>"
        "global=<Should we operate globally or only inside the same device?>"
        "volume=<Volume for the attenuated streams. Default: -20dB. If trigger_roles and ducking_roles are separated by slash, use slash for dividing volume group>"
);

static const char* const valid_modargs[] = {
    "trigger_roles",
    "ducking_roles",
    "global",
    "volume",
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
