/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <assert.h>
#include <stdio.h>

#include "llist.h"
#include "x11wrap.h"
#include "xmalloc.h"
#include "log.h"
#include "props.h"

struct pa_x11_client;

struct pa_x11_internal {
    PA_LLIST_FIELDS(struct pa_x11_internal);
    struct pa_x11_wrapper *wrapper;
    struct pa_io_event* io_event;
    int fd;
};

struct pa_x11_wrapper {
    struct pa_core *core;
    int ref;
    
    char *property_name;
    Display *display;

    struct pa_defer_event* defer_event;
    struct pa_io_event* io_event;

    PA_LLIST_HEAD(struct pa_x11_client, clients);
    PA_LLIST_HEAD(struct pa_x11_internal, internals);
};

struct pa_x11_client {
    PA_LLIST_FIELDS(struct pa_x11_client);
    struct pa_x11_wrapper *wrapper;
    int (*callback)(struct pa_x11_wrapper *w, XEvent *e, void *userdata);
    void *userdata;
};

/* Dispatch all pending X11 events */
static void work(struct pa_x11_wrapper *w) {
    assert(w && w->ref >= 1);
    
    while (XPending(w->display)) {
        struct pa_x11_client *c;
        XEvent e;
        XNextEvent(w->display, &e);

        for (c = w->clients; c; c = c->next) {
            assert(c->callback);
            if (c->callback(w, &e, c->userdata) != 0)
                break;
        }
    }
}

/* IO notification event for the X11 display connection */
static void display_io_event(struct pa_mainloop_api *m, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    struct pa_x11_wrapper *w = userdata;
    assert(m && e && fd >= 0 && w && w->ref >= 1);
    work(w);
}

/* Deferred notification event. Called once each main loop iteration */
static void defer_event(struct pa_mainloop_api *m, struct pa_defer_event *e, void *userdata) {
    struct pa_x11_wrapper *w = userdata;
    assert(m && e && w && w->ref >= 1);
    work(w);
}

/* IO notification event for X11 internal connections */
static void internal_io_event(struct pa_mainloop_api *m, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    struct pa_x11_wrapper *w = userdata;
    assert(m && e && fd >= 0 && w && w->ref >= 1);

    XProcessInternalConnection(w->display, fd);
}

/* Add a new IO source for the specified X11 internal connection */
static struct pa_x11_internal* x11_internal_add(struct pa_x11_wrapper *w, int fd) {
    struct pa_x11_internal *i;
    assert(i && fd >= 0);

    i = pa_xmalloc(sizeof(struct pa_x11_internal));
    i->wrapper = w;
    i->io_event = w->core->mainloop->io_new(w->core->mainloop, fd, PA_IO_EVENT_INPUT, internal_io_event, w);
    i->fd = fd;

    PA_LLIST_PREPEND(struct pa_x11_internal, w->internals, i);
    return i;
}

/* Remove an IO source for an X11 internal connection */
void x11_internal_remove(struct pa_x11_wrapper *w, struct pa_x11_internal *i) {
    assert(i);

    PA_LLIST_REMOVE(struct pa_x11_internal, w->internals, i);
    w->core->mainloop->io_free(i->io_event);
    pa_xfree(i);
}

/* Implementation of XConnectionWatchProc */
static void x11_watch(Display *display, XPointer userdata, int fd, Bool opening, XPointer *watch_data) {
    struct pa_x11_wrapper *w = (struct pa_x11_wrapper*) userdata;
    assert(display && w && fd >= 0);

    if (opening)
        *watch_data = (XPointer) x11_internal_add(w, fd);
    else
        x11_internal_remove(w, (struct pa_x11_internal*) *watch_data);
}

static struct pa_x11_wrapper* x11_wrapper_new(struct pa_core *c, const char *name, const char *t) {
    struct pa_x11_wrapper*w;
    Display *d;
    int r;

    if (!(d = XOpenDisplay(name))) {
        pa_log(__FILE__": XOpenDisplay() failed\n");
        return NULL;
    }

    w = pa_xmalloc(sizeof(struct pa_x11_wrapper));
    w->core = c;
    w->ref = 1;
    w->property_name = pa_xstrdup(t);
    w->display = d;
    
    PA_LLIST_HEAD_INIT(struct pa_x11_client, w->clients);
    PA_LLIST_HEAD_INIT(struct pa_x11_internal, w->internals);

    w->defer_event = c->mainloop->defer_new(c->mainloop, defer_event, w);
    w->io_event = c->mainloop->io_new(c->mainloop, ConnectionNumber(d), PA_IO_EVENT_INPUT, display_io_event, w);

    XAddConnectionWatch(d, x11_watch, (XPointer) w);
    
    r = pa_property_set(c, w->property_name, w);
    assert(r >= 0);
    
    return w;
}

static void x11_wrapper_free(struct pa_x11_wrapper*w) {
    int r;
    assert(w);

    r = pa_property_remove(w->core, w->property_name);
    assert(r >= 0);

    assert(!w->clients);

    XRemoveConnectionWatch(w->display, x11_watch, (XPointer) w);
    XCloseDisplay(w->display);
    
    w->core->mainloop->io_free(w->io_event);
    w->core->mainloop->defer_free(w->defer_event);

    while (w->internals)
        x11_internal_remove(w, w->internals);
    
    pa_xfree(w->property_name);
    pa_xfree(w);
}

struct pa_x11_wrapper* pa_x11_wrapper_get(struct pa_core *c, const char *name) {
    char t[256];
    struct pa_x11_wrapper *w;
    assert(c);
        
    snprintf(t, sizeof(t), "x11-wrapper%s%s", name ? "-" : "", name ? name : "");
    if ((w = pa_property_get(c, t)))
        return pa_x11_wrapper_ref(w);

    return x11_wrapper_new(c, name, t);
}

struct pa_x11_wrapper* pa_x11_wrapper_ref(struct pa_x11_wrapper *w) {
    assert(w && w->ref >= 1);
    w->ref++;
    return w;
}

void pa_x11_wrapper_unref(struct pa_x11_wrapper* w) {
    assert(w && w->ref >= 1);

    if (!(--w->ref))
        x11_wrapper_free(w);
}

Display *pa_x11_wrapper_get_display(struct pa_x11_wrapper *w) {
    assert(w && w->ref >= 1);
    return w->display;
}

struct pa_x11_client* pa_x11_client_new(struct pa_x11_wrapper *w, int (*cb)(struct pa_x11_wrapper *w, XEvent *e, void *userdata), void *userdata) {
    struct pa_x11_client *c;
    assert(w && w->ref >= 1);

    c = pa_xmalloc(sizeof(struct pa_x11_client));
    c->wrapper = w;
    c->callback = cb;
    c->userdata = userdata;

    PA_LLIST_PREPEND(struct pa_x11_client, w->clients, c);

    return c;
}

void pa_x11_client_free(struct pa_x11_client *c) {
    assert(c && c->wrapper && c->wrapper->ref >= 1);

    PA_LLIST_REMOVE(struct pa_x11_client, c->wrapper->clients, c);
    pa_xfree(c);
}
