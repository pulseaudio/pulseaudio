/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/SM/SMlib.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/iochannel.h>
#include <pulsecore/sink.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>

#include "module-x11-xsmp-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("X11 session management")
PA_MODULE_VERSION(PACKAGE_VERSION)

struct userdata {
    pa_core *core;
    SmcConn sm_conn;
};

static const char* const valid_modargs[] = {
    NULL
};

static void die_cb(SmcConn connection, SmPointer client_data){
    pa_core *c = client_data;

    pa_log_debug("Got die message from XSM. Exiting...");
    
    pa_core_assert_ref(c);
    c->mainloop->quit(c->mainloop, 0);
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
    IceConn connection = userdata;

    if (IceProcessMessages(connection, NULL, NULL) == IceProcessMessagesIOError) {
        IceSetShutdownNegotiation(connection, False);
        IceCloseConnection(connection);
    }
}

static void new_ice_connection(IceConn connection, IcePointer client_data, Bool opening, IcePointer *watch_data) {
    pa_core *c = client_data;

    pa_assert(c);
    
    if (opening)
        *watch_data = c->mainloop->io_new(c->mainloop, IceConnectionNumber(connection), PA_IO_EVENT_INPUT, ice_io_cb, connection);
    else
        c->mainloop->io_free(*watch_data);
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    char t[256], *vendor, *client_id;
    SmcCallbacks callbacks;
    SmProp prop_program, prop_user;
    SmProp *prop_list[2];
    SmPropValue val_program, val_user;
    
    pa_assert(c);
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (!getenv("SESSION_MANAGER")) {
        pa_log("X11 session manager not running.");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = c;
    u->sm_conn = NULL;

    IceAddConnectionWatch(new_ice_connection, c);
    
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.die.callback = die_cb;
    callbacks.die.client_data = c;

    callbacks.save_yourself.callback = save_yourself_cb;
    callbacks.save_complete.callback = save_complete_cb;
    callbacks.shutdown_cancelled.callback = shutdown_cancelled_cb;
    
    if (!(u->sm_conn = SmcOpenConnection(
                  NULL, u,
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
    val_program.length = strlen(val_program.value);
    prop_program.num_vals = 1;
    prop_program.vals = &val_program;
    prop_list[0] = &prop_program;

    prop_user.name = (char*) SmUserID;
    prop_user.type = (char*) SmARRAY8;
    pa_get_user_name(t, sizeof(t));
    val_user.value = t;
    val_user.length = strlen(val_user.value);
    prop_user.num_vals = 1;
    prop_user.vals = &val_user;
    prop_list[1] = &prop_user;

    SmcSetProperties(u->sm_conn, PA_ELEMENTSOF(prop_list), prop_list);

    pa_log_info("Connected to session manager '%s' as '%s'.", vendor = SmcVendor(u->sm_conn), client_id);
    free(vendor);
    free(client_id);
    
    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
    
    pa__done(c, m);
    
    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    
    assert(c);
    assert(m);

    if (!m->userdata)
        return;

    u = m->userdata;

    if (u->sm_conn)
        SmcCloseConnection(u->sm_conn, 0, NULL);

    IceRemoveConnectionWatch(new_ice_connection, c);
    
    pa_xfree(u);
}
