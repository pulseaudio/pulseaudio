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

#include "util.h"

void pa_make_nonblock_fd(int fd) {
    int v;

    if ((v = fcntl(fd, F_GETFL)) >= 0)
        if (!(v & O_NONBLOCK))
            fcntl(fd, F_SETFL, v|O_NONBLOCK);
}

void pa_peer_to_string(char *c, size_t l, int fd) {
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

int pa_make_secure_dir(const char* dir) {
    struct stat st;

    if (mkdir(dir, 0700) < 0) 
        if (errno != EEXIST)
            return -1;
    
    if (lstat(dir, &st) < 0) 
        goto fail;
    
    if (!S_ISDIR(st.st_mode) || (st.st_uid != getuid()) || ((st.st_mode & 0777) != 0700))
        goto fail;
    
    return 0;
    
fail:
    rmdir(dir);
    return -1;
}

int pa_make_socket_low_delay(int fd) {
    int ret = 0, buf_size, priority;

    assert(fd >= 0);

    buf_size = 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) < 0)
        ret = -1;

    buf_size = 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) < 0)
        ret = -1;

    priority = 7;
    if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) < 0)
        ret = -1;

    return ret;
}

int pa_make_tcp_socket_low_delay(int fd) {
    int ret, tos, on;
    
    assert(fd >= 0);

    ret = pa_make_socket_low_delay(fd);
    
    on = 1;
    if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0)
        ret = -1;

    tos = IPTOS_LOWDELAY;
    if (setsockopt(fd, SOL_IP, IP_TOS, &tos, sizeof(tos)) < 0)
        ret = -1;

    return ret;

}

ssize_t pa_loop_read(int fd, void*data, size_t size) {
    ssize_t ret = 0;
    assert(fd >= 0 && data && size);

    while (size > 0) {
        ssize_t r;

        if ((r = read(fd, data, size)) < 0)
            return r;

        if (r == 0)
            break;
        
        ret += r;
        data += r;
        size -= r;
    }

    return ret;
}

ssize_t pa_loop_write(int fd, const void*data, size_t size) {
    ssize_t ret = 0;
    assert(fd >= 0 && data && size);

    while (size > 0) {
        ssize_t r;

        if ((r = write(fd, data, size)) < 0)
            return r;

        if (r == 0)
            break;
        
        ret += r;
        data += r;
        size -= r;
    }

    return ret;
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

void pa_check_for_sigpipe(void) {
    struct sigaction sa;

    if (sigaction(SIGPIPE, NULL, &sa) < 0) {
        fprintf(stderr, __FILE__": sigaction() failed: %s\n", strerror(errno));
        return;
    }
        
    if (sa.sa_handler == SIG_DFL)
        fprintf(stderr, "polypaudio: WARNING: SIGPIPE is not trapped. This might cause malfunction!\n");
}
