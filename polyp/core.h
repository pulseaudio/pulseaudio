#ifndef foocorehfoo
#define foocorehfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include "idxset.h"
#include "hashmap.h"
#include "mainloop-api.h"
#include "sample.h"
#include "memblock.h"

struct pa_core {
    struct pa_mainloop_api *mainloop;

    struct pa_idxset *clients, *sinks, *sources, *sink_inputs, *source_outputs, *modules, *scache;

    struct pa_hashmap *namereg, *autoload_hashmap;

    char *default_source_name, *default_sink_name;

    struct pa_sample_spec default_sample_spec;
    int auto_unload_time;
    struct pa_time_event *auto_unload_event;

    struct pa_defer_event *subscription_defer_event;
    struct pa_queue *subscription_event_queue;
    struct pa_subscription *subscriptions;

    struct pa_memblock_stat *memblock_stat;

    int disallow_module_loading;
    int quit_after_last_client_time;

    struct pa_time_event *quit_event;
};

struct pa_core* pa_core_new(struct pa_mainloop_api *m);
void pa_core_free(struct pa_core*c);
void pa_core_check_quit(struct pa_core *c);

#endif
