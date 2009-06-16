#ifndef foodbuscommonhfoo
#define foodbuscommonhfoo

/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulsecore/core.h>
#include <pulsecore/macro.h>

#define PA_DBUS_DEFAULT_PORT 24883
#define PA_DBUS_SOCKET_NAME "dbus_socket"

#define PA_DBUS_SYSTEM_SOCKET_PATH PA_SYSTEM_RUNTIME_PATH PA_PATH_SEP PA_DBUS_SOCKET_NAME

/* Returns the default address of the server type in the escaped form. For
 * PA_SERVER_TYPE_NONE an empty string is returned. The caller frees the
 * string. This function may fail in some rare cases, in which case NULL is
 * returned. */
char *pa_get_dbus_address_from_server_type(pa_server_type_t server_type);

#endif
