#ifndef foosocketclienthfoo
#define foosocketclienthfoo

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

#include <inttypes.h>
#include <sys/socket.h>

#include "mainloop-api.h"
#include "iochannel.h"

struct pa_socket_client;

struct pa_socket_client* pa_socket_client_new_ipv4(struct pa_mainloop_api *m, uint32_t address, uint16_t port);
struct pa_socket_client* pa_socket_client_new_ipv6(struct pa_mainloop_api *m, uint8_t address[16], uint16_t port);
struct pa_socket_client* pa_socket_client_new_unix(struct pa_mainloop_api *m, const char *filename);
struct pa_socket_client* pa_socket_client_new_sockaddr(struct pa_mainloop_api *m, const struct sockaddr *sa, size_t salen);
struct pa_socket_client* pa_socket_client_new_string(struct pa_mainloop_api *m, const char *a, uint16_t default_port);

void pa_socket_client_unref(struct pa_socket_client *c);
struct pa_socket_client* pa_socket_client_ref(struct pa_socket_client *c);

void pa_socket_client_set_callback(struct pa_socket_client *c, void (*on_connection)(struct pa_socket_client *c, struct pa_iochannel*io, void *userdata), void *userdata);

int pa_socket_client_is_local(struct pa_socket_client *c);

#endif
