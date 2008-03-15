/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <pulsecore/core-util.h>
#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>

#include "module-default-device-restore-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Automatically restore the default sink and source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

#define DEFAULT_SINK_FILE "default-sink"
#define DEFAULT_SOURCE_FILE "default-source"

int pa__init(pa_module *m) {
    FILE *f;

    /* We never overwrite manually configured settings */

    if (m->core->default_sink_name)
        pa_log_info("Manually configured default sink, not overwriting.");
    else if ((f = pa_open_config_file(NULL, DEFAULT_SINK_FILE, NULL, NULL, "r"))) {
        char ln[256] = "";

        fgets(ln, sizeof(ln)-1, f);
        pa_strip_nl(ln);
        fclose(f);

        if (!ln[0])
            pa_log_debug("No previous default sink setting, ignoring.");
        else if (pa_namereg_get(m->core, ln, PA_NAMEREG_SINK, 1)) {
            pa_namereg_set_default(m->core, ln, PA_NAMEREG_SINK);
            pa_log_debug("Restored default sink '%s'.", ln);
        } else
            pa_log_info("Saved default sink '%s' not existant, not restoring default sink setting.", ln);
    }

    if (m->core->default_source_name)
        pa_log_info("Manually configured default source, not overwriting.");
    else if ((f = pa_open_config_file(NULL, DEFAULT_SOURCE_FILE, NULL, NULL, "r"))) {
        char ln[256] = "";

        fgets(ln, sizeof(ln)-1, f);
        pa_strip_nl(ln);
        fclose(f);

        if (!ln[0])
            pa_log_debug("No previous default source setting, ignoring.");
        else if (pa_namereg_get(m->core, ln, PA_NAMEREG_SOURCE, 1)) {
            pa_namereg_set_default(m->core, ln, PA_NAMEREG_SOURCE);
            pa_log_debug("Restored default source '%s'.", ln);
        } else
            pa_log_info("Saved default source '%s' not existant, not restoring default source setting.", ln);
    }

    return 0;
}

void pa__done(pa_module*m) {
    FILE *f;

    if ((f = pa_open_config_file(NULL, DEFAULT_SINK_FILE, NULL, NULL, "w"))) {
        const char *n = pa_namereg_get_default_sink_name(m->core);
        fprintf(f, "%s\n", n ? n : "");
        fclose(f);
    }

    if ((f = pa_open_config_file(NULL, DEFAULT_SOURCE_FILE, NULL, NULL, "w"))) {
        const char *n = pa_namereg_get_default_source_name(m->core);
        fprintf(f, "%s\n", n ? n : "");
        fclose(f);
    }
}
