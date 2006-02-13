#ifndef foomainloophfoo
#define foomainloophfoo

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

#include "mainloop-api.h"
#include "cdecl.h"

PA_C_DECL_BEGIN

/** \file
 * 
 * A minimal main loop implementation based on the C library's poll()
 * function. Using the routines defined herein you may create a simple
 * main loop supporting the generic main loop abstraction layer as
 * defined in \ref mainloop-api.h. This implementation is thread safe
 * as long as you access the main loop object from a single thread only.*/

/** \pa_mainloop
 * An opaque main loop object
 */
typedef struct pa_mainloop pa_mainloop;

/** Allocate a new main loop object */
pa_mainloop *pa_mainloop_new(void);

/** Free a main loop object */
void pa_mainloop_free(pa_mainloop* m);


/** Prepare for a single iteration of the main loop. Returns a negative value
on error or exit request. timeout specifies a maximum timeout for the subsequent
poll, or -1 for blocking behaviour. Defer events are also dispatched when this
function is called. On success returns the number of source dispatched in this
iteration.*/
int pa_mainloop_prepare(pa_mainloop *m, int timeout);
/** Execute the previously prepared poll. Returns a negative value on error.*/
int pa_mainloop_poll(pa_mainloop *m);
/** Dispatch timeout and io events from the previously executed poll. Returns
a negative value on error. On success returns the number of source dispatched. */
int pa_mainloop_dispatch(pa_mainloop *m);

/** Return the return value as specified with the main loop's quit() routine. */
int pa_mainloop_get_retval(pa_mainloop *m);

/** Run a single iteration of the main loop. This is a convenience function
for pa_mainloop_prepare(), pa_mainloop_poll() and pa_mainloop_dispatch().
Returns a negative value on error or exit request. If block is nonzero,
block for events if none are queued. Optionally return the return value as
specified with the main loop's quit() routine in the integer variable retval points
to. On success returns the number of source dispatched in this iteration. */
int pa_mainloop_iterate(pa_mainloop *m, int block, int *retval);

/** Run unlimited iterations of the main loop object until the main loop's quit() routine is called. */
int pa_mainloop_run(pa_mainloop *m, int *retval);

/** Return the abstract main loop abstraction layer vtable for this main loop. This calls pa_mainloop_iterate() iteratively.*/
pa_mainloop_api* pa_mainloop_get_api(pa_mainloop*m);

/** Return non-zero when there are any deferred events pending. \since 0.5 */
int pa_mainloop_deferred_pending(pa_mainloop *m);

/** Shutdown the main loop */
void pa_mainloop_quit(pa_mainloop *m, int r);

/** Interrupt a running poll (for threaded systems) */
void pa_mainloop_wakeup(pa_mainloop *m);

PA_C_DECL_END

#endif
