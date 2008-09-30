#ifndef foocorehfoo
#define foocorehfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <pulse/mainloop-api.h>
#include <pulse/sample.h>

#include <pulsecore/idxset.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/memblock.h>
#include <pulsecore/resampler.h>
#include <pulsecore/queue.h>
#include <pulsecore/llist.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/asyncmsgq.h>
#include <pulsecore/sample-util.h>

typedef struct pa_core pa_core;

#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/msgobject.h>

typedef enum pa_core_hook {
    PA_CORE_HOOK_SINK_NEW,
    PA_CORE_HOOK_SINK_FIXATE,
    PA_CORE_HOOK_SINK_PUT,
    PA_CORE_HOOK_SINK_UNLINK,
    PA_CORE_HOOK_SINK_UNLINK_POST,
    PA_CORE_HOOK_SINK_STATE_CHANGED,
    PA_CORE_HOOK_SINK_PROPLIST_CHANGED,
    PA_CORE_HOOK_SOURCE_NEW,
    PA_CORE_HOOK_SOURCE_FIXATE,
    PA_CORE_HOOK_SOURCE_PUT,
    PA_CORE_HOOK_SOURCE_UNLINK,
    PA_CORE_HOOK_SOURCE_UNLINK_POST,
    PA_CORE_HOOK_SOURCE_STATE_CHANGED,
    PA_CORE_HOOK_SOURCE_PROPLIST_CHANGED,
    PA_CORE_HOOK_SINK_INPUT_NEW,
    PA_CORE_HOOK_SINK_INPUT_FIXATE,
    PA_CORE_HOOK_SINK_INPUT_PUT,
    PA_CORE_HOOK_SINK_INPUT_UNLINK,
    PA_CORE_HOOK_SINK_INPUT_UNLINK_POST,
    PA_CORE_HOOK_SINK_INPUT_MOVE,
    PA_CORE_HOOK_SINK_INPUT_MOVE_POST,
    PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED,
    PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED,
    PA_CORE_HOOK_SOURCE_OUTPUT_NEW,
    PA_CORE_HOOK_SOURCE_OUTPUT_FIXATE,
    PA_CORE_HOOK_SOURCE_OUTPUT_PUT,
    PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK,
    PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST,
    PA_CORE_HOOK_SOURCE_OUTPUT_MOVE,
    PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_POST,
    PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED,
    PA_CORE_HOOK_SOURCE_OUTPUT_PROPLIST_CHANGED,
    PA_CORE_HOOK_MAX
} pa_core_hook_t;

/* The core structure of PulseAudio. Every PulseAudio daemon contains
 * exactly one of these. It is used for storing kind of global
 * variables for the daemon. */

struct pa_core {
    pa_msgobject parent;

    /* A random value which may be used to identify this instance of
     * PulseAudio. Not cryptographically secure in any way. */
    uint32_t cookie;

    pa_mainloop_api *mainloop;

    /* idxset of all kinds of entities */
    pa_idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules, *scache, *autoload_idxset;

    /* Some hashmaps for all sorts of entities */
    pa_hashmap *namereg, *autoload_hashmap, *shared;

    /* The name of the default sink/source */
    char *default_source_name, *default_sink_name;

    pa_sample_spec default_sample_spec;
    unsigned default_n_fragments, default_fragment_size_msec;

    pa_time_event *module_auto_unload_event;
    pa_defer_event *module_defer_unload_event;

    pa_defer_event *subscription_defer_event;
    PA_LLIST_HEAD(pa_subscription, subscriptions);
    PA_LLIST_HEAD(pa_subscription_event, subscription_event_queue);
    pa_subscription_event *subscription_event_last;

    pa_mempool *mempool;
    pa_silence_cache silence_cache;

    int exit_idle_time, module_idle_time, scache_idle_time;

    pa_time_event *exit_event;

    pa_time_event *scache_auto_unload_event;

    pa_bool_t disallow_module_loading:1;
    pa_bool_t disallow_exit:1;
    pa_bool_t running_as_daemon:1;
    pa_bool_t realtime_scheduling:1;
    pa_bool_t disable_remixing:1;
    pa_bool_t disable_lfe_remixing:1;

    pa_resample_method_t resample_method;
    int realtime_priority;

    /* hooks */
    pa_hook hooks[PA_CORE_HOOK_MAX];
};

PA_DECLARE_CLASS(pa_core);
#define PA_CORE(o) pa_core_cast(o)

enum {
    PA_CORE_MESSAGE_UNLOAD_MODULE,
    PA_CORE_MESSAGE_MAX
};

pa_core* pa_core_new(pa_mainloop_api *m, pa_bool_t shared, size_t shm_size);

/* Check whether noone is connected to this core */
void pa_core_check_idle(pa_core *c);

int pa_core_exit(pa_core *c, pa_bool_t force, int retval);

#endif
