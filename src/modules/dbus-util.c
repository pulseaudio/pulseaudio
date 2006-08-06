/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include <assert.h>
#include <pulsecore/log.h>
#include <pulsecore/props.h>
#include <pulse/xmalloc.h>

#include "dbus-util.h"

struct pa_dbus_connection {
    int refcount;
    pa_core *core;
    DBusConnection *connection;
    const char *property_name;
    pa_defer_event* dispatch_event;
};

static void dispatch_cb(pa_mainloop_api *ea, pa_defer_event *ev, void *userdata)
{
    DBusConnection *conn = (DBusConnection *) userdata;
    if (dbus_connection_dispatch(conn) == DBUS_DISPATCH_COMPLETE) {
        /* no more data to process, disable the deferred */
        ea->defer_enable(ev, 0);
    }
}

/* DBusDispatchStatusFunction callback for the pa mainloop */
static void dispatch_status(DBusConnection *conn, DBusDispatchStatus status,
                            void *userdata)
{
    pa_dbus_connection *c = (pa_dbus_connection*) userdata;
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

static pa_io_event_flags_t
get_watch_flags(DBusWatch *watch)
{
    unsigned int flags = dbus_watch_get_flags(watch);
    pa_io_event_flags_t events = PA_IO_EVENT_HANGUP | PA_IO_EVENT_ERROR;

    /* no watch flags for disabled watches */
    if (!dbus_watch_get_enabled(watch))
        return PA_IO_EVENT_NULL;

    if (flags & DBUS_WATCH_READABLE)
        events |= PA_IO_EVENT_INPUT;
    if (flags & DBUS_WATCH_WRITABLE)
        events |= PA_IO_EVENT_OUTPUT;

    return events;
}

static void timeval_next(struct timeval *tv, int millint)
{
    /* number of seconds in the milli-second interval */
    tv->tv_sec += (millint / 1000);
    /* milliseconds minus the seconds portion, converted to microseconds */
    tv->tv_usec += (millint - tv->tv_sec * 1000) * 1000;
}

/* pa_io_event_cb_t IO event handler */
static void handle_io_event(PA_GCC_UNUSED pa_mainloop_api *ea, pa_io_event *e,
                            int fd, pa_io_event_flags_t events, void *userdata)
{
    unsigned int flags = 0;
    DBusWatch *watch = (DBusWatch*) userdata;

    assert(fd == dbus_watch_get_fd(watch));

    if (!dbus_watch_get_enabled(watch)) {
        pa_log_warn(__FILE__": Asked to handle disabled watch: %p %i",
                    (void *) watch, fd);
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
static void handle_time_event(pa_mainloop_api *ea, pa_time_event* e,
                              const struct timeval *tv, void *userdata)
{
    DBusTimeout *timeout = (DBusTimeout*) userdata;

    if (dbus_timeout_get_enabled(timeout)) {
        struct timeval next = *tv;
        dbus_timeout_handle(timeout);

        /* restart it for the next scheduled time */
        timeval_next(&next, dbus_timeout_get_interval(timeout));
        ea->time_restart(e, &next);
    }
}

/* DBusAddWatchFunction callback for pa mainloop */
static dbus_bool_t add_watch(DBusWatch *watch, void *data)
{
    pa_io_event *ev;
    pa_core *c = (pa_core*) data;

    ev = c->mainloop->io_new(c->mainloop, dbus_watch_get_fd(watch),
                             get_watch_flags(watch),
                             handle_io_event, (void*) watch);
    if (NULL == ev)
        return FALSE;

    /* dbus_watch_set_data(watch, (void*) ev, c->mainloop->io_free); */
    dbus_watch_set_data(watch, (void*) ev, NULL);

    return TRUE;
}

/* DBusRemoveWatchFunction callback for pa mainloop */
static void remove_watch(DBusWatch *watch, void *data)
{
    pa_core *c = (pa_core*) data;
    pa_io_event *ev = (pa_io_event*) dbus_watch_get_data(watch);

    /* free the event */
    if (NULL != ev)
        c->mainloop->io_free(ev);
}

/* DBusWatchToggledFunction callback for pa mainloop */
static void toggle_watch(DBusWatch *watch, void *data)
{
    pa_core *c = (pa_core*) data;
    pa_io_event *ev = (pa_io_event*) dbus_watch_get_data(watch);

    /* get_watch_flags() checks if the watch is enabled */
    c->mainloop->io_enable(ev, get_watch_flags(watch));
}

/* DBusAddTimeoutFunction callback for pa mainloop */
static dbus_bool_t add_timeout(DBusTimeout *timeout, void *data)
{
    struct timeval tv;
    pa_time_event *ev;
    pa_core *c = (pa_core*) data;

    if (!dbus_timeout_get_enabled(timeout))
        return FALSE;

    if (gettimeofday(&tv, NULL) < 0)
        return -1;

    timeval_next(&tv, dbus_timeout_get_interval(timeout));

    ev = c->mainloop->time_new(c->mainloop, &tv, handle_time_event,
                               (void*) timeout);
    if (NULL == ev)
        return FALSE;

    /* dbus_timeout_set_data(timeout, (void*) ev, c->mainloop->time_free); */
    dbus_timeout_set_data(timeout, (void*) ev, NULL);

    return TRUE;
}

/* DBusRemoveTimeoutFunction callback for pa mainloop */
static void remove_timeout(DBusTimeout *timeout, void *data)
{
    pa_core *c = (pa_core*) data;
    pa_time_event *ev = (pa_time_event*) dbus_timeout_get_data(timeout);

    /* free the event */
    if (NULL != ev)
        c->mainloop->time_free(ev);
}

/* DBusTimeoutToggledFunction callback for pa mainloop */
static void toggle_timeout(DBusTimeout *timeout, void *data)
{
    struct timeval tv;
    pa_core *c = (pa_core*) data;
    pa_time_event *ev = (pa_time_event*) dbus_timeout_get_data(timeout);

    gettimeofday(&tv, NULL);
    if (dbus_timeout_get_enabled(timeout)) {
        timeval_next(&tv, dbus_timeout_get_interval(timeout));
        c->mainloop->time_restart(ev, &tv);
    } else {
        /* set it to expire one second ago */
        tv.tv_sec -= 1;
        c->mainloop->time_restart(ev, &tv);
    }
}

static void
pa_dbus_connection_free(pa_dbus_connection *c)
{
    assert(c);
    assert(!dbus_connection_get_is_connected(c->connection));

    /* already disconnected, just free */
    pa_property_remove(c->core, c->property_name);
    c->core->mainloop->defer_free(c->dispatch_event);
    dbus_connection_unref(c->connection);
    pa_xfree(c);
}

static void
wakeup_main(void *userdata)
{
    pa_dbus_connection *c = (pa_dbus_connection*) userdata;
    /* this will wakeup the mainloop and dispatch events, although
     * it may not be the cleanest way of accomplishing it */
    c->core->mainloop->defer_enable(c->dispatch_event, 1);
}

static pa_dbus_connection* pa_dbus_connection_new(pa_core* c, DBusConnection *conn, const char* name)
{
    pa_dbus_connection *pconn = pa_xmalloc(sizeof(pa_dbus_connection));

    pconn->refcount = 1;
    pconn->core = c;
    pconn->property_name = name;
    pconn->connection = conn;
    pconn->dispatch_event = c->mainloop->defer_new(c->mainloop, dispatch_cb,
                                                   (void*) conn);

    pa_property_set(c, name, pconn);

    return pconn;
}

DBusConnection* pa_dbus_connection_get(pa_dbus_connection *c)
{
    assert(c && c->connection);
    return c->connection;
}

void pa_dbus_connection_unref(pa_dbus_connection *c)
{
    assert(c);

    /* non-zero refcount, still outstanding refs */
    if (--(c->refcount))
        return;

    /* refcount is zero */
    if (dbus_connection_get_is_connected(c->connection)) {
        /* disconnect as we have no more internal references */
        dbus_connection_close(c->connection);
        /* must process remaining messages, bit of a kludge to
         * handle both unload and shutdown */
        while(dbus_connection_read_write_dispatch(c->connection, -1));
    }
    pa_dbus_connection_free(c);
}

pa_dbus_connection* pa_dbus_connection_ref(pa_dbus_connection *c)
{
    assert(c);

    ++(c->refcount);

    return c;
}

pa_dbus_connection* pa_dbus_bus_get(pa_core *c, DBusBusType type,
                                    DBusError *error)
{
    const char* name;
    DBusConnection *conn;
    pa_dbus_connection *pconn;
    static const char sysname[] = "dbus-connection-system";
    static const char sessname[] = "dbus-connection-session";
    static const char startname[] = "dbus-connection-starter";

    switch (type) {
        case DBUS_BUS_SYSTEM:
            name = sysname;
            break;
        case DBUS_BUS_SESSION:
            name = sessname;
            break;
        case DBUS_BUS_STARTER:
            name = startname;
            break;
        default:
            assert(0); /* never reached */
            break;
    }

    if ((pconn = pa_property_get(c, name)))
        return pa_dbus_connection_ref(pconn);

    /* else */
    conn = dbus_bus_get_private(type, error);
    if (conn == NULL || dbus_error_is_set(error)) {
        return NULL;
    }

    pconn = pa_dbus_connection_new(c, conn, name);

    /* don't exit on disconnect */
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    /* set up the DBUS call backs */
    dbus_connection_set_dispatch_status_function(conn, dispatch_status,
                                                 (void*) pconn, NULL);
    dbus_connection_set_watch_functions(conn,
                                        add_watch,
                                        remove_watch,
                                        toggle_watch,
                                        (void*) c, NULL);
    dbus_connection_set_timeout_functions(conn,
                                          add_timeout,
                                          remove_timeout,
                                          toggle_timeout,
                                          (void*) c, NULL);
    dbus_connection_set_wakeup_main_function(conn, wakeup_main, pconn, NULL);

    return pconn;
}
