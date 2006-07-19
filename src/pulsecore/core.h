#ifndef foocorehfoo
#define foocorehfoo

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

typedef struct pa_core pa_core;

#include <pulsecore/idxset.h>
#include <pulsecore/hashmap.h>
#include <pulse/mainloop-api.h>
#include <pulse/sample.h>
#include <pulsecore/memblock.h>
#include <pulsecore/resampler.h>
#include <pulsecore/queue.h>
#include <pulsecore/core-subscribe.h>

/* The core structure of PulseAudio. Every PulseAudio daemon contains
 * exactly one of these. It is used for storing kind of global
 * variables for the daemon. */

struct pa_core {
    /* A random value which may be used to identify this instance of
     * PulseAudio. Not cryptographically secure in any way. */
    uint32_t cookie;
    
    pa_mainloop_api *mainloop;

    /* idxset of all kinds of entities */
    pa_idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules, *scache, *autoload_idxset;

    /* Some hashmaps for all sorts of entities */
    pa_hashmap *namereg, *autoload_hashmap, *properties;

    /* The name of the default sink/source */
    char *default_source_name, *default_sink_name;

    pa_sample_spec default_sample_spec;
    pa_time_event *module_auto_unload_event;
    pa_defer_event *module_defer_unload_event;

    pa_defer_event *subscription_defer_event;
    pa_queue *subscription_event_queue;
    pa_subscription *subscriptions;

    pa_memblock_stat *memblock_stat;

    int disallow_module_loading, running_as_daemon;
    int exit_idle_time, module_idle_time, scache_idle_time;

    pa_time_event *quit_event;

    pa_time_event *scache_auto_unload_event;

    pa_resample_method_t resample_method;

    int is_system_instance;
};

pa_core* pa_core_new(pa_mainloop_api *m);
void pa_core_free(pa_core*c);

/* Check whether noone is connected to this core */
void pa_core_check_quit(pa_core *c);

#endif
