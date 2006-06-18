/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include <polypcore/module.h>
#include <polypcore/iochannel.h>
#include <polypcore/cli.h>
#include <polypcore/sioman.h>
#include <polypcore/log.h>
#include <polypcore/modargs.h>

#include "module-cli-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Command line interface")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("exit_on_eof=<exit daemon after EOF?>")

static const char* const valid_modargs[] = {
    "exit_on_eof",
    NULL
};

static void eof_and_unload_cb(pa_cli*c, void *userdata) {
    pa_module *m = userdata;
    
    assert(c);
    assert(m);

    pa_module_unload_request(m);
}

static void eof_and_exit_cb(pa_cli*c, void *userdata) {
    pa_module *m = userdata;

    assert(c);
    assert(m);

    m->core->mainloop->quit(m->core->mainloop, 0);
}

int pa__init(pa_core *c, pa_module*m) {
    pa_iochannel *io;
    pa_modargs *ma;
    int exit_on_eof = 0;
    
    assert(c);
    assert(m);

    if (c->running_as_daemon) {
        pa_log_info(__FILE__": Running as daemon, refusing to load this module.");
        return 0;
    }

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments.");
        goto fail;
    }
    
    if (pa_modargs_get_value_boolean(ma, "exit_on_eof", &exit_on_eof) < 0) {
        pa_log(__FILE__": exit_on_eof= expects boolean argument.");
        goto fail;
    }

    if (pa_stdio_acquire() < 0) {
        pa_log(__FILE__": STDIN/STDUSE already in use.");
        goto fail;
    }

    io = pa_iochannel_new(c->mainloop, STDIN_FILENO, STDOUT_FILENO);
    assert(io);
    pa_iochannel_set_noclose(io, 1);

    m->userdata = pa_cli_new(c, io, m);
    assert(m->userdata);

    pa_cli_set_eof_callback(m->userdata, exit_on_eof ? eof_and_exit_cb : eof_and_unload_cb, m);

    pa_modargs_free(ma);
    
    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    assert(c);
    assert(m);

    if (c->running_as_daemon == 0) {
        pa_cli_free(m->userdata);
        pa_stdio_release();
    }
}
