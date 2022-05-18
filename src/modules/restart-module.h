/***
    This file is part of PulseAudio.

    Copyright 2022 Craig Howard

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifndef RESTART_MODULE_H
#define RESTART_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pulse/timeval.h>

#include <pulsecore/core.h>
#include <pulsecore/thread-mq.h>

/* Init and exit callbacks of the module */
typedef int (*init_cb)(pa_module *m);
typedef void (*done_cb)(pa_module *m);
/* Restart data structure */
typedef struct pa_restart_data pa_restart_data;

/* Tears down the module using the done callback and schedules a restart after restart_usec.
 * Returns a handle to the restart event. When the init callback finishes successfully during
 * restart or when the restart should be cancelled, the restart event must be destroyed using
 * pa_restart_free(). */
pa_restart_data *pa_restart_module_reinit(pa_module *m, init_cb do_init, done_cb do_done, pa_usec_t restart_usec);

/* Free the restart event */
void pa_restart_free(pa_restart_data *data);

#ifdef __cplusplus
}
#endif

#endif
