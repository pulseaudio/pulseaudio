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
#include <polypcore/pstream-util.h>
#include <polypcore/gccmacro.h>

void pa_command_subscribe_event(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_subscription_event_type_t e;
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


pa_operation* pa_context_subscribe(pa_context *c, pa_subscription_mask_t m, void (*cb)(pa_context *c, int success, void *userdata), void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback) cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_SUBSCRIBE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, m);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

void pa_context_set_subscribe_callback(pa_context *c, void (*cb)(pa_context *c, pa_subscription_event_type_t t, uint32_t index, void *userdata), void *userdata) {
    assert(c);
    c->subscribe_callback = cb;
    c->subscribe_userdata = userdata;
}
