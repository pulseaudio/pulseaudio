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
#include "util.h"

struct socket_client {
    struct pa_mainloop_api *mainloop;
    int fd;

    void *io_source, *fixed_source;
    void (*callback)(struct socket_client*c, struct iochannel *io, void *userdata);
    void *userdata;
};

static struct socket_client*socket_client_new(struct pa_mainloop_api *m) {
    struct socket_client *c;
    assert(m);

    c = malloc(sizeof(struct socket_client));
    assert(c);
    c->mainloop = m;
    c->fd = -1;
    c->io_source = c->fixed_source = NULL;
    c->callback = NULL;
    c->userdata = NULL;
    return c;
}

static void do_call(struct socket_client *c) {
    struct iochannel *io;
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
        
    io = iochannel_new(c->mainloop, c->fd, c->fd);
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

static void connect_fixed_cb(struct pa_mainloop_api *m, void *id, void *userdata) {
    struct socket_client *c = userdata;
    assert(m && c && c->fixed_source == id);
    m->cancel_fixed(m, c->fixed_source);
    c->fixed_source = NULL;
    do_call(c);
}

static void connect_io_cb(struct pa_mainloop_api*m, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
    struct socket_client *c = userdata;
    assert(m && c && c->io_source == id && fd >= 0 && events == PA_MAINLOOP_API_IO_EVENT_OUTPUT);
    m->cancel_io(m, c->io_source);
    c->io_source = NULL;
    do_call(c);
}

static int do_connect(struct socket_client *c, const struct sockaddr *sa, socklen_t len) {
    int r;
    assert(c && sa && len);
    
    make_nonblock_fd(c->fd);
    
    if ((r = connect(c->fd, sa, len)) < 0) {
        if (r != EINPROGRESS) {
            fprintf(stderr, "connect(): %s\n", strerror(errno));
            return -1;
        }

        c->io_source = c->mainloop->source_io(c->mainloop, c->fd, PA_MAINLOOP_API_IO_EVENT_OUTPUT, connect_io_cb, c);
        assert(c->io_source);
    } else {
        c->fixed_source = c->mainloop->source_fixed(c->mainloop, connect_fixed_cb, c);
        assert(c->fixed_source);
    }

    return 0;
}

struct socket_client* socket_client_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port) {
    struct socket_client *c;
    struct sockaddr_in sa;
    assert(m && address && port);

    c = socket_client_new(m);
    assert(c);

    if ((c->fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        goto fail;
    }

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(address);

    if (do_connect(c, (struct sockaddr*) &sa, sizeof(sa)) < 0)
        goto fail;
    
    return c;

fail:
    socket_client_free(c);
    return NULL;
}

struct socket_client* socket_client_new_unix(struct pa_mainloop_api *m, const char *filename) {
    struct socket_client *c;
    struct sockaddr_un sa;
    assert(m && filename);
    
    c = socket_client_new(m);
    assert(c);

    if ((c->fd = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        goto fail;
    }

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, filename, sizeof(sa.sun_path)-1);
    sa.sun_path[sizeof(sa.sun_path) - 1] = 0;
    
    if (do_connect(c, (struct sockaddr*) &sa, sizeof(sa)) < 0)
        goto fail;
    
    return c;

fail:
    socket_client_free(c);
    return NULL;
}
    
void socket_client_free(struct socket_client *c) {
    assert(c && c->mainloop);
    if (c->io_source)
        c->mainloop->cancel_io(c->mainloop, c->io_source);
    if (c->fixed_source)
        c->mainloop->cancel_fixed(c->mainloop, c->fixed_source);
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

void socket_client_set_callback(struct socket_client *c, void (*on_connection)(struct socket_client *c, struct iochannel*io, void *userdata), void *userdata) {
    assert(c);
    c->callback = on_connection;
    c->userdata = userdata;
}
