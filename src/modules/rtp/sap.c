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

#include <assert.h>
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <polypcore/util.h>
#include <polypcore/log.h>
#include <polypcore/xmalloc.h>

#include "sap.h"

pa_sap_context* pa_sap_context_init_send(pa_sap_context *c, int fd, char *sdp_data) {
    assert(c);
    assert(fd >= 0);
    assert(sdp_data);

    c->fd = fd;
    c->sdp_data = sdp_data;
    c->msg_id_hash = (uint16_t) (rand()*rand());
    
    return c;    
}

void pa_sap_context_destroy(pa_sap_context *c) {
    assert(c);

    close(c->fd);
    pa_xfree(c->sdp_data);
}

int pa_sap_send(pa_sap_context *c, int goodbye) {
    uint32_t header;
    const char mime[] = "application/sdp";
    struct sockaddr_storage sa_buf;
    struct sockaddr *sa = (struct sockaddr*) &sa_buf;
    socklen_t salen = sizeof(sa_buf);
    struct iovec iov[4];
    struct msghdr m;
    int k;

    if (getsockname(c->fd, sa, &salen) < 0) {
        pa_log("getsockname() failed: %s\n", strerror(errno));
        return -1;
    }

    assert(sa->sa_family == AF_INET || sa->sa_family == AF_INET6);
    
    header = htonl(((uint32_t) 1 << 29) |
                   (sa->sa_family == AF_INET6 ? (uint32_t) 1 << 28 : 0) |
                   (goodbye ? (uint32_t) 1 << 26 : 0) |
                   (c->msg_id_hash));

    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);

    iov[1].iov_base = sa->sa_family == AF_INET ? (void*) &((struct sockaddr_in*) sa)->sin_addr : (void*) &((struct sockaddr_in6*) sa)->sin6_addr;
    iov[1].iov_len = sa->sa_family == AF_INET ? 4 : 16;

    iov[2].iov_base = (char*) mime;
    iov[2].iov_len = sizeof(mime);

    iov[3].iov_base = c->sdp_data;
    iov[3].iov_len = strlen(c->sdp_data);
                   
    m.msg_name = NULL;
    m.msg_namelen = 0;
    m.msg_iov = iov;
    m.msg_iovlen = 4;
    m.msg_control = NULL;
    m.msg_controllen = 0;
    m.msg_flags = 0;
    
    if ((k = sendmsg(c->fd, &m, MSG_DONTWAIT)) < 0)
        pa_log("sendmsg() failed: %s\n", strerror(errno));

    return k;
}
