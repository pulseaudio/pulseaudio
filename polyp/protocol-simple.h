#ifndef fooprotocolsimplehfoo
#define fooprotocolsimplehfoo

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

#include "socket-server.h"
#include "module.h"
#include "core.h"
#include "modargs.h"

struct pa_protocol_simple;

struct pa_protocol_simple* pa_protocol_simple_new(struct pa_core *core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma);
void pa_protocol_simple_free(struct pa_protocol_simple *n);

#endif
