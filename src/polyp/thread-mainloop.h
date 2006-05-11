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
 * A thread based event loop implementation based on pa_mainloop. The
 * event loop is run in a helper thread in the background. A few
 * synchronization primitives are available to access the objects
 * attached to the event loop safely. */

/** An opaque threaded main loop object */
typedef struct pa_threaded_mainloop pa_threaded_mainloop;

/** Allocate a new threaded main loop object. You have to call
 * pa_threaded_mainloop_start() before the event loop thread starts
 * running. */
pa_threaded_mainloop *pa_threaded_mainloop_new(void);

/** Free a threaded main loop object. If the event loop thread is
 * still running, it is terminated using pa_threaded_mainloop_stop()
 * first. */
void pa_threaded_mainloop_free(pa_threaded_mainloop* m);

/** Start the event loop thread. */
int pa_threaded_mainloop_start(pa_threaded_mainloop *m);

/** Terminate the event loop thread cleanly. Make sure to unlock the
 * mainloop object before calling this function. */
void pa_threaded_mainloop_stop(pa_threaded_mainloop *m);

/** Lock the event loop object, effectively blocking the event loop
 * thread from processing events. You can use this to enforce
 * exclusive access to all objects attached to the event loop. This
 * lock is recursive. This function may not be called inside the event
 * loop thread. Events that are dispatched from the event loop thread
 * are executed with this lock held. */
void pa_threaded_mainloop_lock(pa_threaded_mainloop *m);

/** Unlock the event loop object, inverse of pa_threaded_mainloop_lock() */
void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m);

/** Wait for an event to be signalled by the event loop thread. You
 * can use this to pass data from the event loop thread to the main
 * thread in synchronized fashion. This function may not be called
 * inside the event loop thread. Prior to this call the event loop
 * object needs to be locked using pa_threaded_mainloop_lock(). While
 * waiting the lock will be released, immediately before returning it
 * will be acquired again. */
void pa_threaded_mainloop_wait(pa_threaded_mainloop *m);

/** Signal all threads waiting for a signalling event in
 * pa_threaded_mainloop_wait(). If wait_for_release is non-zero, do
 * not return before the signal was accepted by a
 * pa_threaded_mainloop_accept() call. While waiting for that condition
 * the event loop object is unlocked. */
void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_accept);

/** Accept a signal from the event thread issued with
 * pa_threaded_mainloop_signal(). This call should only be used in
 * conjunction with pa_threaded_mainloop_signal() with a non-zero
 * wait_for_accept value.  */
void pa_threaded_mainloop_accept(pa_threaded_mainloop *m);

/** Return the return value as specified with the main loop's quit() routine. */
int pa_threaded_mainloop_get_retval(pa_threaded_mainloop *m);

/** Return the abstract main loop abstraction layer vtable for this main loop. */
pa_mainloop_api* pa_threaded_mainloop_get_api(pa_threaded_mainloop*m);

PA_C_DECL_END

#endif
