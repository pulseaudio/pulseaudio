/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
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

#include "module.h"
#include "iochannel.h"
#include "cli.h"
#include "sioman.h"
#include "log.h"
#include "module-cli-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Command line interface")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("No arguments")

static void eof_cb(struct pa_cli*c, void *userdata) {
    struct pa_module *m = userdata;
    assert(c && m);

    pa_module_unload_request(m);
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct pa_iochannel *io;
    assert(c && m);

    if (m->argument) {
        pa_log(__FILE__": module doesn't accept arguments.\n");
        return -1;
    }
    
    if (pa_stdio_acquire() < 0) {
        pa_log(__FILE__": STDIN/STDUSE already in use.\n");
        return -1;
    }

    io = pa_iochannel_new(c->mainloop, STDIN_FILENO, STDOUT_FILENO);
    assert(io);
    pa_iochannel_set_noclose(io, 1);

    m->userdata = pa_cli_new(c, io, m);
    assert(m->userdata);

    pa_cli_set_eof_callback(m->userdata, eof_cb, m);
    
    return 0;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    assert(c && m);

    pa_cli_free(m->userdata);
    pa_stdio_release();
}
