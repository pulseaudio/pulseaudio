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

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "module.h"
#include "socket-server.h"
#include "socket-util.h"
#include "util.h"
#include "modargs.h"

#if defined(USE_PROTOCOL_SIMPLE)
  #include "protocol-simple.h"
  #define protocol_new pa_protocol_simple_new
  #define protocol_free pa_protocol_simple_free
  #define IPV4_PORT 4711
  #define UNIX_SOCKET "/tmp/polypaudio/simple"
  #define MODULE_ARGUMENTS "rate", "format", "channels", "sink", "source", "playback", "record",
#elif defined(USE_PROTOCOL_CLI)
  #include "protocol-cli.h" 
  #define protocol_new pa_protocol_cli_new
  #define protocol_free pa_protocol_cli_free
  #define IPV4_PORT 4712
  #define UNIX_SOCKET "/tmp/polypaudio/cli"
  #define MODULE_ARGUMENTS 
#elif defined(USE_PROTOCOL_NATIVE)
  #include "protocol-native.h"
  #define protocol_new pa_protocol_native_new
  #define protocol_free pa_protocol_native_free
  #define IPV4_PORT 4713
  #define UNIX_SOCKET "/tmp/polypaudio/native"
  #define MODULE_ARGUMENTS "public", "cookie",
#elif defined(USE_PROTOCOL_ESOUND)
  #include "protocol-esound.h"
  #include "esound.h"
  #define protocol_new pa_protocol_esound_new
  #define protocol_free pa_protocol_esound_free
  #define IPV4_PORT ESD_DEFAULT_PORT
  #define UNIX_SOCKET ESD_UNIX_SOCKET_NAME
  #define MODULE_ARGUMENTS "sink", "source", "public", "cookie",
#else
  #error "Broken build system" 
#endif

static const char* const valid_modargs[] = {
    MODULE_ARGUMENTS
#ifdef USE_TCP_SOCKETS
    "port",
    "loopback",
#else
    "socket",
#endif
    NULL
};

static struct pa_socket_server *create_socket_server(struct pa_core *c, struct pa_modargs *ma) {
    struct pa_socket_server *s;
#ifdef USE_TCP_SOCKETS
    uint32_t loopback = 1, port = IPV4_PORT;

    if (pa_modargs_get_value_u32(ma, "loopback", &loopback) < 0) {
        fprintf(stderr, "loopback= expects a numerical argument.\n");
        return NULL;
    }

    if (pa_modargs_get_value_u32(ma, "port", &port) < 0) {
        fprintf(stderr, "port= expects a numerical argument.\n");
        return NULL;
    }
    
    if (!(s = pa_socket_server_new_ipv4(c->mainloop, loopback ? INADDR_LOOPBACK : INADDR_ANY, port)))
        return NULL;
#else
    int r;
    const char *p;

    p = pa_modargs_get_value(ma, "socket", UNIX_SOCKET);
    assert(p);

    if (pa_unix_socket_make_secure_dir(p) < 0) {
        fprintf(stderr, "Failed to create secure socket directory.\n");
        return NULL;
    }

    if ((r = pa_unix_socket_remove_stale(p)) < 0) {
        fprintf(stderr, "Failed to remove stale UNIX socket '%s': %s\n", p, strerror(errno));
        return NULL;
    }
    
    if (r)
        fprintf(stderr, "Removed stale UNIX socket '%s'.", p);
    
    if (!(s = pa_socket_server_new_unix(c->mainloop, p)))
        return NULL;
    
#endif
    return s;
}

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct pa_socket_server *s;
    struct pa_modargs *ma = NULL;
    int ret = -1;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        fprintf(stderr, "Failed to parse module arguments\n");
        goto finish;
    }

    if (!(s = create_socket_server(c, ma)))
        goto finish;

    if (!(m->userdata = protocol_new(c, s, m, ma))) {
        pa_socket_server_unref(s);
        goto finish;
    }

    ret = 0;

finish:
    if (ma)
        pa_modargs_free(ma);

    return ret;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    assert(c && m);

    protocol_free(m->userdata);
}
