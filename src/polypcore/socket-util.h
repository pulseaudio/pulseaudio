#ifndef foosocketutilhfoo
#define foosocketutilhfoo

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

#include <sys/types.h>

void pa_socket_peer_to_string(int fd, char *c, size_t l);

int pa_socket_low_delay(int fd);
int pa_socket_tcp_low_delay(int fd);

int pa_socket_set_sndbuf(int fd, size_t l);
int pa_socket_set_rcvbuf(int fd, size_t l);

int pa_unix_socket_is_stale(const char *fn);
int pa_unix_socket_remove_stale(const char *fn);

#endif
