/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2006 Shams E. King

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

#include <stdarg.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>

#include "dbus-util.h"

struct pa_dbus_wrap_connection {
    pa_mainloop_api *mainloop;
    DBusConnection *connection;
    pa_defer_event* dispatch_event;
    pa_bool_t use_rtclock:1;
};

struct timeout_data {
    pa_dbus_wrap_connection *c;
    DBusTimeout *timeout;
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
    pa_dbus_wrap_connection *c = userdata;

    pa_assert(c);

    switch(status) {

        case DBUS_DISPATCH_COMPLETE:
            c->mainloop->defer_enable(c->dispatch_event, 0);
            break;

        case DBUS_DISPATCH_DATA_REMAINS:
        case DBUS_DISPATCH_NEED_MEMORY:
        default:
            c->mainloop->defer_enable(c->dispatch_event, 1);
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
static void handle_time_event(pa_mainloop_api *ea, pa_time_event* e, const struct timeval *t, void *userdata) {
    struct timeval tv;
    struct timeout_data *d = userdata;

    pa_assert(d);
    pa_assert(d->c);

    if (dbus_timeout_get_enabled(d->timeout)) {
        dbus_timeout_handle(d->timeout);

        /* restart it for the next scheduled time */
        ea->time_restart(e, pa_timeval_rtstore(&tv, pa_timeval_load(t) + dbus_timeout_get_interval(d->timeout) * PA_USEC_PER_MSEC, d->c->use_rtclock));
    }
}

/* DBusAddWatchFunction callback for pa mainloop */
static dbus_bool_t add_watch(DBusWatch *watch, void *data) {
    pa_dbus_wrap_connection *c = data;
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
    pa_dbus_wrap_connection *c = data;
    pa_io_event *ev;

    pa_assert(watch);
    pa_assert(c);

    if ((ev = dbus_watch_get_data(watch)))
        c->mainloop->io_free(ev);
}

/* DBusWatchToggledFunction callback for pa mainloop */
static void toggle_watch(DBusWatch *watch, void *data) {
    pa_dbus_wrap_connection *c = data;
    pa_io_event *ev;

    pa_assert(watch);
    pa_assert(c);

    pa_assert_se(ev = dbus_watch_get_data(watch));

    /* get_watch_flags() checks if the watch is enabled */
    c->mainloop->io_enable(ev, get_watch_flags(watch));
}

static void time_event_destroy_cb(pa_mainloop_api *a, pa_time_event *e, void *userdata) {
    pa_xfree(userdata);
}

/* DBusAddTimeoutFunction callback for pa mainloop */
static dbus_bool_t add_timeout(DBusTimeout *timeout, void *data) {
    pa_dbus_wrap_connection *c = data;
    pa_time_event *ev;
    struct timeval tv;
    struct timeout_data *d;

    pa_assert(timeout);
    pa_assert(c);

    if (!dbus_timeout_get_enabled(timeout))
        return FALSE;

    d = pa_xnew(struct timeout_data, 1);
    d->c = c;
    d->timeout = timeout;
    ev = c->mainloop->time_new(c->mainloop, pa_timeval_rtstore(&tv, pa_rtclock_now() + dbus_timeout_get_interval(timeout) * PA_USEC_PER_MSEC, c->use_rtclock), handle_time_event, d);
    c->mainloop->time_set_destroy(ev, time_event_destroy_cb);

    dbus_timeout_set_data(timeout, ev, NULL);

    return TRUE;
}

/* DBusRemoveTimeoutFunction callback for pa mainloop */
static void remove_timeout(DBusTimeout *timeout, void *data) {
    pa_dbus_wrap_connection *c = data;
    pa_time_event *ev;

    pa_assert(timeout);
    pa_assert(c);

    if ((ev = dbus_timeout_get_data(timeout)))
        c->mainloop->time_free(ev);
}

/* DBusTimeoutToggledFunction callback for pa mainloop */
static void toggle_timeout(DBusTimeout *timeout, void *data) {
    struct timeout_data *d = data;
    pa_time_event *ev;
    struct timeval tv;

    pa_assert(d);
    pa_assert(d->c);
    pa_assert(timeout);

    pa_assert_se(ev = dbus_timeout_get_data(timeout));

    if (dbus_timeout_get_enabled(timeout)) {
        d->c->mainloop->time_restart(ev, pa_timeval_rtstore(&tv, pa_rtclock_now() + dbus_timeout_get_interval(timeout) * PA_USEC_PER_MSEC, d->c->use_rtclock));
    } else
        d->c->mainloop->time_restart(ev, pa_timeval_rtstore(&tv, PA_USEC_INVALID, d->c->use_rtclock));
}

static void wakeup_main(void *userdata) {
    pa_dbus_wrap_connection *c = userdata;

    pa_assert(c);

    /* this will wakeup the mainloop and dispatch events, although
     * it may not be the cleanest way of accomplishing it */
    c->mainloop->defer_enable(c->dispatch_event, 1);
}

pa_dbus_wrap_connection* pa_dbus_wrap_connection_new(pa_mainloop_api *m, pa_bool_t use_rtclock, DBusBusType type, DBusError *error) {
    DBusConnection *conn;
    pa_dbus_wrap_connection *pconn;
    char *id;

    pa_assert(type == DBUS_BUS_SYSTEM || type == DBUS_BUS_SESSION || type == DBUS_BUS_STARTER);

    if (!(conn = dbus_bus_get_private(type, error)))
        return NULL;

    pconn = pa_xnew(pa_dbus_wrap_connection, 1);
    pconn->mainloop = m;
    pconn->connection = conn;
    pconn->use_rtclock = use_rtclock;

    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    dbus_connection_set_dispatch_status_function(conn, dispatch_status, pconn, NULL);
    dbus_connection_set_watch_functions(conn, add_watch, remove_watch, toggle_watch, pconn, NULL);
    dbus_connection_set_timeout_functions(conn, add_timeout, remove_timeout, toggle_timeout, pconn, NULL);
    dbus_connection_set_wakeup_main_function(conn, wakeup_main, pconn, NULL);

    pconn->dispatch_event = pconn->mainloop->defer_new(pconn->mainloop, dispatch_cb, conn);

    pa_log_debug("Successfully connected to D-Bus %s bus %s as %s",
                 type == DBUS_BUS_SYSTEM ? "system" : (type == DBUS_BUS_SESSION ? "session" : "starter"),
                 pa_strnull((id = dbus_connection_get_server_id(conn))),
                 pa_strnull(dbus_bus_get_unique_name(conn)));

    dbus_free(id);

    return pconn;
}

void pa_dbus_wrap_connection_free(pa_dbus_wrap_connection* c) {
    pa_assert(c);

    if (dbus_connection_get_is_connected(c->connection)) {
        dbus_connection_close(c->connection);
        /* must process remaining messages, bit of a kludge to handle
         * both unload and shutdown */
        while (dbus_connection_read_write_dispatch(c->connection, -1))
            ;
    }

    c->mainloop->defer_free(c->dispatch_event);
    dbus_connection_unref(c->connection);
    pa_xfree(c);
}

DBusConnection* pa_dbus_wrap_connection_get(pa_dbus_wrap_connection *c) {
  pa_assert(c);
  pa_assert(c->connection);

  return c->connection;
}

int pa_dbus_add_matches(DBusConnection *c, DBusError *error, ...) {
    const char *t;
    va_list ap;
    unsigned k = 0;

    pa_assert(c);
    pa_assert(error);

    va_start(ap, error);
    while ((t = va_arg(ap, const char*))) {
        dbus_bus_add_match(c, t, error);

        if (dbus_error_is_set(error))
            goto fail;

        k++;
    }
    va_end(ap);
    return 0;

fail:

    va_end(ap);
    va_start(ap, error);
    for (; k > 0; k--) {
        DBusError e;

        pa_assert_se(t = va_arg(ap, const char*));

        dbus_error_init(&e);
        dbus_bus_remove_match(c, t, &e);
        dbus_error_free(&e);
    }
    va_end(ap);

    return -1;
}

void pa_dbus_remove_matches(DBusConnection *c, ...) {
    const char *t;
    va_list ap;
    DBusError error;

    pa_assert(c);

    dbus_error_init(&error);

    va_start(ap, c);
    while ((t = va_arg(ap, const char*))) {
        dbus_bus_remove_match(c, t, &error);
        dbus_error_free(&error);
    }
    va_end(ap);
}

pa_dbus_pending *pa_dbus_pending_new(
        DBusConnection *c,
        DBusMessage *m,
        DBusPendingCall *pending,
        void *context_data,
        void *call_data) {

    pa_dbus_pending *p;

    pa_assert(pending);

    p = pa_xnew(pa_dbus_pending, 1);
    p->connection = c;
    p->message = m;
    p->pending = pending;
    p->context_data = context_data;
    p->call_data = call_data;

    PA_LLIST_INIT(pa_dbus_pending, p);

    return p;
}

void pa_dbus_pending_free(pa_dbus_pending *p) {
    pa_assert(p);

    if (p->pending) {
        dbus_pending_call_cancel(p->pending);
        dbus_pending_call_unref(p->pending);
    }

    if (p->message)
        dbus_message_unref(p->message);

    pa_xfree(p);
}

void pa_dbus_sync_pending_list(pa_dbus_pending **p) {
    pa_assert(p);

    while (*p && dbus_connection_read_write_dispatch((*p)->connection, -1))
        ;
}

void pa_dbus_free_pending_list(pa_dbus_pending **p) {
    pa_dbus_pending *i;

    pa_assert(p);

    while ((i = *p)) {
        PA_LLIST_REMOVE(pa_dbus_pending, *p, i);
        pa_dbus_pending_free(i);
    }
}
