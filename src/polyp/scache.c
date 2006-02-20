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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <polypcore/pstream-util.h>

#include "internal.h"

#include "scache.h"

void pa_stream_connect_upload(pa_stream *s, size_t length) {
    pa_tagstruct *t;
    uint32_t tag;
    
    assert(s && length);

    pa_stream_ref(s);
    
    s->state = PA_STREAM_CREATING;
    s->direction = PA_STREAM_UPLOAD;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_CREATE_UPLOAD_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_put_sample_spec(t, &s->sample_spec);
    pa_tagstruct_putu32(t, length);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_create_stream_callback, s);

    pa_stream_unref(s);
}

void pa_stream_finish_upload(pa_stream *s) {
    pa_tagstruct *t;
    uint32_t tag;
    assert(s);

    if (!s->channel_valid || !s->context->state == PA_CONTEXT_READY)
        return;

    pa_stream_ref(s);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    
    pa_tagstruct_putu32(t, PA_COMMAND_FINISH_UPLOAD_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_disconnect_callback, s);

    pa_stream_unref(s);
}

pa_operation * pa_context_play_sample(pa_context *c, const char *name, const char *dev, uint32_t volume, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && name && *name && (!dev || *dev));

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback_t) cb;
    o->userdata = userdata;

    if (!dev)
        dev = c->conf->default_sink;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_PLAY_SAMPLE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_putu32(t, (uint32_t) -1);
    pa_tagstruct_puts(t, dev);
    pa_tagstruct_putu32(t, volume);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_context_remove_sample(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    assert(c && name);

    o = pa_operation_new(c, NULL);
    o->callback = (pa_operation_callback_t) cb;
    o->userdata = userdata;
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_REMOVE_SAMPLE);
    pa_tagstruct_putu32(t, tag = c->ctag++);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, o);

    return pa_operation_ref(o);
}

