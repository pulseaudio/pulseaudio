/* $Id$ */

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

#include <stdlib.h>

#include <pulse/xmalloc.h>

#include <pulsecore/cli.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "protocol-cli.h"

/* Don't allow more than this many concurrent connections */
#define MAX_CONNECTIONS 25

struct pa_protocol_cli {
    pa_module *module;
    pa_core *core;
    pa_socket_server*server;
    pa_idxset *connections;
};

static void cli_eof_cb(pa_cli*c, void*userdata) {
    pa_protocol_cli *p = userdata;
    pa_assert(p);

    pa_idxset_remove_by_data(p->connections, c, NULL);
    pa_cli_free(c);
}

static void on_connection(pa_socket_server*s, pa_iochannel *io, void *userdata) {
    pa_protocol_cli *p = userdata;
    pa_cli *c;

    pa_assert(s);
    pa_assert(io);
    pa_assert(p);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_cli_new(p->core, io, p->module);
    pa_cli_set_eof_callback(c, cli_eof_cb, p);

    pa_idxset_put(p->connections, c, NULL);
}

pa_protocol_cli* pa_protocol_cli_new(pa_core *core, pa_socket_server *server, pa_module *m, PA_GCC_UNUSED pa_modargs *ma) {
    pa_protocol_cli* p;

    pa_core_assert_ref(core);
    pa_assert(server);

    p = pa_xnew(pa_protocol_cli, 1);
    p->module = m;
    p->core = core;
    p->server = server;
    p->connections = pa_idxset_new(NULL, NULL);

    pa_socket_server_set_callback(p->server, on_connection, p);

    return p;
}

static void free_connection(void *p, PA_GCC_UNUSED void *userdata) {
    pa_assert(p);

    pa_cli_free(p);
}

void pa_protocol_cli_free(pa_protocol_cli *p) {
    pa_assert(p);

    pa_idxset_free(p->connections, free_connection, NULL);
    pa_socket_server_unref(p->server);
    pa_xfree(p);
}
