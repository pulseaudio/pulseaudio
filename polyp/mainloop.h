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

/** \struct pa_mainloop
 * An opaque main loop object
 */
struct pa_mainloop;

/** Allocate a new main loop object */
struct pa_mainloop *pa_mainloop_new(void);

/** Free a main loop object */
void pa_mainloop_free(struct pa_mainloop* m);

/** Run a single iteration of the main loop. Returns a negative value
on error or exit request. If block is nonzero, block for events if
none are queued. Optionally return the return value as specified with
the main loop's quit() routine in the integer variable retval points
to. On success returns the number of source dispatched in this iteration. */
int pa_mainloop_iterate(struct pa_mainloop *m, int block, int *retval);

/** Run unlimited iterations of the main loop object until the main loop's quit() routine is called. */
int pa_mainloop_run(struct pa_mainloop *m, int *retval);

/** Return the abstract main loop abstraction layer vtable for this main loop. This calls pa_mainloop_iterate() iteratively.*/
struct pa_mainloop_api* pa_mainloop_get_api(struct pa_mainloop*m);

/** Return non-zero when there are any deferred events pending. \since 0.5 */
int pa_mainloop_deferred_pending(struct pa_mainloop *m);

PA_C_DECL_END

#endif
