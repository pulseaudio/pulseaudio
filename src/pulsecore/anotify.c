/* $Id$ */

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include <pulse/xmalloc.h>

#include "anotify.h"

#define EVENTS_MAX 16

struct pa_anotify {
    pa_mainloop_api *api;
    pa_anotify_cb_t callback;
    void *userdata;
    int fds[2];
    pa_io_event *io_event;
    pa_defer_event *defer_event;

    uint8_t queued_events[EVENTS_MAX];
    unsigned n_queued_events, queue_index;
};

static void dispatch_event(pa_anotify *a) {
    assert(a);
    assert(a->queue_index < a->n_queued_events);

    a->callback(a->queued_events[a->queue_index++], a->userdata);

    if (a->queue_index >= a->n_queued_events) {
        a->n_queued_events = 0;
        a->queue_index = 0;

        a->api->io_enable(a->io_event, PA_IO_EVENT_INPUT);
        a->api->defer_enable(a->defer_event, 0);
    } else {
        a->api->io_enable(a->io_event, 0);
        a->api->defer_enable(a->defer_event, 1);
    }
}

static void io_callback(
        pa_mainloop_api *api,
        pa_io_event *e,
        int fd,
        pa_io_event_flags_t events,
        void *userdata) {

    pa_anotify *a = userdata;
    ssize_t r;

    assert(a);
    assert(events == PA_IO_EVENT_INPUT);
    assert(a->n_queued_events == 0);

    r = read(fd, a->queued_events, sizeof(a->queued_events));
    assert(r > 0);

    a->n_queued_events = (unsigned) r;
    a->queue_index = 0;

    /* Only dispatch a single event */
    dispatch_event(a);
}

static void defer_callback(pa_mainloop_api *api, pa_defer_event *e, void *userdata) {
    pa_anotify *a = userdata;
    assert(a);

    dispatch_event(a);
}

pa_anotify *pa_anotify_new(pa_mainloop_api*api, pa_anotify_cb_t cb, void *userdata) {
    pa_anotify *a;

    assert(api);
    assert(cb);

    a = pa_xnew(pa_anotify, 1);

    if (pipe(a->fds) < 0) {
        pa_xfree(a);
        return NULL;
    }

    a->api = api;
    a->callback = cb;
    a->userdata = userdata;

    a->io_event = api->io_new(api, a->fds[0], PA_IO_EVENT_INPUT, io_callback, a);
    a->defer_event = api->defer_new(api, defer_callback, a);
    a->api->defer_enable(a->defer_event, 0);

    a->n_queued_events = 0;

    return a;
}

void pa_anotify_free(pa_anotify *a) {
    assert(a);

    a->api->io_free(a->io_event);
    a->api->defer_free(a->defer_event);

    if (a->fds[0] >= 0)
        close(a->fds[0]);
    if (a->fds[1] >= 0)
        close(a->fds[1]);

    pa_xfree(a);
}

int pa_anotify_signal(pa_anotify *a, uint8_t event) {
    ssize_t r;
    assert(a);

    r = write(a->fds[1], &event, 1);
    return r != 1 ? -1 : 0;
}
