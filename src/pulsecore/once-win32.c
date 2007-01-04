/* $Id$ */

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>

#include <windows.h>

#include <pulsecore/mutex.h>

#include "once.h"

void pa_once(pa_once_t *control, pa_once_func_t func) {
    HANDLE mutex;
    char name[64];

    assert(control);
    assert(func);

    /* Create the global mutex */
    sprintf(name, "pulse%d", (int)GetCurrentProcessId());

    mutex = CreateMutex(NULL, FALSE, name);
    assert(mutex);

    /* Create the local mutex */
    WaitForSingleObject(mutex, INFINITE);
    if (!control->mutex)
        control->mutex = pa_mutex_new(1);
    ReleaseMutex(mutex);

    CloseHandle(mutex);

    /* Execute function */
    pa_mutex_lock(control->mutex);
    if (!control->once_value) {
        control->once_value = 1;
        func();
    }
    pa_mutex_unlock(control->mutex);

    /* Caveat: We have to make sure that the once func has completed
     * before returning, even if the once func is not actually
     * executed by us. Hence the awkward locking. */
}
