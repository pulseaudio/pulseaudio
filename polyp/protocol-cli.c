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

#include <assert.h>
#include <stdlib.h>

#include "protocol-cli.h"
#include "cli.h"
#include "xmalloc.h"

struct pa_protocol_cli {
    struct pa_module *module;
    struct pa_core *core;
    struct pa_socket_server*server;
    struct pa_idxset *connections;
};

static void cli_eof_cb(struct pa_cli*c, void*userdata) {
    struct pa_protocol_cli *p = userdata;
    assert(p);
    pa_idxset_remove_by_data(p->connections, c, NULL);
    pa_cli_free(c);
}

static void on_connection(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata) {
    struct pa_protocol_cli *p = userdata;
    struct pa_cli *c;
    assert(s && io && p);

    c = pa_cli_new(p->core, io, p->module);
    assert(c);
    pa_cli_set_eof_callback(c, cli_eof_cb, p);

    pa_idxset_put(p->connections, c, NULL);
}

struct pa_protocol_cli* pa_protocol_cli_new(struct pa_core *core, struct pa_socket_server *server, struct pa_module *m, struct pa_modargs *ma) {
    struct pa_protocol_cli* p;
    assert(core && server);

    p = pa_xmalloc(sizeof(struct pa_protocol_cli));
    p->module = m;
    p->core = core;
    p->server = server;
    p->connections = pa_idxset_new(NULL, NULL);

    pa_socket_server_set_callback(p->server, on_connection, p);
    
    return p;
}

static void free_connection(void *p, void *userdata) {
    assert(p);
    pa_cli_free(p);
}

void pa_protocol_cli_free(struct pa_protocol_cli *p) {
    assert(p);

    pa_idxset_free(p->connections, free_connection, NULL);
    pa_socket_server_unref(p->server);
    pa_xfree(p);
}
