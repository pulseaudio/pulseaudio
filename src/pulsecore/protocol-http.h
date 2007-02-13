#ifndef fooprotocolhttphfoo
#define fooprotocolhttphfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2005-2006 Lennart Poettering

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

#include <pulsecore/core.h>
#include <pulsecore/socket-server.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>

typedef struct pa_protocol_http pa_protocol_http;

pa_protocol_http* pa_protocol_http_new(pa_core *core, pa_socket_server *server, pa_module *m, pa_modargs *ma);
void pa_protocol_http_free(pa_protocol_http *n);

#endif
