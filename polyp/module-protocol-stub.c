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

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>

#include "module.h"
#include "socket-server.h"
#include "socket-util.h"
#include "util.h"
#include "modargs.h"
#include "log.h"
#include "native-common.h"
#include "util.h"

#ifdef USE_TCP_SOCKETS
#define SOCKET_DESCRIPTION "(TCP sockets)"
#define SOCKET_USAGE "port=<TCP port number> loopback=<listen on loopback device only?>"
#elif defined(USE_TCP6_SOCKETS)
#define SOCKET_DESCRIPTION "(TCP/IPv6 sockets)"
#define SOCKET_USAGE "port=<TCP port number> loopback=<listen on loopback device only?>"
#else
#define SOCKET_DESCRIPTION "(UNIX sockets)"
#define SOCKET_USAGE "socket=<path to UNIX socket>"
#endif

#if defined(USE_PROTOCOL_SIMPLE)
  #include "protocol-simple.h"
  #define protocol_new pa_protocol_simple_new
  #define protocol_free pa_protocol_simple_free
  #define TCPWRAP_SERVICE "polypaudio-simple"
  #define IPV4_PORT 4711
  #define UNIX_SOCKET "simple"
  #define MODULE_ARGUMENTS "rate", "format", "channels", "sink", "source", "playback", "record",
  #if defined(USE_TCP_SOCKETS)
    #include "module-simple-protocol-tcp-symdef.h"
  #elif defined(USE_TCP6_SOCKETS)
    #include "module-simple-protocol-tcp6-symdef.h"
  #else
    #include "module-simple-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("Simple protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("rate=<sample rate> format=<sample format> channels=<number of channels> sink=<sink to connect to> source=<source to connect to> playback=<enable playback?> record=<enable record?> "SOCKET_USAGE)
#elif defined(USE_PROTOCOL_CLI)
  #include "protocol-cli.h" 
  #define protocol_new pa_protocol_cli_new
  #define protocol_free pa_protocol_cli_free
  #define TCPWRAP_SERVICE "polypaudio-cli"
  #define IPV4_PORT 4712
  #define UNIX_SOCKET "cli"
  #define MODULE_ARGUMENTS 
  #ifdef USE_TCP_SOCKETS
    #include "module-cli-protocol-tcp-symdef.h"
  #elif defined(USE_TCP6_SOCKETS)
    #include "module-cli-protocol-tcp6-symdef.h"
  #else
    #include "module-cli-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("Command line interface protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE(SOCKET_USAGE)
#elif defined(USE_PROTOCOL_NATIVE)
  #include "protocol-native.h"
  #define protocol_new pa_protocol_native_new
  #define protocol_free pa_protocol_native_free
  #define TCPWRAP_SERVICE "polypaudio-native"
  #define IPV4_PORT PA_NATIVE_DEFAULT_PORT
  #define UNIX_SOCKET PA_NATIVE_DEFAULT_UNIX_SOCKET
  #define MODULE_ARGUMENTS "public", "cookie",
  #ifdef USE_TCP_SOCKETS
    #include "module-native-protocol-tcp-symdef.h"
  #elif defined(USE_TCP6_SOCKETS)
    #include "module-native-protocol-tcp6-symdef.h"
  #else
    #include "module-native-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("Native protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("public=<don't check for cookies?> cookie=<path to cookie file> "SOCKET_USAGE)
#elif defined(USE_PROTOCOL_ESOUND)
  #include "protocol-esound.h"
  #include "esound.h"
  #define protocol_new pa_protocol_esound_new
  #define protocol_free pa_protocol_esound_free
  #define TCPWRAP_SERVICE "esound"
  #define IPV4_PORT ESD_DEFAULT_PORT
  #define UNIX_SOCKET ESD_UNIX_SOCKET_NAME
  #define MODULE_ARGUMENTS "sink", "source", "public", "cookie",
  #ifdef USE_TCP_SOCKETS
    #include "module-esound-protocol-tcp-symdef.h"
  #elif defined(USE_TCP6_SOCKETS)
    #include "module-esound-protocol-tcp6-symdef.h"
  #else
    #include "module-esound-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("EsounD protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("sink=<sink to connect to> source=<source to connect to> public=<don't check for cookies?> cookie=<path to cookie file> "SOCKET_USAGE)
#else
  #error "Broken build system" 
#endif

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_VERSION(PACKAGE_VERSION)

static const char* const valid_modargs[] = {
    MODULE_ARGUMENTS
#if defined(USE_TCP_SOCKETS) || defined(USE_TCP6_SOCKETS)
    "port",
    "loopback",
#else
    "socket",
#endif
    NULL
};

static struct pa_socket_server *create_socket_server(struct pa_core *c, struct pa_modargs *ma) {
    struct pa_socket_server *s;
#if defined(USE_TCP_SOCKETS) || defined(USE_TCP6_SOCKETS)
    int loopback = 1;
    uint32_t port = IPV4_PORT;

    if (pa_modargs_get_value_boolean(ma, "loopback", &loopback) < 0) {
        pa_log(__FILE__": loopback= expects a numerical argument.\n");
        return NULL;
    }

    if (pa_modargs_get_value_u32(ma, "port", &port) < 0 || port < 1 || port > 0xFFFF) {
        pa_log(__FILE__": port= expects a numerical argument between 1 and 65535.\n");
        return NULL;
    }

#ifdef USE_TCP6_SOCKETS
    if (!(s = pa_socket_server_new_ipv6(c->mainloop, loopback ? (uint8_t*) &in6addr_loopback : (uint8_t*) &in6addr_any, port)))
        return NULL;
#else
    if (!(s = pa_socket_server_new_ipv4(c->mainloop, loopback ? INADDR_LOOPBACK : INADDR_ANY, port, TCPWRAP_SERVICE)))
        return NULL;
#endif
    
#else
    int r;
    const char *v;
    char tmp[PATH_MAX];

    v = pa_modargs_get_value(ma, "socket", UNIX_SOCKET);
    assert(v);

    pa_runtime_path(v, tmp, sizeof(tmp));

    if (pa_make_secure_parent_dir(tmp) < 0) {
        pa_log(__FILE__": Failed to create secure socket directory.\n");
        return NULL;
    }

    if ((r = pa_unix_socket_remove_stale(tmp)) < 0) {
        pa_log(__FILE__": Failed to remove stale UNIX socket '%s': %s\n", tmp, strerror(errno));
        return NULL;
    }
    
    if (r)
        pa_log(__FILE__": Removed stale UNIX socket '%s'.", tmp);
    
    if (!(s = pa_socket_server_new_unix(c->mainloop, tmp)))
        return NULL;
    
#endif
    return s;
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct pa_socket_server *s;
    struct pa_modargs *ma = NULL;
    int ret = -1;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": Failed to parse module arguments\n");
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

void pa__done(struct pa_core *c, struct pa_module*m) {
    assert(c && m);

    protocol_free(m->userdata);
}
