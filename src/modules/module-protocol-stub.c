/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <string.h>
#include <errno.h>
#include <stdio.h>
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

#include <pulse/xmalloc.h>

#include <pulsecore/winsock.h>
#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/socket-server.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/native-common.h>
#include <pulsecore/creds.h>

#ifdef USE_TCP_SOCKETS
#define SOCKET_DESCRIPTION "(TCP sockets)"
#define SOCKET_USAGE "port=<TCP port number> listen=<address to listen on>"
#else
#define SOCKET_DESCRIPTION "(UNIX sockets)"
#define SOCKET_USAGE "socket=<path to UNIX socket>"
#endif

#if defined(USE_PROTOCOL_SIMPLE)
  #include <pulsecore/protocol-simple.h>
  #define protocol_new pa_protocol_simple_new
  #define protocol_free pa_protocol_simple_free
  #define TCPWRAP_SERVICE "pulseaudio-simple"
  #define IPV4_PORT 4711
  #define UNIX_SOCKET "simple"
  #define MODULE_ARGUMENTS "rate", "format", "channels", "sink", "source", "playback", "record",
  #if defined(USE_TCP_SOCKETS)
    #include "module-simple-protocol-tcp-symdef.h"
  #else
    #include "module-simple-protocol-unix-symdef.h"
  #endif
  PA_MODULE_DESCRIPTION("Simple protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("rate=<sample rate> "
                  "format=<sample format> "
                  "channels=<number of channels> "
                  "sink=<sink to connect to> "
                  "source=<source to connect to> "
                  "playback=<enable playback?> "
                  "record=<enable record?> "
                  SOCKET_USAGE)
#elif defined(USE_PROTOCOL_CLI)
  #include <pulsecore/protocol-cli.h>
  #define protocol_new pa_protocol_cli_new
  #define protocol_free pa_protocol_cli_free
  #define TCPWRAP_SERVICE "pulseaudio-cli"
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
  #include <pulsecore/protocol-http.h>
  #define protocol_new pa_protocol_http_new
  #define protocol_free pa_protocol_http_free
  #define TCPWRAP_SERVICE "pulseaudio-http"
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
  #include <pulsecore/protocol-native.h>
  #define protocol_new pa_protocol_native_new
  #define protocol_free pa_protocol_native_free
  #define TCPWRAP_SERVICE "pulseaudio-native"
  #define IPV4_PORT PA_NATIVE_DEFAULT_PORT
  #define UNIX_SOCKET PA_NATIVE_DEFAULT_UNIX_SOCKET
  #define MODULE_ARGUMENTS_COMMON "cookie", "auth-anonymous",
  #ifdef USE_TCP_SOCKETS
    #include "module-native-protocol-tcp-symdef.h"
  #else
    #include "module-native-protocol-unix-symdef.h"
  #endif

  #if defined(HAVE_CREDS) && !defined(USE_TCP_SOCKETS)
    #define MODULE_ARGUMENTS MODULE_ARGUMENTS_COMMON "auth-group", "auth-group-enable",
    #define AUTH_USAGE "auth-group=<system group to allow access> auth-group-enable=<enable auth by UNIX group?> "
  #elif defined(USE_TCP_SOCKETS)
    #define MODULE_ARGUMENTS MODULE_ARGUMENTS_COMMON "auth-ip-acl",
    #define AUTH_USAGE "auth-ip-acl=<IP address ACL to allow access> "
  #else
    #define MODULE_ARGUMENTS MODULE_ARGUMENTS_COMMON
    #define AUTH_USAGE
  #endif

  PA_MODULE_DESCRIPTION("Native protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("auth-anonymous=<don't check for cookies?> "
                  "cookie=<path to cookie file> "
                  AUTH_USAGE
                  SOCKET_USAGE)
#elif defined(USE_PROTOCOL_ESOUND)
  #include <pulsecore/protocol-esound.h>
  #include <pulsecore/esound.h>
  #define protocol_new pa_protocol_esound_new
  #define protocol_free pa_protocol_esound_free
  #define TCPWRAP_SERVICE "esound"
  #define IPV4_PORT ESD_DEFAULT_PORT
  #define MODULE_ARGUMENTS_COMMON "sink", "source", "auth-anonymous", "cookie",
  #ifdef USE_TCP_SOCKETS
    #include "module-esound-protocol-tcp-symdef.h"
  #else
    #include "module-esound-protocol-unix-symdef.h"
  #endif

  #if defined(USE_TCP_SOCKETS)
    #define MODULE_ARGUMENTS MODULE_ARGUMENTS_COMMON "auth-ip-acl",
    #define AUTH_USAGE "auth-ip-acl=<IP address ACL to allow access> "
  #else
    #define MODULE_ARGUMENTS MODULE_ARGUMENTS_COMMON
    #define AUTH_USAGE
  #endif

  PA_MODULE_DESCRIPTION("ESOUND protocol "SOCKET_DESCRIPTION)
  PA_MODULE_USAGE("sink=<sink to connect to> "
                  "source=<source to connect to> "
                  "auth-anonymous=<don't verify cookies?> "
                  "cookie=<path to cookie file> "
                  AUTH_USAGE
                  SOCKET_USAGE)
#else
  #error "Broken build system"
#endif

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_VERSION(PACKAGE_VERSION)

static const char* const valid_modargs[] = {
    MODULE_ARGUMENTS
#if defined(USE_TCP_SOCKETS)
    "port",
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

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    int ret = -1;
    struct userdata *u = NULL;

#if defined(USE_TCP_SOCKETS)
    pa_socket_server *s_ipv4 = NULL, *s_ipv6 = NULL;
    uint32_t port = IPV4_PORT;
    const char *listen_on;
#else
    pa_socket_server *s;
    int r;
    char tmp[PATH_MAX];

#if defined(USE_PROTOCOL_ESOUND)
    char tmp2[PATH_MAX];
#endif
#endif

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto finish;
    }

    u = pa_xnew0(struct userdata, 1);

#if defined(USE_TCP_SOCKETS)
    if (pa_modargs_get_value_u32(ma, "port", &port) < 0 || port < 1 || port > 0xFFFF) {
        pa_log("port= expects a numerical argument between 1 and 65535.");
        goto fail;
    }

    listen_on = pa_modargs_get_value(ma, "listen", NULL);

    if (listen_on) {
        s_ipv6 = pa_socket_server_new_ipv6_string(m->core->mainloop, listen_on, port, TCPWRAP_SERVICE);
        s_ipv4 = pa_socket_server_new_ipv4_string(m->core->mainloop, listen_on, port, TCPWRAP_SERVICE);
    } else {
        s_ipv6 = pa_socket_server_new_ipv6_any(m->core->mainloop, port, TCPWRAP_SERVICE);
        s_ipv4 = pa_socket_server_new_ipv4_any(m->core->mainloop, port, TCPWRAP_SERVICE);
    }

    if (!s_ipv4 && !s_ipv6)
        goto fail;

    if (s_ipv4)
        if (!(u->protocol_ipv4 = protocol_new(m->core, s_ipv4, m, ma)))
            pa_socket_server_unref(s_ipv4);

    if (s_ipv6)
        if (!(u->protocol_ipv6 = protocol_new(m->core, s_ipv6, m, ma)))
            pa_socket_server_unref(s_ipv6);

    if (!u->protocol_ipv4 && !u->protocol_ipv6)
        goto fail;

#else

#if defined(USE_PROTOCOL_ESOUND)

    snprintf(tmp2, sizeof(tmp2), "/tmp/.esd-%lu/socket", (unsigned long) getuid());
    pa_runtime_path(pa_modargs_get_value(ma, "socket", tmp2), tmp, sizeof(tmp));
    u->socket_path = pa_xstrdup(tmp);

    /* This socket doesn't reside in our own runtime dir but in
     * /tmp/.esd/, hence we have to create the dir first */

    if (pa_make_secure_parent_dir(u->socket_path, m->core->is_system_instance ? 0755 : 0700, (uid_t)-1, (gid_t)-1) < 0) {
        pa_log("Failed to create socket directory '%s': %s\n", u->socket_path, pa_cstrerror(errno));
        goto fail;
    }

#else
    pa_runtime_path(pa_modargs_get_value(ma, "socket", UNIX_SOCKET), tmp, sizeof(tmp));
    u->socket_path = pa_xstrdup(tmp);
#endif

    if ((r = pa_unix_socket_remove_stale(tmp)) < 0) {
        pa_log("Failed to remove stale UNIX socket '%s': %s", tmp, pa_cstrerror(errno));
        goto fail;
    }

    if (r)
        pa_log("Removed stale UNIX socket '%s'.", tmp);

    if (!(s = pa_socket_server_new_unix(m->core->mainloop, tmp)))
        goto fail;

    if (!(u->protocol_unix = protocol_new(m->core, s, m, ma)))
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

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    u = m->userdata;

#if defined(USE_TCP_SOCKETS)
    if (u->protocol_ipv4)
        protocol_free(u->protocol_ipv4);
    if (u->protocol_ipv6)
        protocol_free(u->protocol_ipv6);
#else
    if (u->protocol_unix)
        protocol_free(u->protocol_unix);

#if defined(USE_PROTOCOL_ESOUND)
    if (u->socket_path) {
        char *p = pa_parent_dir(u->socket_path);
        rmdir(p);
        pa_xfree(p);
    }
#endif

    pa_xfree(u->socket_path);
#endif

    pa_xfree(u);
}
