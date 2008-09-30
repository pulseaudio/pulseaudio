/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/autoload.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/shared.h>
#include <pulsecore/random.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "core.h"

static PA_DEFINE_CHECK_TYPE(pa_core, pa_msgobject);

static int core_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_core *c = PA_CORE(o);

    pa_core_assert_ref(c);

    switch (code) {

        case PA_CORE_MESSAGE_UNLOAD_MODULE:
            pa_module_unload(c, userdata, TRUE);
            return 0;

        default:
            return -1;
    }
}

static void core_free(pa_object *o);

pa_core* pa_core_new(pa_mainloop_api *m, pa_bool_t shared, size_t shm_size) {
    pa_core* c;
    pa_mempool *pool;
    int j;

    pa_assert(m);

    if (shared) {
        if (!(pool = pa_mempool_new(shared, shm_size))) {
            pa_log_warn("failed to allocate shared memory pool. Falling back to a normal memory pool.");
            shared = FALSE;
        }
    }

    if (!shared) {
        if (!(pool = pa_mempool_new(shared, shm_size))) {
            pa_log("pa_mempool_new() failed.");
            return NULL;
        }
    }

    c = pa_msgobject_new(pa_core);
    c->parent.parent.free = core_free;
    c->parent.process_msg = core_process_msg;

    c->mainloop = m;
    c->clients = pa_idxset_new(NULL, NULL);
    c->sinks = pa_idxset_new(NULL, NULL);
    c->sources = pa_idxset_new(NULL, NULL);
    c->source_outputs = pa_idxset_new(NULL, NULL);
    c->sink_inputs = pa_idxset_new(NULL, NULL);

    c->default_source_name = c->default_sink_name = NULL;

    c->modules = NULL;
    c->namereg = NULL;
    c->scache = NULL;
    c->autoload_idxset = NULL;
    c->autoload_hashmap = NULL;
    c->running_as_daemon = FALSE;

    c->default_sample_spec.format = PA_SAMPLE_S16NE;
    c->default_sample_spec.rate = 44100;
    c->default_sample_spec.channels = 2;
    c->default_n_fragments = 4;
    c->default_fragment_size_msec = 25;

    c->module_auto_unload_event = NULL;
    c->module_defer_unload_event = NULL;
    c->scache_auto_unload_event = NULL;

    c->subscription_defer_event = NULL;
    PA_LLIST_HEAD_INIT(pa_subscription, c->subscriptions);
    PA_LLIST_HEAD_INIT(pa_subscription_event, c->subscription_event_queue);
    c->subscription_event_last = NULL;

    c->mempool = pool;
    pa_silence_cache_init(&c->silence_cache);

    c->exit_event = NULL;

    c->exit_idle_time = -1;
    c->module_idle_time = 20;
    c->scache_idle_time = 20;

    c->resample_method = PA_RESAMPLER_SPEEX_FLOAT_BASE + 3;

    c->disallow_module_loading = FALSE;
    c->disallow_exit = FALSE;
    c->realtime_scheduling = FALSE;
    c->realtime_priority = 5;
    c->disable_remixing = FALSE;
    c->disable_lfe_remixing = FALSE;

    for (j = 0; j < PA_CORE_HOOK_MAX; j++)
        pa_hook_init(&c->hooks[j], c);

    pa_shared_init(c);

    pa_random(&c->cookie, sizeof(c->cookie));

#ifdef SIGPIPE
    pa_check_signal_is_blocked(SIGPIPE);
#endif

    pa_core_check_idle(c);

    return c;
}

static void core_free(pa_object *o) {
    pa_core *c = PA_CORE(o);
    int j;
    pa_assert(c);

    pa_module_unload_all(c);
    pa_assert(!c->modules);

    pa_assert(pa_idxset_isempty(c->clients));
    pa_idxset_free(c->clients, NULL, NULL);

    pa_assert(pa_idxset_isempty(c->sinks));
    pa_idxset_free(c->sinks, NULL, NULL);

    pa_assert(pa_idxset_isempty(c->sources));
    pa_idxset_free(c->sources, NULL, NULL);

    pa_assert(pa_idxset_isempty(c->source_outputs));
    pa_idxset_free(c->source_outputs, NULL, NULL);

    pa_assert(pa_idxset_isempty(c->sink_inputs));
    pa_idxset_free(c->sink_inputs, NULL, NULL);

    pa_scache_free(c);
    pa_namereg_free(c);
    pa_autoload_free(c);
    pa_subscription_free_all(c);

    if (c->exit_event)
        c->mainloop->time_free(c->exit_event);

    pa_xfree(c->default_source_name);
    pa_xfree(c->default_sink_name);

    pa_silence_cache_done(&c->silence_cache);
    pa_mempool_free(c->mempool);

    pa_shared_cleanup(c);

    for (j = 0; j < PA_CORE_HOOK_MAX; j++)
        pa_hook_done(&c->hooks[j]);

    pa_xfree(c);
}

static void exit_callback(pa_mainloop_api*m, pa_time_event *e, const struct timeval *tv, void *userdata) {
    pa_core *c = userdata;
    pa_assert(c->exit_event == e);

    pa_log_info("We are idle, quitting...");
    pa_core_exit(c, TRUE, 0);
}

void pa_core_check_idle(pa_core *c) {
    pa_assert(c);

    if (!c->exit_event &&
        c->exit_idle_time >= 0 &&
        pa_idxset_size(c->clients) == 0) {

        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec+= c->exit_idle_time;

        c->exit_event = c->mainloop->time_new(c->mainloop, &tv, exit_callback, c);

    } else if (c->exit_event && pa_idxset_size(c->clients) > 0) {
        c->mainloop->time_free(c->exit_event);
        c->exit_event = NULL;
    }
}

int pa_core_exit(pa_core *c, pa_bool_t force, int retval) {
    pa_assert(c);

    if (c->disallow_exit && !force)
        return -1;

    c->mainloop->quit(c->mainloop, retval);
    return 0;
}
