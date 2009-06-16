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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dbus/dbus.h>

#include <pulsecore/core-util.h>

#include "dbus-common.h"

char *pa_get_dbus_address_from_server_type(pa_server_type_t server_type) {
    char *address = NULL;
    char *runtime_path = NULL;
    char *escaped_path = NULL;

    switch (server_type) {
        case PA_SERVER_TYPE_USER:
            if (!(runtime_path = pa_runtime_path(PA_DBUS_SOCKET_NAME))) {
                pa_log("pa_runtime_path() failed.");
                break;
            }

            if (!(escaped_path = dbus_address_escape_value(runtime_path))) {
                pa_log("dbus_address_escape_value() failed.");
                break;
            }

            address = pa_sprintf_malloc("unix:path=%s", escaped_path);
            break;

        case PA_SERVER_TYPE_SYSTEM:
            if (!(escaped_path = dbus_address_escape_value(PA_DBUS_SYSTEM_SOCKET_PATH))) {
                pa_log("dbus_address_escape_value() failed.");
                break;
            }

            address = pa_sprintf_malloc("unix:path=%s", escaped_path);
            break;

        case PA_SERVER_TYPE_NONE:
            address = pa_xnew0(char, 1);
            break;

        default:
            pa_assert_not_reached();
    }

    pa_xfree(runtime_path);
    pa_xfree(escaped_path);

    return address;
}
