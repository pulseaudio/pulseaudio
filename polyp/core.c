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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>

#include "core.h"
#include "module.h"
#include "sink.h"
#include "source.h"
#include "namereg.h"
#include "util.h"
#include "scache.h"
#include "autoload.h"
#include "xmalloc.h"
#include "subscribe.h"
#include "props.h"

struct pa_core* pa_core_new(struct pa_mainloop_api *m) {
    struct pa_core* c;
    c = pa_xmalloc(sizeof(struct pa_core));

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

    c->default_sample_spec.format = PA_SAMPLE_S16NE;
    c->default_sample_spec.rate = 44100;
    c->default_sample_spec.channels = 2;

    c->module_auto_unload_event = NULL;
    c->module_defer_unload_event = NULL;
    c->scache_auto_unload_event = NULL;

    c->subscription_defer_event = NULL;
    c->subscription_event_queue = NULL;
    c->subscriptions = NULL;

    c->memblock_stat = pa_memblock_stat_new();

    c->disallow_module_loading = 0;

    c->quit_event = NULL;

    c->exit_idle_time = -1;
    c->module_idle_time = 20;
    c->scache_idle_time = 20;

    c->resample_method = PA_RESAMPLER_SRC_SINC_FASTEST;

    pa_property_init(c);
    
    pa_check_signal_is_blocked(SIGPIPE);
    
    return c;
}

void pa_core_free(struct pa_core *c) {
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
    
    pa_xfree(c);    
}

static void quit_callback(struct pa_mainloop_api*m, struct pa_time_event *e, const struct timeval *tv, void *userdata) {
    struct pa_core *c = userdata;
    assert(c->quit_event = e);

    m->quit(m, 0);
}

void pa_core_check_quit(struct pa_core *c) {
    assert(c);

    if (!c->quit_event && c->exit_idle_time >= 0 && pa_idxset_ncontents(c->clients) == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        tv.tv_sec+= c->exit_idle_time;
        c->quit_event = c->mainloop->time_new(c->mainloop, &tv, quit_callback, c);
    } else if (c->quit_event && pa_idxset_ncontents(c->clients) > 0) {
        c->mainloop->time_free(c->quit_event);
        c->quit_event = NULL;
    }
}

