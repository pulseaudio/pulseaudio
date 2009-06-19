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

#include <dbus/dbus.h>

#include <pulsecore/core.h>
#include <pulsecore/macro.h>

#define PA_DBUS_DEFAULT_PORT 24883
#define PA_DBUS_SOCKET_NAME "dbus_socket"

#define PA_DBUS_SYSTEM_SOCKET_PATH PA_SYSTEM_RUNTIME_PATH PA_PATH_SEP PA_DBUS_SOCKET_NAME

/* NOTE: These functions may only be called from the main thread. */

/* Returns the default address of the server type in the escaped form. For
 * PA_SERVER_TYPE_NONE an empty string is returned. The caller frees the
 * string. This function may fail in some rare cases, in which case NULL is
 * returned. */
char *pa_get_dbus_address_from_server_type(pa_server_type_t server_type);

/* Registers the given interface to the given object path. This is additive: it
 * doesn't matter whether or not the object has already been registered; if it
 * is, then its interface set is just extended.
 *
 * Introspection requests are handled automatically. For that to work, the
 * caller gives an XML snippet containing the interface introspection element.
 * All interface snippets are automatically combined to provide the final
 * introspection string.
 *
 * The introspection snippet contains the interface name and the methods, but
 * since this function doesn't do XML parsing, the interface name and the set
 * of method names have to be supplied separately. If the interface doesn't
 * contain any methods, NULL may be given as the methods parameter, otherwise
 * the methods parameter must be a NULL-terminated array of strings.
 *
 * Fails and returns a negative number if the object already has the interface
 * registered. */
int pa_dbus_add_interface(pa_core *c, const char* path, const char* interface, const char * const *methods, const char* introspection_snippet, DBusObjectPathMessageFunction receive_cb, void *userdata);

/* Returns a negative number if the given object doesn't have the given
 * interface registered. */
int pa_dbus_remove_interface(pa_core *c, const char* path, const char* interface);

/* Fails and returns a negative number if the connection is already
 * registered. */
int pa_dbus_register_connection(pa_core *c, DBusConnection *conn);

/* Returns a negative number if the connection wasn't registered. */
int pa_dbus_unregister_connection(pa_core *c, DBusConnection *conn);

#endif
