/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <stdio.h>
#include <unistd.h>

#include <pulsecore/module.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/cli.h>
#include <pulsecore/sioman.h>
#include <pulsecore/log.h>
#include <pulsecore/modargs.h>
#include <pulsecore/macro.h>

#include "module-cli-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Command line interface");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE("exit_on_eof=<exit daemon after EOF?>");

static const char* const valid_modargs[] = {
    "exit_on_eof",
    NULL
};

static void eof_and_unload_cb(pa_cli*c, void *userdata) {
    pa_module *m = userdata;

    pa_assert(c);
    pa_assert(m);

    pa_module_unload_request(m, TRUE);
}

static void eof_and_exit_cb(pa_cli*c, void *userdata) {
    pa_module *m = userdata;

    pa_assert(c);
    pa_assert(m);

    pa_core_exit(m->core, FALSE, 0);
}

int pa__init(pa_module*m) {
    pa_iochannel *io;
    pa_modargs *ma;
    pa_bool_t exit_on_eof = FALSE;

    pa_assert(m);

    if (m->core->running_as_daemon) {
        pa_log_info("Running as daemon, refusing to load this module.");
        return 0;
    }

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "exit_on_eof", &exit_on_eof) < 0) {
        pa_log("exit_on_eof= expects boolean argument.");
        goto fail;
    }

    if (pa_stdio_acquire() < 0) {
        pa_log("STDIN/STDUSE already in use.");
        goto fail;
    }

    io = pa_iochannel_new(m->core->mainloop, STDIN_FILENO, STDOUT_FILENO);
    pa_iochannel_set_noclose(io, 1);

    m->userdata = pa_cli_new(m->core, io, m);

    pa_cli_set_eof_callback(m->userdata, exit_on_eof ? eof_and_exit_cb : eof_and_unload_cb, m);

    pa_modargs_free(ma);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module*m) {
    pa_assert(m);

    if (m->core->running_as_daemon == 0) {
        pa_cli_free(m->userdata);
        pa_stdio_release();
    }
}
