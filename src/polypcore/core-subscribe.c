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

#include <stdio.h>
#include <assert.h>

#include <polypcore/queue.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>

#include "core-subscribe.h"

/* The subscription subsystem may be used to be notified whenever an
 * entity (sink, source, ...) is created or deleted. Modules may
 * register a callback function that is called whenever an event
 * matching a subscription mask happens. The execution of the callback
 * function is postponed to the next main loop iteration, i.e. is not
 * called from within the stack frame the entity was created in. */

struct pa_subscription {
    pa_core *core;
    int dead;
    void (*callback)(pa_core *c, pa_subscription_event_type_t t, uint32_t index, void *userdata);
    void *userdata;
    pa_subscription_mask_t mask;

    pa_subscription *prev, *next;
};

struct pa_subscription_event {
    pa_subscription_event_type_t type;
    uint32_t index;
};

static void sched_event(pa_core *c);

/* Allocate a new subscription object for the given subscription mask. Use the specified callback function and user data */
pa_subscription* pa_subscription_new(pa_core *c, pa_subscription_mask_t m, void (*callback)(pa_core *c, pa_subscription_event_type_t t, uint32_t index, void *userdata), void *userdata) {
    pa_subscription *s;
    assert(c);

    s = pa_xmalloc(sizeof(pa_subscription));
    s->core = c;
    s->dead = 0;
    s->callback = callback;
    s->userdata = userdata;
    s->mask = m;

    if ((s->next = c->subscriptions))
        s->next->prev = s;
    s->prev = NULL;
    c->subscriptions = s;
    return s;
}

/* Free a subscription object, effectively marking it for deletion */
void pa_subscription_free(pa_subscription*s) {
    assert(s && !s->dead);
    s->dead = 1;
    sched_event(s->core);
}

static void free_item(pa_subscription *s) {
    assert(s && s->core);

    if (s->prev)
        s->prev->next = s->next;
    else
        s->core->subscriptions = s->next;
            
    if (s->next)
        s->next->prev = s->prev;
    
    pa_xfree(s);
}

/* Free all subscription objects */
void pa_subscription_free_all(pa_core *c) {
    pa_subscription_event *e;
    assert(c);
    
    while (c->subscriptions)
        free_item(c->subscriptions);

    if (c->subscription_event_queue) {
        while ((e = pa_queue_pop(c->subscription_event_queue)))
            pa_xfree(e);
        
        pa_queue_free(c->subscription_event_queue, NULL, NULL);
        c->subscription_event_queue = NULL;
    }

    if (c->subscription_defer_event) {
        c->mainloop->defer_free(c->subscription_defer_event);
        c->subscription_defer_event = NULL;
    }
}

#if 0
static void dump_event(pa_subscription_event*e) {
    switch (e->type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
        case PA_SUBSCRIPTION_EVENT_SINK:
            pa_log(__FILE__": SINK_EVENT");
            break;
        case PA_SUBSCRIPTION_EVENT_SOURCE:
            pa_log(__FILE__": SOURCE_EVENT");
            break;
        case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
            pa_log(__FILE__": SINK_INPUT_EVENT");
            break;
        case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
            pa_log(__FILE__": SOURCE_OUTPUT_EVENT");
            break;
        case PA_SUBSCRIPTION_EVENT_MODULE:
            pa_log(__FILE__": MODULE_EVENT");
            break;
        case PA_SUBSCRIPTION_EVENT_CLIENT:
            pa_log(__FILE__": CLIENT_EVENT");
            break;
        default:
            pa_log(__FILE__": OTHER");
            break;
    }

    switch (e->type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) {
        case PA_SUBSCRIPTION_EVENT_NEW:
            pa_log(__FILE__":  NEW");
            break;
        case PA_SUBSCRIPTION_EVENT_CHANGE:
            pa_log(__FILE__":  CHANGE");
            break;
        case PA_SUBSCRIPTION_EVENT_REMOVE:
            pa_log(__FILE__":  REMOVE");
            break;
        default:
            pa_log(__FILE__":  OTHER");
            break;
    }

    pa_log(__FILE__":  %u", e->index);
}
#endif

/* Deferred callback for dispatching subscirption events */
static void defer_cb(pa_mainloop_api *m, pa_defer_event *de, void *userdata) {
    pa_core *c = userdata;
    pa_subscription *s;
    assert(c && c->subscription_defer_event == de && c->mainloop == m);

    c->mainloop->defer_enable(c->subscription_defer_event, 0);

    /* Dispatch queued events */
    
    if (c->subscription_event_queue) {
        pa_subscription_event *e;
        
        while ((e = pa_queue_pop(c->subscription_event_queue))) {

            for (s = c->subscriptions; s; s = s->next) {

                if (!s->dead && pa_subscription_match_flags(s->mask, e->type))
                    s->callback(c, e->type, e->index, s->userdata);
            }
            
            pa_xfree(e);
        }
    }

    /* Remove dead subscriptions */
    
    s = c->subscriptions;
    while (s) {
        pa_subscription *n = s->next;
        if (s->dead)
            free_item(s);
        s = n;
    }
}

/* Schedule an mainloop event so that a pending subscription event is dispatched */
static void sched_event(pa_core *c) {
    assert(c);

    if (!c->subscription_defer_event) {
        c->subscription_defer_event = c->mainloop->defer_new(c->mainloop, defer_cb, c);
        assert(c->subscription_defer_event);
    }
        
    c->mainloop->defer_enable(c->subscription_defer_event, 1);
}

/* Append a new subscription event to the subscription event queue and schedule a main loop event */
void pa_subscription_post(pa_core *c, pa_subscription_event_type_t t, uint32_t index) {
    pa_subscription_event *e;
    assert(c);

    e = pa_xmalloc(sizeof(pa_subscription_event));
    e->type = t;
    e->index = index;

    if (!c->subscription_event_queue) {
        c->subscription_event_queue = pa_queue_new();
        assert(c->subscription_event_queue);
    }
    
    pa_queue_push(c->subscription_event_queue, e);
    sched_event(c);
}


