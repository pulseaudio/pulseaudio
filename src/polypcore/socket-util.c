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

#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_NETINET_IN_SYSTM_H
#include <netinet/in_systm.h>
#endif
#ifdef HAVE_NETINET_IP_H
#include <netinet/ip.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#include "winsock.h"

#include <polypcore/util.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>

#include "socket-util.h"

void pa_socket_peer_to_string(int fd, char *c, size_t l) {
    struct stat st;

    assert(c && l && fd >= 0);
    
#ifndef OS_IS_WIN32
    if (fstat(fd, &st) < 0) {
        snprintf(c, l, "Invalid client fd");
        return;
    }
#endif

#ifndef OS_IS_WIN32
    if (S_ISSOCK(st.st_mode)) {
#endif    
        union {
            struct sockaddr sa;
            struct sockaddr_in in;
#ifdef HAVE_SYS_UN_H
            struct sockaddr_un un;
#endif
        } sa;
        socklen_t sa_len = sizeof(sa);
        
        if (getpeername(fd, &sa.sa, &sa_len) >= 0) {

            if (sa.sa.sa_family == AF_INET) {
                uint32_t ip = ntohl(sa.in.sin_addr.s_addr);
                
                snprintf(c, l, "TCP/IP client from %i.%i.%i.%i:%u",
                         ip >> 24,
                         (ip >> 16) & 0xFF,
                         (ip >> 8) & 0xFF,
                         ip & 0xFF,
                         ntohs(sa.in.sin_port));
                return;
#ifdef HAVE_SYS_UN_H
            } else if (sa.sa.sa_family == AF_UNIX) {
                snprintf(c, l, "UNIX socket client");
                return;
#endif
            }

        }
#ifndef OS_IS_WIN32
        snprintf(c, l, "Unknown network client");
        return;
    } else if (S_ISCHR(st.st_mode) && (fd == 0 || fd == 1)) {
        snprintf(c, l, "STDIN/STDOUT client");
        return;
    }
#endif /* OS_IS_WIN32 */

    snprintf(c, l, "Unknown client");
}

int pa_socket_low_delay(int fd) {
#ifdef SO_PRIORITY
    int priority;
    assert(fd >= 0);

    priority = 7;
    if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, (void*)&priority, sizeof(priority)) < 0)
        return -1;
#endif

    return 0;
}

int pa_socket_tcp_low_delay(int fd) {
    int ret, tos, on;

    assert(fd >= 0);

    ret = pa_socket_low_delay(fd);
    
    on = 1;
    tos = 0;

#if defined(SOL_TCP) || defined(IPPROTO_TCP)
#if defined(SOL_TCP)
    if (setsockopt(fd, SOL_TCP, TCP_NODELAY, (void*)&on, sizeof(on)) < 0)
#else
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)&on, sizeof(on)) < 0)
#endif
        ret = -1;
#endif

#if defined(IPTOS_LOWDELAY) && defined(IP_TOS) && (defined(SOL_IP) || \
	defined(IPPROTO_IP))
    tos = IPTOS_LOWDELAY;
#ifdef SOL_IP
    if (setsockopt(fd, SOL_IP, IP_TOS, (void*)&tos, sizeof(tos)) < 0)
#else
    if (setsockopt(fd, IPPROTO_IP, IP_TOS, (void*)&tos, sizeof(tos)) < 0)
#endif
        ret = -1;
#endif

    return ret;

}

int pa_socket_set_rcvbuf(int fd, size_t l) {
    assert(fd >= 0);

/*     if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void*)&l, sizeof(l)) < 0) { */
/*         pa_log(__FILE__": SO_RCVBUF: %s\n", strerror(errno)); */
/*         return -1; */
/*     } */

    return 0;
}

int pa_socket_set_sndbuf(int fd, size_t l) {
    assert(fd >= 0);

/*     if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void*)&l, sizeof(l)) < 0) { */
/*         pa_log(__FILE__": SO_SNDBUF: %s\n", strerror(errno)); */
/*         return -1; */
/*     } */

    return 0;
}

#ifdef HAVE_SYS_UN_H

int pa_unix_socket_is_stale(const char *fn) {
    struct sockaddr_un sa;
    int fd = -1, ret = -1;

    if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
        pa_log(__FILE__": socket(): %s\n", strerror(errno));
        goto finish;
    }

    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, fn, sizeof(sa.sun_path)-1);
    sa.sun_path[sizeof(sa.sun_path) - 1] = 0;

    if (connect(fd, (struct sockaddr*) &sa, sizeof(sa)) < 0) {
        if (errno == ECONNREFUSED)
            ret = 1;
    } else
        ret = 0;

finish:
    if (fd >= 0)
        close(fd);

    return ret;
}

int pa_unix_socket_remove_stale(const char *fn) {
    int r;
    
    if ((r = pa_unix_socket_is_stale(fn)) < 0)
        return errno != ENOENT ? -1 : 0;

    if (!r)
        return 0;
        
    /* Yes, here is a race condition. But who cares? */
    if (unlink(fn) < 0)
        return -1;

    return 0;
}

#else /* HAVE_SYS_UN_H */

int pa_unix_socket_is_stale(const char *fn) {
    return -1;
}

int pa_unix_socket_remove_stale(const char *fn) {
    return -1;
}

#endif /* HAVE_SYS_UN_H */
