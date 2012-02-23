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

void pa_device_port_set_available(pa_device_port *p, pa_port_available_t status)
{
    uint32_t state;
    pa_card *card;
/*    pa_source *source;
    pa_sink *sink; */
    pa_core *core;

    pa_assert(p);

    if (p->available == status)
        return;

/*    pa_assert(status != PA_PORT_AVAILABLE_UNKNOWN); */

    p->available = status;
    pa_log_debug("Setting port %s to status %s", p->name, status == PA_PORT_AVAILABLE_YES ? "yes" :
       status == PA_PORT_AVAILABLE_NO ? "no" : "unknown");

    /* Post subscriptions to the card which owns us */
    pa_assert_se(core = p->core);
    PA_IDXSET_FOREACH(card, core->cards, state)
        if (p == pa_hashmap_get(card->ports, p->name))
            pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE, card->index);
#if 0
/* This stuff is temporarily commented out while figuring out whether to actually do this */
    if (p->is_output)
        PA_IDXSET_FOREACH(sink, core->sinks, state)
            if (p == pa_hashmap_get(sink->ports, p->name))
                pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, sink->index);
    if (p->is_input)
        PA_IDXSET_FOREACH(source, core->sources, state)
            if (p == pa_hashmap_get(source->ports, p->name))
                pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, source->index);
#endif

    pa_hook_fire(&core->hooks[PA_CORE_HOOK_PORT_AVAILABLE_CHANGED], p);
}

static void device_port_free(pa_object *o) {
    pa_device_port *p = PA_DEVICE_PORT(o);

    pa_assert(p);
    pa_assert(pa_device_port_refcnt(p) == 0);

    if (p->proplist)
        pa_proplist_free(p->proplist);
    if (p->profiles)
        pa_hashmap_free(p->profiles, NULL, NULL);
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
    p->priority = 0;
    p->available = PA_PORT_AVAILABLE_UNKNOWN;
    p->profiles = NULL;
    p->is_input = FALSE;
    p->is_output = FALSE;
    p->proplist = pa_proplist_new();

    return p;
}

void pa_device_port_hashmap_free(pa_hashmap *h) {
    pa_device_port *p;

    pa_assert(h);

    while ((p = pa_hashmap_steal_first(h)))
        pa_device_port_unref(p);

    pa_hashmap_free(h, NULL, NULL);
}
