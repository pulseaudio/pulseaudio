#ifndef foothreadmainloophfoo
#define foothreadmainloophfoo

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

#include <polyp/mainloop-api.h>
#include <polyp/cdecl.h>

PA_C_DECL_BEGIN

/** \file
 * 
 * A thread based main loop implementation based on pa_mainloop.*/

/** An opaque main loop object */
typedef struct pa_threaded_mainloop pa_threaded_mainloop;

/** Allocate a new main loop object */
pa_threaded_mainloop *pa_threaded_mainloop_new(void);

/** Free a main loop object */
void pa_threaded_mainloop_free(pa_threaded_mainloop* m);

int pa_threaded_mainloop_start(pa_threaded_mainloop *m);
void pa_threaded_mainloop_stop(pa_threaded_mainloop *m);
void pa_threaded_mainloop_lock(pa_threaded_mainloop *m);
void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m);
void pa_threaded_mainloop_signal(pa_threaded_mainloop *m);
void pa_threaded_mainloop_wait(pa_threaded_mainloop *m);

/** Return the return value as specified with the main loop's quit() routine. */
int pa_threaded_mainloop_get_retval(pa_threaded_mainloop *m);

/** Return the abstract main loop abstraction layer vtable for this main loop. */
pa_mainloop_api* pa_threaded_mainloop_get_api(pa_threaded_mainloop*m);

PA_C_DECL_END

#endif
