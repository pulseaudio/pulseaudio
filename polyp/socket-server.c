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

#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_LIBWRAP
#include <tcpd.h>
#endif

#include "socket-server.h"
#include "socket-util.h"
#include "xmalloc.h"
#include "util.h"
#include "log.h"

struct pa_socket_server {
    int ref;
    int fd;
    char *filename;
    char *tcpwrap_service;

    void (*on_connection)(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata);
    void *userdata;

    struct pa_io_event *io_event;
    struct pa_mainloop_api *mainloop;
    enum { SOCKET_SERVER_GENERIC, SOCKET_SERVER_IPV4, SOCKET_SERVER_UNIX, SOCKET_SERVER_IPV6 } type;
};

static void callback(struct pa_mainloop_api *mainloop, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    struct pa_socket_server *s = userdata;
    struct pa_iochannel *io;
    int nfd;
    assert(s && s->mainloop == mainloop && s->io_event == e && e && fd >= 0 && fd == s->fd);

    pa_socket_server_ref(s);
    
    if ((nfd = accept(fd, NULL, NULL)) < 0) {
        pa_log(__FILE__": accept(): %s\n", strerror(errno));
        goto finish;
    }

    pa_fd_set_cloexec(nfd, 1);
    
    if (!s->on_connection) {
        close(nfd);
        goto finish;
    }

#ifdef HAVE_LIBWRAP

    if (s->type == SOCKET_SERVER_IPV4 && s->tcpwrap_service) {
        struct request_info req;

        request_init(&req, RQ_DAEMON, s->tcpwrap_service, RQ_FILE, nfd, NULL);
        fromhost(&req);
        if (!hosts_access(&req)) {
            pa_log(__FILE__": TCP connection refused by tcpwrap.\n");
            close(nfd);
            goto finish;
        }

        pa_log(__FILE__": TCP connection accepted by tcpwrap.\n");
    }
#endif
    
    /* There should be a check for socket type here */
    if (s->type == SOCKET_SERVER_IPV4) 
        pa_socket_tcp_low_delay(fd);
    else
        pa_socket_low_delay(fd);
    
    io = pa_iochannel_new(s->mainloop, nfd, nfd);
    assert(io);
    s->on_connection(s, io, s->userdata);

finish:
    pa_socket_server_unref(s);
}

struct pa_socket_server* pa_socket_server_new(struct pa_mainloop_api *m, int fd) {
    struct pa_socket_server *s;
    assert(m && fd >= 0);
    
    s = pa_xmalloc(sizeof(struct pa_socket_server));
    s->ref = 1;
    s->fd = fd;
    s->filename = NULL;
    s->on_connection = NULL;
    s->userdata = NULL;
    s->tcpwrap_service = NULL;

    s->mainloop = m;
    s->io_event = m->io_new(m, fd, PA_IO_EVENT_INPUT, callback, s);
    assert(s->io_event);

    s->type = SOCKET_SERVER_GENERIC;
    
    return s;
}

struct pa_socket_server* pa_socket_server_ref(struct pa_socket_server *s) {
    assert(s && s->ref >= 1);
    s->ref++;
    return s;
}

struct pa_socket_server* pa_socket_server_new_unix(struct pa_mainloop_api *m, const char *filename) {
    int fd = -1;
    struct sockaddr_un sa;
    struct pa_socket_server *s;
    
    assert(m && filename);

    if ((fd = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
        pa_log(__FILE__": socket(): %s\n", strerror(errno));
        goto fail;
    }

    pa_fd_set_cloexec(fd, 1);

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, filename, sizeof(sa.sun_path)-1);
    sa.sun_path[sizeof(sa.sun_path) - 1] = 0;

    pa_socket_low_delay(fd);
    
    if (bind(fd, (struct sockaddr*) &sa, SUN_LEN(&sa)) < 0) {
        pa_log(__FILE__": bind(): %s\n", strerror(errno));
        goto fail;
    }

    if (listen(fd, 5) < 0) {
        pa_log(__FILE__": listen(): %s\n", strerror(errno));
        goto fail;
    }

    s = pa_socket_server_new(m, fd);
    assert(s);

    s->filename = pa_xstrdup(filename);
    s->type = SOCKET_SERVER_UNIX;
    
    return s;
                                                                                                                                                                         
fail:
    if (fd >= 0)
        close(fd);

    return NULL;
}

struct pa_socket_server* pa_socket_server_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port, const char *tcpwrap_service) {
    struct pa_socket_server *ss;
    int fd = -1;
    struct sockaddr_in sa;
    int on = 1;

    assert(m && port);

    if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        pa_log(__FILE__": socket(): %s\n", strerror(errno));
        goto fail;
    }

    pa_fd_set_cloexec(fd, 1);

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        pa_log(__FILE__": setsockopt(): %s\n", strerror(errno));

    pa_socket_tcp_low_delay(fd);
    
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(address);

    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        pa_log(__FILE__": bind(): %s\n", strerror(errno));
        goto fail;
    }

    if (listen(fd, 5) < 0) {
        pa_log(__FILE__": listen(): %s\n", strerror(errno));
        goto fail;
    }

    if ((ss = pa_socket_server_new(m, fd))) {
        ss->type = SOCKET_SERVER_IPV4;
        ss->tcpwrap_service = pa_xstrdup(tcpwrap_service);
    }

    return ss;
    
fail:
    if (fd >= 0)
        close(fd);

    return NULL;
}

struct pa_socket_server* pa_socket_server_new_ipv6(struct pa_mainloop_api *m, uint8_t address[16], uint16_t port) {
    struct pa_socket_server *ss;
    int fd = -1;
    struct sockaddr_in6 sa;
    int on = 1;

    assert(m && port);

    if ((fd = socket(PF_INET6, SOCK_STREAM, 0)) < 0) {
        pa_log(__FILE__": socket(): %s\n", strerror(errno));
        goto fail;
    }

    pa_fd_set_cloexec(fd, 1);

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        pa_log(__FILE__": setsockopt(): %s\n", strerror(errno));

    pa_socket_tcp_low_delay(fd);

    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    memcpy(sa.sin6_addr.s6_addr, address, 16);

    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        pa_log(__FILE__": bind(): %s\n", strerror(errno));
        goto fail;
    }

    if (listen(fd, 5) < 0) {
        pa_log(__FILE__": listen(): %s\n", strerror(errno));
        goto fail;
    }

    if ((ss = pa_socket_server_new(m, fd)))
        ss->type = SOCKET_SERVER_IPV6;

    return ss;
    
fail:
    if (fd >= 0)
        close(fd);

    return NULL;
}

static void socket_server_free(struct pa_socket_server*s) {
    assert(s);
    close(s->fd);

    if (s->filename) {
        unlink(s->filename);
        pa_xfree(s->filename);
    }

    pa_xfree(s->tcpwrap_service);

    s->mainloop->io_free(s->io_event);
    pa_xfree(s);
}

void pa_socket_server_unref(struct pa_socket_server *s) {
    assert(s && s->ref >= 1);

    if (!(--(s->ref)))
        socket_server_free(s);
}

void pa_socket_server_set_callback(struct pa_socket_server*s, void (*on_connection)(struct pa_socket_server*s, struct pa_iochannel *io, void *userdata), void *userdata) {
    assert(s && s->ref >= 1);

    s->on_connection = on_connection;
    s->userdata = userdata;
}


char *pa_socket_server_get_address(struct pa_socket_server *s, char *c, size_t l) {
    assert(s && c && l > 0);
    
    switch (s->type) {
        case SOCKET_SERVER_IPV6: {
            struct sockaddr_in6 sa;
            socklen_t l = sizeof(sa);

            if (getsockname(s->fd, (struct sockaddr*) &sa, &l) < 0) {
                pa_log(__FILE__": getsockname() failed: %s\n", strerror(errno));
                return NULL;
            }

            if (memcmp(&in6addr_any, &sa.sin6_addr, sizeof(in6addr_any)) == 0) {
                char fqdn[256];
                if (!pa_get_fqdn(fqdn, sizeof(fqdn)))
                    return NULL;
                
                snprintf(c, l, "tcp6:%s:%u", fqdn, (unsigned) ntohs(sa.sin6_port));
                
            } else if (memcmp(&in6addr_loopback, &sa.sin6_addr, sizeof(in6addr_loopback)) == 0) {
                char hn[256];
                if (!pa_get_host_name(hn, sizeof(hn)))
                    return NULL;
                
                snprintf(c, l, "{%s}tcp6:localhost:%u", hn, (unsigned) ntohs(sa.sin6_port));
            } else {
                char ip[INET6_ADDRSTRLEN];
                
                if (!inet_ntop(AF_INET6, &sa.sin6_addr, ip, sizeof(ip))) {
                    pa_log(__FILE__": inet_ntop() failed: %s\n", strerror(errno));
                    return NULL;
                }
                
                snprintf(c, l, "tcp6:[%s]:%u", ip, (unsigned) ntohs(sa.sin6_port));
            }

            return c;
        }

        case SOCKET_SERVER_IPV4: {
            struct sockaddr_in sa;
            socklen_t l = sizeof(sa);

            if (getsockname(s->fd, &sa, &l) < 0) {
                pa_log(__FILE__": getsockname() failed: %s\n", strerror(errno));
                return NULL;
            }

            if (sa.sin_addr.s_addr == INADDR_ANY) {
                char fqdn[256];
                if (!pa_get_fqdn(fqdn, sizeof(fqdn)))
                    return NULL;
                
                snprintf(c, l, "tcp:%s:%u", fqdn, (unsigned) ntohs(sa.sin_port));
            } else if (sa.sin_addr.s_addr == INADDR_LOOPBACK) {
                char hn[256];
                if (!pa_get_host_name(hn, sizeof(hn)))
                    return NULL;
                
                snprintf(c, l, "{%s}tcp:localhost:%u", hn, (unsigned) ntohs(sa.sin_port));
            } else {
                char ip[INET_ADDRSTRLEN];

                if (!inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip))) {
                    pa_log(__FILE__": inet_ntop() failed: %s\n", strerror(errno));
                    return NULL;
                }
                
                snprintf(c, l, "tcp:[%s]:%u", ip, (unsigned) ntohs(sa.sin_port));

            }
            
            return c;
        }

        case SOCKET_SERVER_UNIX: {
            char hn[256];

            if (!s->filename)
                return NULL;
            
            if (!pa_get_host_name(hn, sizeof(hn)))
                return NULL;

            snprintf(c, l, "{%s}unix:%s", hn, s->filename);
            return c;
        }

        default:
            return NULL;
    }
}
