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

#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include "module.h"
#include "util.h"
#include "modargs.h"
#include "log.h"
#include "module-esound-compat-spawnfd-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("ESOUND compatibility module: -spawnfd emulation")
PA_MODULE_USAGE("fd=<file descriptor>")
PA_MODULE_VERSION(PACKAGE_VERSION)

static const char* const valid_modargs[] = {
    "fd",
    NULL,
};

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct pa_modargs *ma = NULL;
    int ret = -1, fd = -1;
    char x = 1;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs)) ||
        pa_modargs_get_value_s32(ma, "fd", &fd) < 0 ||
        fd < 0) {
        pa_log(__FILE__": Failed to parse module arguments\n");
        goto finish;
    }

    if (pa_loop_write(fd, &x, sizeof(x)) != sizeof(x))
        pa_log(__FILE__": WARNING: write(%u, 1, 1) failed: %s\n", fd, strerror(errno));

    close(fd);

    pa_module_unload_request(m);

    ret = 0;

finish:
    if (ma)
        pa_modargs_free(ma);

    return ret;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    assert(c && m);
}


