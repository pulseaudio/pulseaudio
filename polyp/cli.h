#ifndef fooclihfoo
#define fooclihfoo

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

#include "iochannel.h"
#include "core.h"
#include "module.h"

struct pa_cli;

struct pa_cli* pa_cli_new(struct pa_core *core, struct pa_iochannel *io, struct pa_module *m);
void pa_cli_free(struct pa_cli *cli);

void pa_cli_set_eof_callback(struct pa_cli *cli, void (*cb)(struct pa_cli*c, void *userdata), void *userdata);

#endif
