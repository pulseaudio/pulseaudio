/***
  This file is part of PulseAudio.

  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>

#ifndef HAVE_INET_NTOP

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "winsock.h"

#include "inet_ntop.h"

const char *inet_ntop(int af, const void *src, char *dst, socklen_t cnt) {
    struct in_addr *in = (struct in_addr*)src;
#ifdef HAVE_IPV6
    struct in6_addr *in6 = (struct in6_addr*)src;
#endif

    assert(src && dst);

    switch (af) {
    case AF_INET:
        pa_snprintf(dst, cnt, "%d.%d.%d.%d",
#ifdef WORDS_BIGENDIAN
            (int)(in->s_addr >> 24) & 0xff,
            (int)(in->s_addr >> 16) & 0xff,
            (int)(in->s_addr >>  8) & 0xff,
            (int)(in->s_addr >>  0) & 0xff);
#else
            (int)(in->s_addr >>  0) & 0xff,
            (int)(in->s_addr >>  8) & 0xff,
            (int)(in->s_addr >> 16) & 0xff,
            (int)(in->s_addr >> 24) & 0xff);
#endif
        break;
#ifdef HAVE_IPV6
    case AF_INET6:
        pa_snprintf(dst, cnt, "%x:%x:%x:%x:%x:%x:%x:%x",
            in6->s6_addr[ 0] << 8 | in6->s6_addr[ 1],
            in6->s6_addr[ 2] << 8 | in6->s6_addr[ 3],
            in6->s6_addr[ 4] << 8 | in6->s6_addr[ 5],
            in6->s6_addr[ 6] << 8 | in6->s6_addr[ 7],
            in6->s6_addr[ 8] << 8 | in6->s6_addr[ 9],
            in6->s6_addr[10] << 8 | in6->s6_addr[11],
            in6->s6_addr[12] << 8 | in6->s6_addr[13],
            in6->s6_addr[14] << 8 | in6->s6_addr[15]);
        break;
#endif
    default:
        errno = EAFNOSUPPORT;
        return NULL;
    }

    return dst;
}

#endif /* INET_NTOP */
