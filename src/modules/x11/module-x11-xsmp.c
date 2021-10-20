/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/SM/SMlib.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/x11wrap.h>

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("X11 session management");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE("session_manager=<session manager string> display=<X11 display>");

static bool ice_in_use = false;

static const char* const valid_modargs[] = {
    "session_manager",
    "display",
    "xauthority",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_client *client;
    SmcConn connection;

    pa_x11_wrapper *x11_wrapper;
    pa_x11_client *x11_client;
};

typedef struct {
    IceConn connection;
    struct userdata *userdata;
} ice_io_callback_data;

static void* ice_io_cb_data_new(IceConn connection, struct userdata *userdata) {
    ice_io_callback_data *data = pa_xnew(ice_io_callback_data, 1);

    data->connection = connection;
    data->userdata = userdata;

    return data;
}

static void ice_io_cb_data_destroy(pa_mainloop_api*a, pa_io_event *e, void *userdata) {
    pa_assert(userdata);

    pa_xfree(userdata);
}

static void x11_kill_cb(pa_x11_wrapper *w, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(w);
    pa_assert(u);
    pa_assert(u->x11_wrapper == w);

    pa_log_debug("X11 client kill callback called");

    if (u->connection) {
        SmcCloseConnection(u->connection, 0, NULL);
        u->connection = NULL;
    }

    if (u->x11_client) {
        pa_x11_client_free(u->x11_client);
        u->x11_client = NULL;
    }

    if (u->x11_wrapper) {
        pa_x11_wrapper_unref(u->x11_wrapper);
        u->x11_wrapper = NULL;
    }

    pa_module_unload_request(u->module, true);
}

static void close_xsmp_connection(struct userdata *userdata) {
    pa_assert(userdata);

    if (userdata->connection) {
        SmcCloseConnection(userdata->connection, 0, NULL);
        userdata->connection = NULL;
    }

    pa_x11_wrapper_kill_deferred(userdata->x11_wrapper);
}

static void die_cb(SmcConn connection, SmPointer client_data) {
    struct userdata *u = client_data;

    pa_assert(u);

    pa_log_debug("Got die message from XSMP.");

    close_xsmp_connection(u);
}

static void save_complete_cb(SmcConn connection, SmPointer client_data) {
}

static void shutdown_cancelled_cb(SmcConn connection, SmPointer client_data) {
    SmcSaveYourselfDone(connection, True);
}

static void save_yourself_cb(SmcConn connection, SmPointer client_data, int save_type, Bool _shutdown, int interact_style, Bool fast) {
    SmcSaveYourselfDone(connection, True);
}

static void ice_io_cb(pa_mainloop_api*a, pa_io_event *e, int fd, pa_io_event_flags_t flags, void *userdata) {
    ice_io_callback_data *io_data = userdata;

    pa_assert(io_data);

    if (IceProcessMessages(io_data->connection, NULL, NULL) == IceProcessMessagesIOError) {
        pa_log_debug("IceProcessMessages: I/O error, closing XSMP.");

        IceSetShutdownNegotiation(io_data->connection, False);

        /* SM owns this connection, close via SmcCloseConnection() */
        close_xsmp_connection(io_data->userdata);
    }
}

static void new_ice_connection(IceConn connection, IcePointer client_data, Bool opening, IcePointer *watch_data) {
    struct userdata *u = client_data;

    pa_assert(u);

    if (opening) {
        *watch_data = u->core->mainloop->io_new(
                u->core->mainloop,
                IceConnectionNumber(connection),
                PA_IO_EVENT_INPUT,
                ice_io_cb,
                ice_io_cb_data_new(connection, u));

        u->core->mainloop->io_set_destroy(*watch_data, ice_io_cb_data_destroy);
    } else
        u->core->mainloop->io_free(*watch_data);
}

static IceIOErrorHandler ice_installed_handler;

/* We call any handler installed before (or after) module is loaded but
   avoid calling the default libICE handler which does an exit() */

static void ice_io_error_handler(IceConn iceConn) {
    pa_log_warn("ICE I/O error handler called");
    if (ice_installed_handler)
      (*ice_installed_handler) (iceConn);
}

int pa__init(pa_module*m) {

    pa_modargs *ma = NULL;
    char t[256], *vendor, *client_id;
    SmcCallbacks callbacks;
    SmProp prop_program, prop_user;
    SmProp *prop_list[2];
    SmPropValue val_program, val_user;
    struct userdata *u;
    const char *e;
    pa_client_new_data data;

    pa_assert(m);

    if (ice_in_use) {
        pa_log("module-x11-xsmp may not be loaded twice.");
        return -1;
    } else {
        IceIOErrorHandler default_handler;

        ice_installed_handler = IceSetIOErrorHandler (NULL);
        default_handler = IceSetIOErrorHandler (ice_io_error_handler);

        if (ice_installed_handler == default_handler)
            ice_installed_handler = NULL;

        IceSetIOErrorHandler(ice_io_error_handler);
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->client = NULL;
    u->connection = NULL;
    u->x11_wrapper = NULL;

    IceAddConnectionWatch(new_ice_connection, u);
    ice_in_use = true;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value(ma, "xauthority", NULL)) {
        if (setenv("XAUTHORITY", pa_modargs_get_value(ma, "xauthority", NULL), 1)) {
            pa_log("setenv() for $XAUTHORITY failed");
            goto fail;
        }
    }

    if (!(u->x11_wrapper = pa_x11_wrapper_get(m->core, pa_modargs_get_value(ma, "display", NULL))))
        goto fail;

    u->x11_client = pa_x11_client_new(u->x11_wrapper, NULL, x11_kill_cb, u);

    e = pa_modargs_get_value(ma, "session_manager", NULL);

    if (!e && !getenv("SESSION_MANAGER")) {
        pa_log("X11 session manager not running.");
        goto fail;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.die.callback = die_cb;
    callbacks.die.client_data = u;
    callbacks.save_yourself.callback = save_yourself_cb;
    callbacks.save_yourself.client_data = m->core;
    callbacks.save_complete.callback = save_complete_cb;
    callbacks.save_complete.client_data = m->core;
    callbacks.shutdown_cancelled.callback = shutdown_cancelled_cb;
    callbacks.shutdown_cancelled.client_data = m->core;

    if (!(u->connection = SmcOpenConnection(
                  (char*) e, m->core,
                  SmProtoMajor, SmProtoMinor,
                  SmcSaveYourselfProcMask | SmcDieProcMask | SmcSaveCompleteProcMask | SmcShutdownCancelledProcMask,
                  &callbacks, NULL, &client_id,
                  sizeof(t), t))) {

        pa_log("Failed to open connection to session manager: %s", t);
        goto fail;
    }

    prop_program.name = (char*) SmProgram;
    prop_program.type = (char*) SmARRAY8;
    val_program.value = (char*) PACKAGE_NAME;
    val_program.length = (int) strlen(val_program.value);
    prop_program.num_vals = 1;
    prop_program.vals = &val_program;
    prop_list[0] = &prop_program;

    prop_user.name = (char*) SmUserID;
    prop_user.type = (char*) SmARRAY8;
    pa_get_user_name(t, sizeof(t));
    val_user.value = t;
    val_user.length = (int) strlen(val_user.value);
    prop_user.num_vals = 1;
    prop_user.vals = &val_user;
    prop_list[1] = &prop_user;

    SmcSetProperties(u->connection, PA_ELEMENTSOF(prop_list), prop_list);

    pa_log_info("Connected to session manager '%s' as '%s'.", vendor = SmcVendor(u->connection), client_id);

    pa_client_new_data_init(&data);
    data.module = m;
    data.driver = __FILE__;
    pa_proplist_setf(data.proplist, PA_PROP_APPLICATION_NAME, "XSMP Session on %s as %s", vendor, client_id);
    pa_proplist_sets(data.proplist, "xsmp.vendor", vendor);
    pa_proplist_sets(data.proplist, "xsmp.client.id", client_id);
    u->client = pa_client_new(u->core, &data);
    pa_client_new_data_done(&data);

    free(vendor);
    free(client_id);

    if (!u->client)
        goto fail;

    /* Positive exit_idle_time is only useful when we have no session tracking
     * capability, so we can set it to 0 now that we have detected a session.
     * The benefit of setting exit_idle_time to 0 is that pulseaudio will exit
     * immediately when the session ends. That in turn is useful, because some
     * systems (those that use pam_systemd but don't use systemd for managing
     * pulseaudio) clean $XDG_RUNTIME_DIR on logout, but fail to terminate all
     * services that depend on the files in $XDG_RUNTIME_DIR. The directory
     * contains our sockets, and if the sockets are removed without terminating
     * pulseaudio, a quick relogin will likely cause trouble, because a new
     * instance will be spawned while the old instance is still running. */
    if (u->core->exit_idle_time > 0)
        pa_core_set_exit_idle_time(u->core, 0);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    /* set original ICE I/O error handler and forget it */
    IceSetIOErrorHandler(ice_installed_handler);
    ice_installed_handler = NULL;

    if ((u = m->userdata)) {

        if (u->connection)
            SmcCloseConnection(u->connection, 0, NULL);

        if (u->client)
            pa_client_free(u->client);

        if (u->x11_client)
            pa_x11_client_free(u->x11_client);

        if (u->x11_wrapper)
            pa_x11_wrapper_unref(u->x11_wrapper);
    }

    if (ice_in_use) {
        IceRemoveConnectionWatch(new_ice_connection, u);
        ice_in_use = false;
    }

    if (u)
        pa_xfree(u);
}
