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

enum pa_mainloop_api_io_events {
    PA_MAINLOOP_API_IO_EVENT_NULL = 0,
    PA_MAINLOOP_API_IO_EVENT_INPUT = 1,
    PA_MAINLOOP_API_IO_EVENT_OUTPUT = 2,
    PA_MAINLOOP_API_IO_EVENT_BOTH = 3,
    PA_MAINLOOP_API_IO_EVENT_HUP = 4
};

struct pa_mainloop_api {
    void *userdata;

    /* IO sources */
    void* (*source_io)(struct pa_mainloop_api*a, int fd, enum pa_mainloop_api_io_events events, void (*callback) (struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata), void *userdata);
    void  (*enable_io)(struct pa_mainloop_api*a, void* id, enum pa_mainloop_api_io_events events);
    void  (*cancel_io)(struct pa_mainloop_api*a, void* id);

    /* Fixed sources */
    void* (*source_fixed)(struct pa_mainloop_api*a, void (*callback) (struct pa_mainloop_api*a, void *id, void *userdata), void *userdata);
    void  (*enable_fixed)(struct pa_mainloop_api*a, void* id, int b);
    void  (*cancel_fixed)(struct pa_mainloop_api*a, void* id);

    /* Idle sources */
    void* (*source_idle)(struct pa_mainloop_api*a, void (*callback) (struct pa_mainloop_api*a, void *id, void *userdata), void *userdata);
    void  (*enable_idle)(struct pa_mainloop_api*a, void* id, int b);
    void  (*cancel_idle)(struct pa_mainloop_api*a, void* id);
    
    /* Time sources */
    void* (*source_time)(struct pa_mainloop_api*a, const struct timeval *tv, void (*callback) (struct pa_mainloop_api*a, void *id, const struct timeval *tv, void *userdata), void *userdata);
    void  (*enable_time)(struct pa_mainloop_api*a, void *id, const struct timeval *tv);
    void  (*cancel_time)(struct pa_mainloop_api*a, void* id);

    /* Exit mainloop */
    void (*quit)(struct pa_mainloop_api*a, int retval);
};

void pa_mainloop_api_once(struct pa_mainloop_api*m, void (*callback)(void *userdata), void *userdata);

#endif
