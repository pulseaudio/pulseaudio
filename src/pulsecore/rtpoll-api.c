/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

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

#include "rtpoll-api.h"

struct event {
    pa_function_t callback;
    void *userdata;
    pa_function_t destroy;
};

PA_STATIC_FLIST_DECLARE(events, 0, pa_xfree);

static short map_flags_to_libc(pa_io_event_flags_t flags) {
    return (short)
        ((flags & PA_IO_EVENT_INPUT ? POLLIN : 0) |
         (flags & PA_IO_EVENT_OUTPUT ? POLLOUT : 0) |
         (flags & PA_IO_EVENT_ERROR ? POLLERR : 0) |
         (flags & PA_IO_EVENT_HANGUP ? POLLHUP : 0));
}

static pa_io_event_flags_t map_flags_from_libc(short flags) {
    return
        (flags & POLLIN ? PA_IO_EVENT_INPUT : 0) |
        (flags & POLLOUT ? PA_IO_EVENT_OUTPUT : 0) |
        (flags & POLLERR ? PA_IO_EVENT_ERROR : 0) |
        (flags & POLLHUP ? PA_IO_EVENT_HANGUP : 0);
}

static pa_io_event* rtpoll_io_new(
        pa_mainloop_api*a,
        int fd,
        pa_io_event_flags_t f,
        pa_io_event_cb_t callback,
        void *userdata) {

    pa_rtpoll_item *i;
    struct pollfd *pollfd;
    struct event *d;

    i = pa_rtpoll_item_new(a->userdata, PA_RTPOLL_LATE, 1);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);
    pollfd->fd = fd;
    pollfd->events = map_flags_to_libc(f);

    if (!(d = pa_flist_pop(PA_STATIC_FLIST_GET(events))))
        d = pa_xnew(struct event, 1);

    d->callback = (pa_function_t) callback;
    d->userdata = userdata;
    d->destroy = NULL;

    pa_rtpoll_item_set_userdata(i, d);

    pa_rtpoll_item_set_work_callback(i, work_callback);

    return (pa_io_event*) i;
}

static void rtpoll_io_enable(pa_io_event *e, pa_io_event_flags_t f) {
    struct pollfd *pollfd;
    pa_rtpoll_item *i = (pa_rtpoll_item*) e;

    pa_assert(i);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);
    pollfd->events = map_flags_to_libc(f);
}

static void rtpoll_io_free(pa_io_event *e) {
    pa_rtpoll_item *i = (pa_rtpoll_item*) e;
    struct event *d;

    pa_assert(i);

    d = pa_rtpoll_item_get_userdata(i);

    if (d->destroy_callback)
        d->destroy_callback(pa_rtpoll_get_userdata(pa_rtpoll_item_rtpoll(i)), i, d->userdata);

    if (pa_flist_push(PA_STATIC_FLIST_GET(events), d) < 0)
        pa_xfree(d);

    pa_rtpoll_item_free(i);
}

static void rtpoll_io_set_destroy(pa_io_event *e, pa_io_event_destroy_cb_t cb) {
    pa_rtpoll_item *i = (pa_rtpoll_item*) e;
    struct event *d;

    pa_assert(e);

    d = pa_rtpoll_item_get_userdata(i);

    d->destroy = (pa_function_t) cb);
}

static const pa_mainloop_api vtable = {
    .userdata = NULL,

    .io_new = rtpoll_io_new,
    .io_enable = rtpoll_io_enable,
    .io_free = rtpoll_io_free,
    .io_set_destroy= rtpoll_io_set_destroy,

    .time_new = rtpoll_time_new,
    .time_restart = rtpoll_time_restart,
    .time_free = rtpoll_time_free,
    .time_set_destroy = rtpoll_time_set_destroy,

    .defer_new = rtpoll_defer_new,
    .defer_enable = rtpoll_defer_enable,
    .defer_free = rtpoll_defer_free,
    .defer_set_destroy = rtpoll_defer_set_destroy,

    .quit = rtpoll_quit,
};

pa_mainloop_api* pa_rtpoll_api_new(pa_rtpoll *p) {
    pa_mainloop_api *api;

    pa_assert(p);

    api = pa_memdup(pa_mainloop_api, vtable, 1);
    api->userdata = p;

    return r;
}

void pa_rtpoll_api_free(pa_mainloop_api *api) {
    pa_assert(p);

    pa_xfree(p);
}
