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
#include <unistd.h>
#include <limits.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "../polypcore/winsock.h"

#include <polyp/xmalloc.h>

#include <polypcore/module.h>
#include <polypcore/socket-server.h>
#include <polypcore/socket-util.h>
#include <polypcore/core-util.h>
#include <polypcore/modargs.h>
#include <polypcore/log.h>
#include <polypcore/native-common.h>

#ifdef USE_TCP_SOCKETS
#define SOCKET_DESCRIPTION "(TCP sockets)"
#define SOCKET_USAGE "port=<TCP port number> loopback=<listen on loopback device only?> listen=<address to listen on>"
#else
#define SOCKET_DESCRIPTION "(UNIX sockets)"
#define SOCKET_USAGE "socket=<path to UNIX socket>"
#endif

#if defined(USE_PROTOCOL_SIMPLE)
  #include <polypcore/protocol-simple.h>
  #define protocol_new pa_protocol_simple_new
  #define protocol_free pa_protocol_simple_free
  #define TCPWRAP_SERVICE "polypaudio-simple"
  #define IPV4_PORT 4711
  #define UNIX_SOCKET "simple"
  #define MODULE_ARGUMENTS "rate", "format", "channels", "sink", "source", "playback", "record",
  #if defined(USE_TCP_SOCKETS)
    #include "module-simple-protocol-tcp-symdef.h"
  #else
    #include "module-simple-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("Simple protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("rate=<sample rate> format=<sample format> channels=<number of channels> sink=<sink to connect to> source=<source to connect to> playback=<enable playback?> record=<enable record?> "SOCKET_USAGE)
#elif defined(USE_PROTOCOL_CLI)
  #include <polypcore/protocol-cli.h> 
  #define protocol_new pa_protocol_cli_new
  #define protocol_free pa_protocol_cli_free
  #define TCPWRAP_SERVICE "polypaudio-cli"
  #define IPV4_PORT 4712
  #define UNIX_SOCKET "cli"
  #define MODULE_ARGUMENTS 
  #ifdef USE_TCP_SOCKETS
    #include "module-cli-protocol-tcp-symdef.h"
  #else
    #include "module-cli-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("Command line interface protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE(SOCKET_USAGE)
#elif defined(USE_PROTOCOL_HTTP)
  #include <polypcore/protocol-http.h>
  #define protocol_new pa_protocol_http_new
  #define protocol_free pa_protocol_http_free
  #define TCPWRAP_SERVICE "polypaudio-http"
  #define IPV4_PORT 4714
  #define UNIX_SOCKET "http"
  #define MODULE_ARGUMENTS 
  #ifdef USE_TCP_SOCKETS
    #include "module-http-protocol-tcp-symdef.h"
  #else
    #include "module-http-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("HTTP "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE(SOCKET_USAGE)
#elif defined(USE_PROTOCOL_NATIVE)
  #include <polypcore/protocol-native.h>
  #define protocol_new pa_protocol_native_new
  #define protocol_free pa_protocol_native_free
  #define TCPWRAP_SERVICE "polypaudio-native"
  #define IPV4_PORT PA_NATIVE_DEFAULT_PORT
  #define UNIX_SOCKET PA_NATIVE_DEFAULT_UNIX_SOCKET
  #define MODULE_ARGUMENTS_COMMON "cookie", "auth-anonymous",
  #ifdef USE_TCP_SOCKETS
    #include "module-native-protocol-tcp-symdef.h"
  #else
    #include "module-native-protocol-unix-symdef.h"
  #endif

  #if defined(SCM_CREDENTIALS) && !defined(USE_TCP_SOCKETS)
    #define MODULE_ARGUMENTS MODULE_ARGUMENTS_COMMON "auth-group",
    #define AUTH_USAGE "auth-group=<local group to allow access>"
  #else
    #define MODULE_ARGUMENTS MODULE_ARGUMENTS_COMMON
    #define AUTH_USAGE
  #endif
  
  PA_MODULE_DESCRIPTION("Native protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("auth-anonymous=<don't check for cookies?> cookie=<path to cookie file> "AUTH_USAGE SOCKET_USAGE)
#elif defined(USE_PROTOCOL_ESOUND)
  #include <polypcore/protocol-esound.h>
  #include <polypcore/esound.h>
  #define protocol_new pa_protocol_esound_new
  #define protocol_free pa_protocol_esound_free
  #define TCPWRAP_SERVICE "esound"
  #define IPV4_PORT ESD_DEFAULT_PORT
  #define UNIX_SOCKET ESD_UNIX_SOCKET_NAME
  #define MODULE_ARGUMENTS "sink", "source", "auth-anonymous", "cookie",
  #ifdef USE_TCP_SOCKETS
    #include "module-esound-protocol-tcp-symdef.h"
  #else
    #include "module-esound-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("ESOUND protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("sink=<sink to connect to> source=<source to connect to> auth-anonymous=<don't check for cookies?> cookie=<path to cookie file> "SOCKET_USAGE)
#else
  #error "Broken build system" 
#endif

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_VERSION(PACKAGE_VERSION)

static const char* const valid_modargs[] = {
    MODULE_ARGUMENTS
#if defined(USE_TCP_SOCKETS)
    "port",
    "loopback",
    "listen",
#else
    "socket",
#endif
    NULL
};

struct userdata {
#if defined(USE_TCP_SOCKETS)
    void *protocol_ipv4;
    void *protocol_ipv6;
#else
    void *protocol_unix;
    char *socket_path;
#endif
};

int pa__init(pa_core *c, pa_module*m) {
    pa_modargs *ma = NULL;
    int ret = -1;

    struct userdata *u = NULL;

#if defined(USE_TCP_SOCKETS)
    pa_socket_server *s_ipv4 = NULL, *s_ipv6 = NULL;
    int loopback = 1;
    uint32_t port = IPV4_PORT;
    const char *listen_on;
#else
    pa_socket_server *s;
    int r;
    const char *v;
    char tmp[PATH_MAX];
#endif

    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": Failed to parse module arguments");
        goto finish;
    }

    u = pa_xnew0(struct userdata, 1);

#if defined(USE_TCP_SOCKETS)
    if (pa_modargs_get_value_boolean(ma, "loopback", &loopback) < 0) {
        pa_log(__FILE__": loopback= expects a boolean argument.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "port", &port) < 0 || port < 1 || port > 0xFFFF) {
        pa_log(__FILE__": port= expects a numerical argument between 1 and 65535.");
        goto fail;
    }

    listen_on = pa_modargs_get_value(ma, "listen", NULL);

    if (listen_on) {
        s_ipv6 = pa_socket_server_new_ipv6_string(c->mainloop, listen_on, port, TCPWRAP_SERVICE);
        s_ipv4 = pa_socket_server_new_ipv4_string(c->mainloop, listen_on, port, TCPWRAP_SERVICE);
    } else if (loopback) {
        s_ipv6 = pa_socket_server_new_ipv6_loopback(c->mainloop, port, TCPWRAP_SERVICE);
        s_ipv4 = pa_socket_server_new_ipv4_loopback(c->mainloop, port, TCPWRAP_SERVICE);
    } else {
        s_ipv6 = pa_socket_server_new_ipv6_any(c->mainloop, port, TCPWRAP_SERVICE);
        s_ipv4 = pa_socket_server_new_ipv4_any(c->mainloop, port, TCPWRAP_SERVICE);
    }

    if (!s_ipv4 && !s_ipv6)
        goto fail;

    if (s_ipv4)
        if (!(u->protocol_ipv4 = protocol_new(c, s_ipv4, m, ma)))
            pa_socket_server_unref(s_ipv4);

    if (s_ipv6)
        if (!(u->protocol_ipv6 = protocol_new(c, s_ipv6, m, ma)))
            pa_socket_server_unref(s_ipv6);

    if (!u->protocol_ipv4 && !u->protocol_ipv6)
        goto fail;

#else
    v = pa_modargs_get_value(ma, "socket", UNIX_SOCKET);
    pa_runtime_path(v, tmp, sizeof(tmp));
    u->socket_path = pa_xstrdup(tmp);

    if (pa_make_secure_parent_dir(tmp) < 0) {
        pa_log(__FILE__": Failed to create secure socket directory.");
        goto fail;
    }

    if ((r = pa_unix_socket_remove_stale(tmp)) < 0) {
        pa_log(__FILE__": Failed to remove stale UNIX socket '%s': %s", tmp, strerror(errno));
        goto fail;
    }
    
    if (r)
        pa_log(__FILE__": Removed stale UNIX socket '%s'.", tmp);
    
    if (!(s = pa_socket_server_new_unix(c->mainloop, tmp)))
        goto fail;

    if (!(u->protocol_unix = protocol_new(c, s, m, ma)))
        goto fail;

#endif

    m->userdata = u;

    ret = 0;

finish:
    if (ma)
        pa_modargs_free(ma);

    return ret;

fail:
    if (u) {
#if defined(USE_TCP_SOCKETS)
        if (u->protocol_ipv4)
            protocol_free(u->protocol_ipv4);
        if (u->protocol_ipv6)
            protocol_free(u->protocol_ipv6);
#else
        if (u->protocol_unix)
            protocol_free(u->protocol_unix);

        if (u->socket_path)
            pa_xfree(u->socket_path);
#endif

        pa_xfree(u);
    } else {
#if defined(USE_TCP_SOCKETS)
        if (s_ipv4)
            pa_socket_server_unref(s_ipv4);
        if (s_ipv6)
            pa_socket_server_unref(s_ipv6);
#else
        if (s)
            pa_socket_server_unref(s);
#endif
    }

    goto finish;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    
    assert(c);
    assert(m);

    u = m->userdata;

#if defined(USE_TCP_SOCKETS)
    if (u->protocol_ipv4)
        protocol_free(u->protocol_ipv4);
    if (u->protocol_ipv6)
        protocol_free(u->protocol_ipv6);
#else
    if (u->protocol_unix)
        protocol_free(u->protocol_unix);

    if (u->socket_path) {
        char *p;
        
        if ((p = pa_parent_dir(u->socket_path))) {
            if (rmdir(p) < 0 && errno != ENOENT && errno != ENOTEMPTY)
                pa_log(__FILE__": Failed to remove %s: %s.", u->socket_path, strerror(errno));

            pa_xfree(p);
        }

        pa_xfree(u->socket_path);
    }
#endif

    pa_xfree(u);
}
