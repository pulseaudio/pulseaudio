/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2006 Shams E. King

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

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulsecore/log.h>
#include <pulsecore/shared.h>

#include "dbus-util.h"

struct pa_dbus_connection {
    PA_REFCNT_DECLARE;

    pa_core *core;
    DBusConnection *connection;
    const char *property_name;
    pa_defer_event* dispatch_event;
};

static void dispatch_cb(pa_mainloop_api *ea, pa_defer_event *ev, void *userdata) {
    DBusConnection *conn = userdata;

    if (dbus_connection_dispatch(conn) == DBUS_DISPATCH_COMPLETE) {
        /* no more data to process, disable the deferred */
        ea->defer_enable(ev, 0);
    }
}

/* DBusDispatchStatusFunction callback for the pa mainloop */
static void dispatch_status(DBusConnection *conn, DBusDispatchStatus status, void *userdata) {
    pa_dbus_connection *c = userdata;

    pa_assert(c);

    switch(status) {

        case DBUS_DISPATCH_COMPLETE:
            c->core->mainloop->defer_enable(c->dispatch_event, 0);
            break;

        case DBUS_DISPATCH_DATA_REMAINS:
        case DBUS_DISPATCH_NEED_MEMORY:
        default:
            c->core->mainloop->defer_enable(c->dispatch_event, 1);
            break;
    }
}

static pa_io_event_flags_t get_watch_flags(DBusWatch *watch) {
    unsigned int flags;
    pa_io_event_flags_t events = 0;

    pa_assert(watch);

    flags = dbus_watch_get_flags(watch);

    /* no watch flags for disabled watches */
    if (!dbus_watch_get_enabled(watch))
        return PA_IO_EVENT_NULL;

    if (flags & DBUS_WATCH_READABLE)
        events |= PA_IO_EVENT_INPUT;
    if (flags & DBUS_WATCH_WRITABLE)
        events |= PA_IO_EVENT_OUTPUT;

    return events | PA_IO_EVENT_HANGUP | PA_IO_EVENT_ERROR;
}

/* pa_io_event_cb_t IO event handler */
static void handle_io_event(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    unsigned int flags = 0;
    DBusWatch *watch = userdata;

#if HAVE_DBUS_WATCH_GET_UNIX_FD
    pa_assert(fd == dbus_watch_get_unix_fd(watch));
#else
    pa_assert(fd == dbus_watch_get_fd(watch));
#endif

    if (!dbus_watch_get_enabled(watch)) {
        pa_log_warn("Asked to handle disabled watch: %p %i", (void*) watch, fd);
        return;
    }

    if (events & PA_IO_EVENT_INPUT)
        flags |= DBUS_WATCH_READABLE;
    if (events & PA_IO_EVENT_OUTPUT)
        flags |= DBUS_WATCH_WRITABLE;
    if (events & PA_IO_EVENT_HANGUP)
        flags |= DBUS_WATCH_HANGUP;
    if (events & PA_IO_EVENT_ERROR)
        flags |= DBUS_WATCH_ERROR;

    dbus_watch_handle(watch, flags);
}

/* pa_time_event_cb_t timer event handler */
static void handle_time_event(pa_mainloop_api *ea, pa_time_event* e, const struct timeval *tv, void *userdata) {
    DBusTimeout *timeout = userdata;

    if (dbus_timeout_get_enabled(timeout)) {
        struct timeval next = *tv;
        dbus_timeout_handle(timeout);

        /* restart it for the next scheduled time */
        pa_timeval_add(&next, (pa_usec_t) dbus_timeout_get_interval(timeout) * 1000);
        ea->time_restart(e, &next);
    }
}

/* DBusAddWatchFunction callback for pa mainloop */
static dbus_bool_t add_watch(DBusWatch *watch, void *data) {
    pa_core *c = PA_CORE(data);
    pa_io_event *ev;

    pa_assert(watch);
    pa_assert(c);

    ev = c->mainloop->io_new(
            c->mainloop,
#if HAVE_DBUS_WATCH_GET_UNIX_FD
            dbus_watch_get_unix_fd(watch),
#else
            dbus_watch_get_fd(watch),
#endif
            get_watch_flags(watch), handle_io_event, watch);

    dbus_watch_set_data(watch, ev, NULL);

    return TRUE;
}

/* DBusRemoveWatchFunction callback for pa mainloop */
static void remove_watch(DBusWatch *watch, void *data) {
    pa_core *c = PA_CORE(data);
    pa_io_event *ev;

    pa_assert(watch);
    pa_assert(c);

    if ((ev = dbus_watch_get_data(watch)))
        c->mainloop->io_free(ev);
}

/* DBusWatchToggledFunction callback for pa mainloop */
static void toggle_watch(DBusWatch *watch, void *data) {
    pa_core *c = PA_CORE(data);
    pa_io_event *ev;

    pa_assert(watch);
    pa_core_assert_ref(c);

    pa_assert_se(ev = dbus_watch_get_data(watch));

    /* get_watch_flags() checks if the watch is enabled */
    c->mainloop->io_enable(ev, get_watch_flags(watch));
}

/* DBusAddTimeoutFunction callback for pa mainloop */
static dbus_bool_t add_timeout(DBusTimeout *timeout, void *data) {
    pa_core *c = PA_CORE(data);
    pa_time_event *ev;
    struct timeval tv;

    pa_assert(timeout);
    pa_assert(c);

    if (!dbus_timeout_get_enabled(timeout))
        return FALSE;

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, (pa_usec_t) dbus_timeout_get_interval(timeout) * 1000);

    ev = c->mainloop->time_new(c->mainloop, &tv, handle_time_event, timeout);

    dbus_timeout_set_data(timeout, ev, NULL);

    return TRUE;
}

/* DBusRemoveTimeoutFunction callback for pa mainloop */
static void remove_timeout(DBusTimeout *timeout, void *data) {
    pa_core *c = PA_CORE(data);
    pa_time_event *ev;

    pa_assert(timeout);
    pa_assert(c);

    if ((ev = dbus_timeout_get_data(timeout)))
        c->mainloop->time_free(ev);
}

/* DBusTimeoutToggledFunction callback for pa mainloop */
static void toggle_timeout(DBusTimeout *timeout, void *data) {
    pa_core *c = PA_CORE(data);
    pa_time_event *ev;

    pa_assert(timeout);
    pa_assert(c);

    pa_assert_se(ev = dbus_timeout_get_data(timeout));

    if (dbus_timeout_get_enabled(timeout)) {
        struct timeval tv;

        pa_gettimeofday(&tv);
        pa_timeval_add(&tv, (pa_usec_t) dbus_timeout_get_interval(timeout) * 1000);

        c->mainloop->time_restart(ev, &tv);
    } else
        c->mainloop->time_restart(ev, NULL);
}

static void wakeup_main(void *userdata) {
    pa_dbus_connection *c = userdata;

    pa_assert(c);

    /* this will wakeup the mainloop and dispatch events, although
     * it may not be the cleanest way of accomplishing it */
    c->core->mainloop->defer_enable(c->dispatch_event, 1);
}

static pa_dbus_connection* pa_dbus_connection_new(pa_core* c, DBusConnection *conn, const char* name) {
    pa_dbus_connection *pconn;

    pconn = pa_xnew(pa_dbus_connection, 1);
    PA_REFCNT_INIT(pconn);
    pconn->core = c;
    pconn->property_name = name;
    pconn->connection = conn;
    pconn->dispatch_event = c->mainloop->defer_new(c->mainloop, dispatch_cb, conn);

    pa_shared_set(c, name, pconn);

    return pconn;
}

DBusConnection* pa_dbus_connection_get(pa_dbus_connection *c){
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) > 0);
    pa_assert(c->connection);

    return c->connection;
}

void pa_dbus_connection_unref(pa_dbus_connection *c) {
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) > 0);

    if (PA_REFCNT_DEC(c) > 0)
        return;

    if (dbus_connection_get_is_connected(c->connection)) {
        dbus_connection_close(c->connection);
         /* must process remaining messages, bit of a kludge to handle
         * both unload and shutdown */
        while (dbus_connection_read_write_dispatch(c->connection, -1));
    }

    /* already disconnected, just free */
    pa_shared_remove(c->core, c->property_name);
    c->core->mainloop->defer_free(c->dispatch_event);
    dbus_connection_unref(c->connection);
    pa_xfree(c);
}

pa_dbus_connection* pa_dbus_connection_ref(pa_dbus_connection *c) {
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) > 0);

    PA_REFCNT_INC(c);

    return c;
}

pa_dbus_connection* pa_dbus_bus_get(pa_core *c, DBusBusType type, DBusError *error) {

    static const char *const prop_name[] = {
        [DBUS_BUS_SESSION] = "dbus-connection-session",
        [DBUS_BUS_SYSTEM] = "dbus-connection-system",
        [DBUS_BUS_STARTER] = "dbus-connection-starter"
    };
    DBusConnection *conn;
    pa_dbus_connection *pconn;

    pa_assert(type == DBUS_BUS_SYSTEM || type == DBUS_BUS_SESSION || type == DBUS_BUS_STARTER);

    if ((pconn = pa_shared_get(c, prop_name[type])))
        return pa_dbus_connection_ref(pconn);

    if (!(conn = dbus_bus_get_private(type, error)))
        return NULL;

    pconn = pa_dbus_connection_new(c, conn, prop_name[type]);

    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    dbus_connection_set_dispatch_status_function(conn, dispatch_status, pconn, NULL);
    dbus_connection_set_watch_functions(conn, add_watch, remove_watch, toggle_watch, c, NULL);
    dbus_connection_set_timeout_functions(conn, add_timeout, remove_timeout, toggle_timeout, c, NULL);
    dbus_connection_set_wakeup_main_function(conn, wakeup_main, pconn, NULL);

    return pconn;
}
