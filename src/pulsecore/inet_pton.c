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

#ifndef HAVE_INET_PTON

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "winsock.h"

#include "inet_pton.h"

int inet_pton(int af, const char *src, void *dst) {
    struct in_addr *in = (struct in_addr*)dst;
#ifdef HAVE_IPV6
    struct in6_addr *in6 = (struct in6_addr*)dst;
#endif

    assert(src && dst);

    switch (af) {
    case AF_INET:
        in->s_addr = inet_addr(src);
        if (in->s_addr == INADDR_NONE)
            return 0;
        break;
#ifdef HAVE_IPV6
    case AF_INET6:
        /* FIXME */
#endif
    default:
        errno = EAFNOSUPPORT;
        return -1;
    }

    return 1;
}

#endif /* INET_PTON */
