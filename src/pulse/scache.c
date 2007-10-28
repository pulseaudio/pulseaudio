/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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
#include <string.h>

#include <pulsecore/pstream-util.h>
#include <pulsecore/macro.h>

#include "internal.h"

#include "scache.h"

int pa_stream_connect_upload(pa_stream *s, size_t length) {
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, length > 0, PA_ERR_INVALID);

    pa_stream_ref(s);

    s->direction = PA_STREAM_UPLOAD;

    t = pa_tagstruct_command(s->context, PA_COMMAND_CREATE_UPLOAD_STREAM, &tag);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_put_sample_spec(t, &s->sample_spec);
    pa_tagstruct_put_channel_map(t, &s->channel_map);
    pa_tagstruct_putu32(t, length);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_create_stream_callback, s, NULL);

    pa_stream_set_state(s, PA_STREAM_CREATING);

    pa_stream_unref(s);
    return 0;
}

int pa_stream_finish_upload(pa_stream *s) {
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->channel_valid, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->context->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    pa_stream_ref(s);

    t = pa_tagstruct_command(s->context, PA_COMMAND_FINISH_UPLOAD_STREAM, &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_disconnect_callback, s, NULL);

    pa_stream_unref(s);
    return 0;
}

pa_operation *pa_context_play_sample(pa_context *c, const char *name, const char *dev, pa_volume_t volume, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name && *name, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !dev || *dev, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    if (!dev)
        dev = c->conf->default_sink;

    t = pa_tagstruct_command(c, PA_COMMAND_PLAY_SAMPLE, &tag);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, dev);
    pa_tagstruct_putu32(t, volume);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_context_remove_sample(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(c, name && *name, PA_ERR_INVALID);

    o = pa_operation_new(c, NULL, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(c, PA_COMMAND_REMOVE_SAMPLE, &tag);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(c->pstream, t);
    pa_pdispatch_register_reply(c->pdispatch, tag, DEFAULT_TIMEOUT, pa_context_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

