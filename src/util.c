#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "util.h"

void make_nonblock_fd(int fd) {
    int v;

    if ((v = fcntl(fd, F_GETFL)) >= 0)
        if (!(v & O_NONBLOCK))
            fcntl(fd, F_SETFL, v|O_NONBLOCK);
}

void peer_to_string(char *c, size_t l, int fd) {
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
                snprintf(c, l, "UNIX client for %s", sa.un.sun_path);
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

int make_secure_dir(const char* dir) {
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
