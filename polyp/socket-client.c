/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "socket-client.h"
#include "socket-util.h"
#include "util.h"
#include "xmalloc.h"
#include "log.h"

struct pa_socket_client {
    int ref;
    struct pa_mainloop_api *mainloop;
    int fd;
    struct pa_io_event *io_event;
    struct pa_defer_event *defer_event;
    void (*callback)(struct pa_socket_client*c, struct pa_iochannel *io, void *userdata);
    void *userdata;
    int local;
};

static struct pa_socket_client*pa_socket_client_new(struct pa_mainloop_api *m) {
    struct pa_socket_client *c;
    assert(m);

    c = pa_xmalloc(sizeof(struct pa_socket_client));
    c->ref = 1;
    c->mainloop = m;
    c->fd = -1;
    c->io_event = NULL;
    c->defer_event = NULL;
    c->callback = NULL;
    c->userdata = NULL;
    c->local = 0;
    return c;
}

static void do_call(struct pa_socket_client *c) {
    struct pa_iochannel *io = NULL;
    int error;
    socklen_t lerror;
    assert(c && c->callback);

    pa_socket_client_ref(c);
    
    lerror = sizeof(error);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &lerror) < 0) {
        pa_log(__FILE__": getsockopt(): %s\n", strerror(errno));
        goto finish;
    }

    if (lerror != sizeof(error)) {
        pa_log(__FILE__": getsockopt() returned invalid size.\n");
        goto finish;
    }

    if (error != 0) {
/*         pa_log(__FILE__": connect(): %s\n", strerror(error)); */
        errno = error;
        goto finish;
    }
        
    io = pa_iochannel_new(c->mainloop, c->fd, c->fd);
    assert(io);
    
finish:
    if (!io)
        close(c->fd);
    c->fd = -1;
    
    assert(c->callback);
    c->callback(c, io, c->userdata);
    
    pa_socket_client_unref(c);
}

static void connect_fixed_cb(struct pa_mainloop_api *m, struct pa_defer_event *e, void *userdata) {
    struct pa_socket_client *c = userdata;
    assert(m && c && c->defer_event == e);
    m->defer_free(c->defer_event);
    c->defer_event = NULL;
    do_call(c);
}

static void connect_io_cb(struct pa_mainloop_api*m, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    struct pa_socket_client *c = userdata;
    assert(m && c && c->io_event == e && fd >= 0);
    m->io_free(c->io_event);
    c->io_event = NULL;
    do_call(c);
}

static int do_connect(struct pa_socket_client *c, const struct sockaddr *sa, socklen_t len) {
    int r;
    assert(c && sa && len);
    
    pa_make_nonblock_fd(c->fd);
    
    if ((r = connect(c->fd, sa, len)) < 0) {
        if (errno != EINPROGRESS) {
            /*pa_log(__FILE__": connect(): %s\n", strerror(errno));*/
            return -1;
        }

        c->io_event = c->mainloop->io_new(c->mainloop, c->fd, PA_IO_EVENT_OUTPUT, connect_io_cb, c);
        assert(c->io_event);
    } else {
        c->defer_event = c->mainloop->defer_new(c->mainloop, connect_fixed_cb, c);
        assert(c->defer_event);
    }

    return 0;
}

struct pa_socket_client* pa_socket_client_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port) {
    struct sockaddr_in sa;
    assert(m && port > 0);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(address);

    return pa_socket_client_new_sockaddr(m, (struct sockaddr*) &sa, sizeof(sa));
}

struct pa_socket_client* pa_socket_client_new_unix(struct pa_mainloop_api *m, const char *filename) {
    struct sockaddr_un sa;
    assert(m && filename);
    
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, filename, sizeof(sa.sun_path)-1);
    sa.sun_path[sizeof(sa.sun_path) - 1] = 0;

    return pa_socket_client_new_sockaddr(m, (struct sockaddr*) &sa, sizeof(sa));
}

struct pa_socket_client* pa_socket_client_new_sockaddr(struct pa_mainloop_api *m, const struct sockaddr *sa, size_t salen) {
    struct pa_socket_client *c;
    assert(m && sa);
    c = pa_socket_client_new(m);
    assert(c);

    switch (sa->sa_family) {
        case AF_UNIX:
            c->local = 1;
            break;
            
        case AF_INET:
            c->local = ((const struct sockaddr_in*) sa)->sin_addr.s_addr == INADDR_LOOPBACK;
            break;
            
        case AF_INET6:
            c->local = memcmp(&((const struct sockaddr_in6*) sa)->sin6_addr, &in6addr_loopback, sizeof(struct in6_addr)) == 0;
            break;
            
        default:
            c->local = 0;
    }
    
    if ((c->fd = socket(sa->sa_family, SOCK_STREAM, 0)) < 0) {
        pa_log(__FILE__": socket(): %s\n", strerror(errno));
        goto fail;
    }

    pa_fd_set_cloexec(c->fd, 1);
    if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6)
        pa_socket_tcp_low_delay(c->fd);
    else
        pa_socket_low_delay(c->fd);

    if (do_connect(c, sa, salen) < 0)
        goto fail;
    
    return c;

fail:
    pa_socket_client_unref(c);
    return NULL;
    
}

void socket_client_free(struct pa_socket_client *c) {
    assert(c && c->mainloop);
    if (c->io_event)
        c->mainloop->io_free(c->io_event);
    if (c->defer_event)
        c->mainloop->defer_free(c->defer_event);
    if (c->fd >= 0)
        close(c->fd);
    pa_xfree(c);
}

void pa_socket_client_unref(struct pa_socket_client *c) {
    assert(c && c->ref >= 1);

    if (!(--(c->ref)))
        socket_client_free(c);
}

struct pa_socket_client* pa_socket_client_ref(struct pa_socket_client *c) {
    assert(c && c->ref >= 1);
    c->ref++;
    return c;
}

void pa_socket_client_set_callback(struct pa_socket_client *c, void (*on_connection)(struct pa_socket_client *c, struct pa_iochannel*io, void *userdata), void *userdata) {
    assert(c);
    c->callback = on_connection;
    c->userdata = userdata;
}

struct pa_socket_client* pa_socket_client_new_ipv6(struct pa_mainloop_api *m, uint8_t address[16], uint16_t port) {
    struct sockaddr_in6 sa;
    
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    memcpy(&sa.sin6_addr, address, sizeof(sa.sin6_addr));

    return pa_socket_client_new_sockaddr(m, (struct sockaddr*) &sa, sizeof(sa));
}

/* Parse addresses in one of the following forms:
 *    HOSTNAME
 *    HOSTNAME:PORT
 *    [HOSTNAME]
 *    [HOSTNAME]:PORT
 *
 *  Return a newly allocated string of the hostname and fill in *port if specified  */

static char *parse_address(const char *s, uint16_t *port) {
    assert(s && port);
    if (*s == '[') {
        char *e;
        if (!(e = strchr(s+1, ']')))
            return NULL;

        if (e[1] == ':')
            *port = atoi(e+2);
        else if (e[1] != 0)
            return NULL;
        
        return pa_xstrndup(s+1, e-s-1);
    } else {
        char *e;
        
        if (!(e = strrchr(s, ':')))
            return pa_xstrdup(s);

        *port = atoi(e+1);
        return pa_xstrndup(s, e-s);
    }
}

struct pa_socket_client* pa_socket_client_new_string(struct pa_mainloop_api *m, const char*name, uint16_t default_port) {
    const char *p;
    struct pa_socket_client *c = NULL;
    enum { KIND_UNIX, KIND_TCP_AUTO, KIND_TCP4, KIND_TCP6 } kind = KIND_TCP_AUTO;
    assert(m && name);

    if (*name == '{') {
        char hn[256], *pfx;
        /* The URL starts with a host specification for detecting local connections */
        
        if (!pa_get_host_name(hn, sizeof(hn)))
            return NULL;
                
        pfx = pa_sprintf_malloc("{%s}", hn);
        if (!pa_startswith(name, pfx))
            /* Not local */
            return NULL;
        
        p = name + strlen(pfx);
    } else
        p = name;
    
    if (*p == '/')
        kind = KIND_UNIX;
    else if (pa_startswith(p, "unix:")) {
        kind = KIND_UNIX;
        p += sizeof("unix:")-1;
    } else if (pa_startswith(p, "tcp:") || pa_startswith(p, "tcp4:")) {
        kind = KIND_TCP4;
        p += sizeof("tcp:")-1;
    } else if (pa_startswith(p, "tcp6:")) {
        kind = KIND_TCP6;
        p += sizeof("tcp6:")-1;
    }

    switch (kind) {
        case KIND_UNIX:
            return pa_socket_client_new_unix(m, p);

        case KIND_TCP_AUTO:  /* Fallthrough */
        case KIND_TCP4: 
        case KIND_TCP6: {
            uint16_t port = default_port;
            char *h;
            struct addrinfo hints, *res;

            if (!(h = parse_address(p, &port)))
                return NULL;

            memset(&hints, 0, sizeof(hints));
            hints.ai_family = kind == KIND_TCP4 ? AF_INET : (kind == KIND_TCP6 ? AF_INET6 : AF_UNSPEC);
            
            if (getaddrinfo(h, NULL, &hints, &res) < 0 || !res || !res->ai_addr)
                return NULL;

            if (res->ai_family == AF_INET) {
                if (res->ai_addrlen != sizeof(struct sockaddr_in))
                    return NULL;
                assert(res->ai_addr->sa_family == res->ai_family);
                
                ((struct sockaddr_in*) res->ai_addr)->sin_port = htons(port);
            } else if (res->ai_family == AF_INET6) {
                if (res->ai_addrlen != sizeof(struct sockaddr_in6))
                    return NULL;
                assert(res->ai_addr->sa_family == res->ai_family);
                
                ((struct sockaddr_in6*) res->ai_addr)->sin6_port = htons(port);
            } else
                return NULL;

            c = pa_socket_client_new_sockaddr(m, res->ai_addr, res->ai_addrlen);
            freeaddrinfo(res);
            return c;
        }
    }

    /* Should never be reached */
    assert(0);
    return NULL;
    
}

/* Return non-zero when the target sockaddr is considered
   local. "local" means UNIX socket or TCP socket on localhost. Other
   local IP addresses are not considered local. */
int pa_socket_client_is_local(struct pa_socket_client *c) {
    assert(c);
    return c->local;
}
