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

#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>

#include "socket-util.h"
#include "util.h"
#include "xmalloc.h"

void pa_socket_peer_to_string(int fd, char *c, size_t l) {
    struct stat st;

    assert(c && l && fd >= 0);
    
    if (fstat(fd, &st) < 0) {
        snprintf(c, l, "Invalid client fd");
        return;
    }

    if (S_ISSOCK(st.st_mode)) {
        union {
            struct sockaddr sa;
            struct sockaddr_in in;
            struct sockaddr_un un;
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
            } else if (sa.sa.sa_family == AF_LOCAL) {
                snprintf(c, l, "UNIX socket client");
                return;
            }

        }
        snprintf(c, l, "Unknown network client");
        return;
    } else if (S_ISCHR(st.st_mode) && (fd == 0 || fd == 1)) {
        snprintf(c, l, "STDIN/STDOUT client");
        return;
    }

    snprintf(c, l, "Unknown client");
}

int pa_socket_low_delay(int fd) {
    int priority;
    assert(fd >= 0);

    priority = 7;
    if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) < 0)
        return -1;

    return 0;
}

int pa_socket_tcp_low_delay(int fd) {
    int ret, tos;

    assert(fd >= 0);

    ret = pa_socket_low_delay(fd);
    
/*     on = 1; */
/*     if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) */
/*         ret = -1; */

    tos = IPTOS_LOWDELAY;
    if (setsockopt(fd, SOL_IP, IP_TOS, &tos, sizeof(tos)) < 0)
        ret = -1;

    return ret;

}

int pa_socket_set_rcvbuf(int fd, size_t l) {
    assert(fd >= 0);

    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &l, sizeof(l)) < 0) {
        fprintf(stderr, "SO_RCVBUF: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int pa_socket_set_sndbuf(int fd, size_t l) {
    assert(fd >= 0);

    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &l, sizeof(l)) < 0) {
        fprintf(stderr, "SO_SNDBUF: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int pa_unix_socket_is_stale(const char *fn) {
    struct sockaddr_un sa;
    int fd = -1, ret = -1;

    if ((fd = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        goto finish;
    }

    sa.sun_family = AF_LOCAL;
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

int pa_unix_socket_make_secure_dir(const char *fn) {
    int ret = -1;
    char *slash, *dir = pa_xstrdup(fn);
    
    if (!(slash = strrchr(dir, '/')))
        goto finish;
    *slash = 0;
    
    if (pa_make_secure_dir(dir) < 0)
        goto finish;

    ret = 0;
    
finish:
    pa_xfree(dir);
    return ret;
}

int pa_unix_socket_remove_secure_dir(const char *fn) {
    int ret = -1;
    char *slash, *dir = pa_xstrdup(fn);
    
    if (!(slash = strrchr(dir, '/')))
        goto finish;
    *slash = 0;

    if (rmdir(dir) < 0)
        goto finish;
    
    ret = 0;
    
finish:
    pa_xfree(dir);
    return ret;
}

