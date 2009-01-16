/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include "alsa-util.h"
#include "module-alsa-card-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("ALSA Card");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);

static void enumerate_cb(
        const pa_alsa_profile_info *sink,
        const pa_alsa_profile_info *source,
        void *userdata) {

    if (sink && source)
        pa_log("Found Output %s + Input %s", sink->description, source->description);
    else if (sink)
        pa_log("Found Output %s", sink->description);
    else if (source)
        pa_log("Found Input %s", source->description);

}

int pa__init(pa_module*m) {
    pa_alsa_redirect_errors_inc();
    pa_alsa_probe_profiles("1", &m->core->default_sample_spec, enumerate_cb, m);
    return 0;
}

void pa__done(pa_module*m) {
    pa_alsa_redirect_errors_dec();
}
