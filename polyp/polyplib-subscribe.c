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

#include <assert.h>
#include <stdio.h>

#include "polyplib-subscribe.h"
#include "polyplib-internal.h"
#include "pstream-util.h"

void pa_command_subscribe_event(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    enum pa_subscription_event_type e;
    uint32_t index;
    assert(pd && command == PA_COMMAND_SUBSCRIBE_EVENT && t && c);

    pa_context_ref(c);

    if (pa_tagstruct_getu32(t, &e) < 0 ||
        pa_tagstruct_getu32(t, &index) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERROR_PROTOCOL);
        goto finish;
    }

    if (c->subscribe_callback)
        c->subscribe_callback(c, e, index, c->subscribe_userdata);

finish:
    pa_context_unref(c);
}


struct pa_operation* pa_context_subscribe(struct pa_context *c, enum pa_subscription_mask m, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata) {
    struct pa_operation *o;
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(c);

    o = pa_operation_new(c, NULL);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SUBSCRIBE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, m);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

void pa_context_set_subscribe_callback(struct pa_context *c, void (*cb)(struct pa_context *c, enum pa_subscription_event_type t, uint32_t index, void *userdata), void *userdata) {
    assert(c);
    c->subscribe_callback = cb;
    c->subscribe_userdata = userdata;
}
