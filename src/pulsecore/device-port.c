/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB
  Copyright 2011 David Henningsson, Canonical Ltd.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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

#include "device-port.h"
#include <pulsecore/card.h>

PA_DEFINE_PUBLIC_CLASS(pa_device_port, pa_object);

void pa_device_port_set_available(pa_device_port *p, pa_available_t status)
{
    pa_core *core;

    pa_assert(p);

    if (p->available == status)
        return;

/*    pa_assert(status != PA_AVAILABLE_UNKNOWN); */

    p->available = status;
    pa_log_debug("Setting port %s to status %s", p->name, status == PA_AVAILABLE_YES ? "yes" :
       status == PA_AVAILABLE_NO ? "no" : "unknown");

    /* Post subscriptions to the card which owns us */
    pa_assert_se(core = p->core);
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE, p->card->index);

    pa_hook_fire(&core->hooks[PA_CORE_HOOK_PORT_AVAILABLE_CHANGED], p);
}

static void device_port_free(pa_object *o) {
    pa_device_port *p = PA_DEVICE_PORT(o);

    pa_assert(p);
    pa_assert(pa_device_port_refcnt(p) == 0);

    if (p->proplist)
        pa_proplist_free(p->proplist);

    if (p->profiles)
        pa_hashmap_free(p->profiles, NULL);

    pa_xfree(p->name);
    pa_xfree(p->description);
    pa_xfree(p);
}


pa_device_port *pa_device_port_new(pa_core *c, const char *name, const char *description, size_t extra) {
    pa_device_port *p;

    pa_assert(name);

    p = PA_DEVICE_PORT(pa_object_new_internal(PA_ALIGN(sizeof(pa_device_port)) + extra, pa_device_port_type_id, pa_device_port_check_type));
    p->parent.free = device_port_free;

    p->name = pa_xstrdup(name);
    p->description = pa_xstrdup(description);
    p->core = c;
    p->card = NULL;
    p->priority = 0;
    p->available = PA_AVAILABLE_UNKNOWN;
    p->profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    p->is_input = FALSE;
    p->is_output = FALSE;
    p->latency_offset = 0;
    p->proplist = pa_proplist_new();

    return p;
}

void pa_device_port_set_latency_offset(pa_device_port *p, int64_t offset) {
    uint32_t state;
    pa_core *core;

    pa_assert(p);

    if (offset == p->latency_offset)
        return;

    p->latency_offset = offset;

    if (p->is_output) {
        pa_sink *sink;

        PA_IDXSET_FOREACH(sink, p->core->sinks, state)
            if (sink->active_port == p) {
                pa_sink_set_latency_offset(sink, p->latency_offset);
                break;
            }

    } else {
        pa_source *source;

        PA_IDXSET_FOREACH(source, p->core->sources, state)
            if (source->active_port == p) {
                pa_source_set_latency_offset(source, p->latency_offset);
                break;
            }
    }

    pa_assert_se(core = p->core);
    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE, p->card->index);
    pa_hook_fire(&core->hooks[PA_CORE_HOOK_PORT_LATENCY_OFFSET_CHANGED], p);
}
