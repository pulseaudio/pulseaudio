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

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "socket-client.h"
#include "socket-util.h"
#include "util.h"
#include "xmalloc.h"

struct pa_socket_client {
    struct pa_mainloop_api *mainloop;
    int fd;
    struct pa_io_event *io_event;
    struct pa_defer_event *defer_event;
    void (*callback)(struct pa_socket_client*c, struct pa_iochannel *io, void *userdata);
    void *userdata;
};

static struct pa_socket_client*pa_socket_client_new(struct pa_mainloop_api *m) {
    struct pa_socket_client *c;
    assert(m);

    c = pa_xmalloc(sizeof(struct pa_socket_client));
    c->mainloop = m;
    c->fd = -1;
    c->io_event = NULL;
    c->defer_event = NULL;
    c->callback = NULL;
    c->userdata = NULL;
    return c;
}

static void do_call(struct pa_socket_client *c) {
    struct pa_iochannel *io;
    int error, lerror;
    assert(c && c->callback);

    lerror = sizeof(error);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &lerror) < 0) {
        fprintf(stderr, "getsockopt(): %s\n", strerror(errno));
        goto failed;
    }

    if (lerror != sizeof(error)) {
        fprintf(stderr, "getsocktop() returned invalid size.\n");
        goto failed;
    }

    if (error != 0) {
        fprintf(stderr, "connect(): %s\n", strerror(error));
        goto failed;
    }
        
    io = pa_iochannel_new(c->mainloop, c->fd, c->fd);
    assert(io);
    c->fd = -1;
    c->callback(c, io, c->userdata);

    return;
    
failed:
    close(c->fd);
    c->fd = -1;
    c->callback(c, NULL, c->userdata);
    return;
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
            /*fprintf(stderr, "connect(): %s\n", strerror(errno));*/
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
    struct pa_socket_client *c;
    struct sockaddr_in sa;
    assert(m && address && port);

    c = pa_socket_client_new(m);
    assert(c);

    if ((c->fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        goto fail;
    }

    pa_socket_tcp_low_delay(c->fd);

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(address);

    if (do_connect(c, (struct sockaddr*) &sa, sizeof(sa)) < 0)
        goto fail;
    
    return c;

fail:
    pa_socket_client_free(c);
    return NULL;
}

struct pa_socket_client* pa_socket_client_new_unix(struct pa_mainloop_api *m, const char *filename) {
    struct pa_socket_client *c;
    struct sockaddr_un sa;
    assert(m && filename);
    
    c = pa_socket_client_new(m);
    assert(c);

    if ((c->fd = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        goto fail;
    }

    pa_socket_low_delay(c->fd);

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, filename, sizeof(sa.sun_path)-1);
    sa.sun_path[sizeof(sa.sun_path) - 1] = 0;
    
    if (do_connect(c, (struct sockaddr*) &sa, sizeof(sa)) < 0)
        goto fail;
    
    return c;

fail:
    pa_socket_client_free(c);
    return NULL;
}

struct pa_socket_client* pa_socket_client_new_sockaddr(struct pa_mainloop_api *m, const struct sockaddr *sa, size_t salen) {
    struct pa_socket_client *c;
    assert(m && sa);
    c = pa_socket_client_new(m);
    assert(c);

    if ((c->fd = socket(sa->sa_family, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        goto fail;
    }

    if (sa->sa_family == AF_INET)
        pa_socket_tcp_low_delay(c->fd);
    else
        pa_socket_low_delay(c->fd);

    if (do_connect(c, sa, salen) < 0)
        goto fail;
    
    return c;

fail:
    pa_socket_client_free(c);
    return NULL;
    
}

void pa_socket_client_free(struct pa_socket_client *c) {
    assert(c && c->mainloop);
    if (c->io_event)
        c->mainloop->io_free(c->io_event);
    if (c->defer_event)
        c->mainloop->defer_free(c->defer_event);
    if (c->fd >= 0)
        close(c->fd);
    pa_xfree(c);
}

void pa_socket_client_set_callback(struct pa_socket_client *c, void (*on_connection)(struct pa_socket_client *c, struct pa_iochannel*io, void *userdata), void *userdata) {
    assert(c);
    c->callback = on_connection;
    c->userdata = userdata;
}
