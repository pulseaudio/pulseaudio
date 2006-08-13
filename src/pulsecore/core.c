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

#include <stdlib.h>
#include <assert.h>
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
#include <pulsecore/props.h>
#include <pulsecore/random.h>

#include "core.h"

pa_core* pa_core_new(pa_mainloop_api *m) {
    pa_core* c;
    
    c = pa_xnew(pa_core, 1);

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
    c->running_as_daemon = 0;

    c->default_sample_spec.format = PA_SAMPLE_S16NE;
    c->default_sample_spec.rate = 44100;
    c->default_sample_spec.channels = 2;

    c->module_auto_unload_event = NULL;
    c->module_defer_unload_event = NULL;
    c->scache_auto_unload_event = NULL;

    c->subscription_defer_event = NULL;
    PA_LLIST_HEAD_INIT(pa_subscription, c->subscriptions);
    PA_LLIST_HEAD_INIT(pa_subscription_event, c->subscription_event_queue);
    c->subscription_event_last = NULL;

    c->memblock_stat = pa_memblock_stat_new();

    c->disallow_module_loading = 0;

    c->quit_event = NULL;

    c->exit_idle_time = -1;
    c->module_idle_time = 20;
    c->scache_idle_time = 20;

    c->resample_method = PA_RESAMPLER_SRC_SINC_FASTEST;

    c->is_system_instance = 0;

    pa_hook_init(&c->hook_sink_input_new, c);
    pa_hook_init(&c->hook_sink_disconnect, c);
    pa_hook_init(&c->hook_source_output_new, c);
    pa_hook_init(&c->hook_source_disconnect, c);

    pa_property_init(c);

    pa_random(&c->cookie, sizeof(c->cookie));
    
#ifdef SIGPIPE
    pa_check_signal_is_blocked(SIGPIPE);
#endif
    return c;
}

void pa_core_free(pa_core *c) {
    assert(c);

    pa_module_unload_all(c);
    assert(!c->modules);

    assert(pa_idxset_isempty(c->clients));
    pa_idxset_free(c->clients, NULL, NULL);
    
    assert(pa_idxset_isempty(c->sinks));
    pa_idxset_free(c->sinks, NULL, NULL);

    assert(pa_idxset_isempty(c->sources));
    pa_idxset_free(c->sources, NULL, NULL);
    
    assert(pa_idxset_isempty(c->source_outputs));
    pa_idxset_free(c->source_outputs, NULL, NULL);
    
    assert(pa_idxset_isempty(c->sink_inputs));
    pa_idxset_free(c->sink_inputs, NULL, NULL);

    pa_scache_free(c);
    pa_namereg_free(c);
    pa_autoload_free(c);
    pa_subscription_free_all(c);

    if (c->quit_event)
        c->mainloop->time_free(c->quit_event);

    pa_xfree(c->default_source_name);
    pa_xfree(c->default_sink_name);

    pa_memblock_stat_unref(c->memblock_stat);

    pa_property_cleanup(c);

    pa_hook_free(&c->hook_sink_input_new);
    pa_hook_free(&c->hook_sink_disconnect);
    pa_hook_free(&c->hook_source_output_new);
    pa_hook_free(&c->hook_source_disconnect);
    
    pa_xfree(c);    
}

static void quit_callback(pa_mainloop_api*m, pa_time_event *e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    pa_core *c = userdata;
    assert(c->quit_event = e);

    m->quit(m, 0);
}

void pa_core_check_quit(pa_core *c) {
    assert(c);

    if (!c->quit_event && c->exit_idle_time >= 0 && pa_idxset_size(c->clients) == 0) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_sec+= c->exit_idle_time;
        c->quit_event = c->mainloop->time_new(c->mainloop, &tv, quit_callback, c);
    } else if (c->quit_event && pa_idxset_size(c->clients) > 0) {
        c->mainloop->time_free(c->quit_event);
        c->quit_event = NULL;
    }
}

