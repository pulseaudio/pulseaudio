#ifndef foomainloopapihfoo
#define foomainloopapihfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pa_io_event_flags {
    PA_IO_EVENT_NULL = 0,
    PA_IO_EVENT_INPUT = 1,
    PA_IO_EVENT_OUTPUT = 2,
    PA_IO_EVENT_HANGUP = 4,
    PA_IO_EVENT_ERROR = 8,
};

struct pa_io_event;
struct pa_defer_event;
struct pa_time_event;

struct pa_mainloop_api {
    void *userdata;

    /* IO sources */
    struct pa_io_event* (*io_new)(struct pa_mainloop_api*a, int fd, enum pa_io_event_flags events, void (*callback) (struct pa_mainloop_api*a, struct pa_io_event* e, int fd, enum pa_io_event_flags events, void *userdata), void *userdata);
    void (*io_enable)(struct pa_io_event* e, enum pa_io_event_flags events);
    void (*io_free)(struct pa_io_event* e);
    void (*io_set_destroy)(struct pa_io_event *e, void (*callback) (struct pa_mainloop_api*a, struct pa_io_event *e, void *userdata));

    /* Time sources */
    struct pa_time_event* (*time_new)(struct pa_mainloop_api*a, const struct timeval *tv, void (*callback) (struct pa_mainloop_api*a, struct pa_time_event* e, const struct timeval *tv, void *userdata), void *userdata);
    void (*time_restart)(struct pa_time_event* e, const struct timeval *tv);
    void (*time_free)(struct pa_time_event* e);
    void (*time_set_destroy)(struct pa_time_event *e, void (*callback) (struct pa_mainloop_api*a, struct pa_time_event *e, void *userdata));

    /* Deferred sources */
    struct pa_defer_event* (*defer_new)(struct pa_mainloop_api*a, void (*callback) (struct pa_mainloop_api*a, struct pa_defer_event* e, void *userdata), void *userdata);
    void (*defer_enable)(struct pa_defer_event* e, int b);
    void (*defer_free)(struct pa_defer_event* e);
    void (*defer_set_destroy)(struct pa_defer_event *e, void (*callback) (struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata));

    /* Exit mainloop */
    void (*quit)(struct pa_mainloop_api*a, int retval);
};

void pa_mainloop_api_once(struct pa_mainloop_api*m, void (*callback)(struct pa_mainloop_api*m, void *userdata), void *userdata);

#ifdef __cplusplus
}
#endif

#endif
