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

#include <sys/time.h>
#include <time.h>

#include "cdecl.h"

PA_C_DECL_BEGIN

/** A bitmask for IO events */
enum pa_io_event_flags {
    PA_IO_EVENT_NULL = 0,     /**< No event */
    PA_IO_EVENT_INPUT = 1,    /**< Input event */
    PA_IO_EVENT_OUTPUT = 2,   /**< Output event */
    PA_IO_EVENT_HANGUP = 4,   /**< Hangup event */
    PA_IO_EVENT_ERROR = 8,    /**< Error event */
};

/** \struct pa_io_event
 * An IO event source object */
struct pa_io_event;

/** \struct pa_defer_event
 * A deferred event source object. Events of this type are triggered once in every main loop iteration */
struct pa_defer_event;

/** \struct pa_time_event
 * A timer event source object */
struct pa_time_event;

/** An abstract mainloop API vtable */
struct pa_mainloop_api {
    /** A pointer to some private, arbitrary data of the main loop implementation */
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

    /** Create a new deferred event source object */
    struct pa_defer_event* (*defer_new)(struct pa_mainloop_api*a, void (*callback) (struct pa_mainloop_api*a, struct pa_defer_event* e, void *userdata), void *userdata);

    /** Enable or disable a deferred event source temporarily */
    void (*defer_enable)(struct pa_defer_event* e, int b);

    /** Free a deferred event source object */
    void (*defer_free)(struct pa_defer_event* e);

    /** Set a function that is called when the deferred event source is destroyed. Use this to free the userdata argument if required */
    void (*defer_set_destroy)(struct pa_defer_event *e, void (*callback) (struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata));

    /** Exit the main loop and return the specfied retval*/
    void (*quit)(struct pa_mainloop_api*a, int retval);
};

/** Run the specified callback function once from the main loop using an anonymous defer event. */
void pa_mainloop_api_once(struct pa_mainloop_api*m, void (*callback)(struct pa_mainloop_api*m, void *userdata), void *userdata);

PA_C_DECL_END

#endif
