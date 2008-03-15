/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>

#include <dbus/dbus.h>
#include <polkit-dbus/polkit-dbus.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "polkit.h"

static pa_bool_t show_grant_dialog(const char *action_id) {
    DBusError dbus_error;
    DBusConnection *bus = NULL;
    DBusMessage *m = NULL, *reply = NULL;
    pa_bool_t r = FALSE;
    uint32_t xid = 0;
    int verdict;

    dbus_error_init(&dbus_error);

    if (!(bus = dbus_bus_get(DBUS_BUS_SESSION, &dbus_error))) {
        pa_log_error("Cannot connect to session bus: %s", dbus_error.message);
        goto finish;
    }

    if (!(m = dbus_message_new_method_call("org.gnome.PolicyKit", "/org/gnome/PolicyKit/Manager", "org.gnome.PolicyKit.Manager", "ShowDialog"))) {
        pa_log_error("Failed to allocate D-Bus message.");
        goto finish;
    }

    if (!(dbus_message_append_args(m, DBUS_TYPE_STRING, &action_id, DBUS_TYPE_UINT32, &xid, DBUS_TYPE_INVALID))) {
        pa_log_error("Failed to append arguments to D-Bus message.");
        goto finish;
    }

    if (!(reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &dbus_error))) {
        pa_log_warn("Failed to show grant dialog: %s", dbus_error.message);
        goto finish;
    }

    if (!(dbus_message_get_args(reply, &dbus_error, DBUS_TYPE_BOOLEAN, &verdict, DBUS_TYPE_INVALID))) {
        pa_log_warn("Malformed response from grant manager: %s", dbus_error.message);
        goto finish;
    }

    r = !!verdict;

finish:

    if (bus)
        dbus_connection_unref(bus);

    dbus_error_free(&dbus_error);

    if (m)
        dbus_message_unref(m);

    if (reply)
        dbus_message_unref(reply);

    return r;
}

int pa_polkit_check(const char *action_id) {
    int ret = -1;
    DBusError dbus_error;
    DBusConnection *bus = NULL;
    PolKitCaller *caller = NULL;
    PolKitAction *action = NULL;
    PolKitContext *context = NULL;
    PolKitError *polkit_error = NULL;
    PolKitSession *session = NULL;
    PolKitResult polkit_result;

    dbus_error_init(&dbus_error);

    if (!(bus = dbus_bus_get(DBUS_BUS_SYSTEM, &dbus_error))) {
        pa_log_error("Cannot connect to system bus: %s", dbus_error.message);
        goto finish;
    }

    if (!(caller = polkit_caller_new_from_pid(bus, getpid(), &dbus_error))) {
        pa_log_error("Cannot get caller from PID: %s", dbus_error.message);
        goto finish;
    }

    /* This function is called when PulseAudio is called SUID root. We
     * want to authenticate the real user that called us and not the
     * effective user we gained through being SUID root. Hence we
     * overwrite the UID caller data here explicitly, just for
     * paranoia. In fact PolicyKit should fill in the UID here anyway
     * -- an not the EUID or any other user id. */

    if (!(polkit_caller_set_uid(caller, getuid()))) {
        pa_log_error("Cannot set UID on caller object.");
        goto finish;
    }

    if (!(polkit_caller_get_ck_session(caller, &session))) {
        pa_log_error("Failed to get CK session.");
        goto finish;
    }

    /* We need to overwrite the UID in both the caller and the session
     * object */

    if (!(polkit_session_set_uid(session, getuid()))) {
        pa_log_error("Cannot set UID on session object.");
        goto finish;
    }

    if (!(action = polkit_action_new())) {
        pa_log_error("Cannot allocate PolKitAction.");
        goto finish;
    }

    if (!polkit_action_set_action_id(action, action_id)) {
        pa_log_error("Cannot set action_id");
        goto finish;
    }

    if (!(context = polkit_context_new())) {
        pa_log_error("Cannot allocate PolKitContext.");
        goto finish;
    }

    if (!polkit_context_init(context, &polkit_error)) {
        pa_log_error("Cannot initialize PolKitContext: %s", polkit_error_get_error_message(polkit_error));
        goto finish;
    }

    for (;;) {

#ifdef HAVE_POLKIT_CONTEXT_IS_CALLER_AUTHORIZED
        polkit_result = polkit_context_is_caller_authorized(context, action, caller, TRUE, &polkit_error);

        if (polkit_error_is_set(polkit_error)) {
            pa_log_error("Could not determine whether caller is authorized: %s", polkit_error_get_error_message(polkit_error));
            goto finish;
        }
#else

        polkit_result = polkit_context_can_caller_do_action(context, action, caller);

#endif

        if (polkit_result == POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH ||
            polkit_result == POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION ||
            polkit_result == POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS ||
#ifdef POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT
            polkit_result == POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT ||
#endif
            polkit_result == POLKIT_RESULT_ONLY_VIA_SELF_AUTH ||
            polkit_result == POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION ||
            polkit_result == POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS
#ifdef POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT
            || polkit_result == POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT
#endif
        ) {

            if (show_grant_dialog(action_id))
                continue;
        }

        break;
    }

    if (polkit_result != POLKIT_RESULT_YES && polkit_result != POLKIT_RESULT_NO)
        pa_log_warn("PolicyKit responded with '%s'", polkit_result_to_string_representation(polkit_result));

    ret = polkit_result == POLKIT_RESULT_YES;

finish:

    if (caller)
        polkit_caller_unref(caller);

    if (action)
        polkit_action_unref(action);

    if (context)
        polkit_context_unref(context);

    if (bus)
        dbus_connection_unref(bus);

    dbus_error_free(&dbus_error);

    if (polkit_error)
        polkit_error_free(polkit_error);

    return ret;
}
