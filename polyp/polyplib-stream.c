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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "polyplib-internal.h"
#include "xmalloc.h"
#include "pstream-util.h"
#include "util.h"

struct pa_stream *pa_stream_new(struct pa_context *c, const char *name, const struct pa_sample_spec *ss) {
    struct pa_stream *s;
    assert(c && ss);

    s = pa_xmalloc(sizeof(struct pa_stream));
    s->ref = 1;
    s->context = c;

    s->read_callback = NULL;
    s->read_userdata = NULL;
    s->write_callback = NULL;
    s->write_userdata = NULL;
    s->state_callback = NULL;
    s->state_userdata = NULL;

    s->state = PA_STREAM_NODIRECTION;
    s->name = pa_xstrdup(name);
    s->sample_spec = *ss;
    s->channel = 0;
    s->channel_valid = 0;
    s->device_index = PA_INVALID_INDEX;
    s->requested_bytes = 0;
    s->state = PA_STREAM_DISCONNECTED;
    memset(&s->buffer_attr, 0, sizeof(s->buffer_attr));

    PA_LLIST_PREPEND(struct pa_stream, c->streams, s);

    return pa_stream_ref(s);
}

static void stream_free(struct pa_stream *s) {
    assert(s);
    pa_xfree(s->name);
    pa_xfree(s);
}

void pa_stream_unref(struct pa_stream *s) {
    assert(s && s->ref >= 1);

    if (--(s->ref) == 0)
        stream_free(s);
}

struct pa_stream* pa_stream_ref(struct pa_stream *s) {
    assert(s && s->ref >= 1);
    s->ref++;
    return s;
}

enum pa_stream_state pa_stream_get_state(struct pa_stream *s) {
    assert(s && s->ref >= 1);
    return s->state;
}

struct pa_context* pa_stream_get_context(struct pa_stream *s) {
    assert(s && s->ref >= 1);
    return s->context;
}

uint32_t pa_stream_get_index(struct pa_stream *s) {
    assert(s && s->ref >= 1);
    return s->device_index;
}
    
void pa_stream_set_state(struct pa_stream *s, enum pa_stream_state st) {
    assert(s && s->ref >= 1);

    if (s->state == st)
        return;
    
    pa_stream_ref(s);

    s->state = st;
    
    if ((st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) && s->context) {
        if (s->channel_valid)
            pa_dynarray_put((s->direction == PA_STREAM_PLAYBACK) ? s->context->playback_streams : s->context->record_streams, s->channel, NULL);

        PA_LLIST_REMOVE(struct pa_stream, s->context->streams, s);
        pa_stream_unref(s);
    }

    if (s->state_callback)
        s->state_callback(s, s->state_userdata);

    pa_stream_unref(s);
}

void pa_command_stream_killed(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_context *c = userdata;
    struct pa_stream *s;
    uint32_t channel;
    assert(pd && (command == PA_COMMAND_PLAYBACK_STREAM_KILLED || command == PA_COMMAND_RECORD_STREAM_KILLED) && t && c);

    pa_context_ref(c);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERROR_PROTOCOL);
        goto finish;
    }
    
    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_KILLED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    c->error = PA_ERROR_KILLED;
    pa_stream_set_state(s, PA_STREAM_FAILED);

finish:
    pa_context_unref(c);
}

void pa_command_request(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s;
    struct pa_context *c = userdata;
    uint32_t bytes, channel;
    assert(pd && command == PA_COMMAND_REQUEST && t && c);

    pa_context_ref(c);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERROR_PROTOCOL);
        goto finish;
    }
    
    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state != PA_STREAM_READY)
        goto finish;

    pa_stream_ref(s);
    
    s->requested_bytes += bytes;

    if (s->requested_bytes && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);

    pa_stream_unref(s);

finish:
    pa_context_unref(c);
}

void pa_create_stream_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s = userdata;
    assert(pd && s && s->state == PA_STREAM_CREATING);

    pa_stream_ref(s);
    
    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t) < 0)
            goto finish;
        
        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &s->channel) < 0 ||
        ((s->direction != PA_STREAM_UPLOAD) && pa_tagstruct_getu32(t, &s->device_index) < 0) ||
        ((s->direction == PA_STREAM_PLAYBACK) && pa_tagstruct_getu32(t, &s->requested_bytes) < 0) ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERROR_PROTOCOL);
        goto finish;
    }

    s->channel_valid = 1;
    pa_dynarray_put((s->direction == PA_STREAM_RECORD) ? s->context->record_streams : s->context->playback_streams, s->channel, s);
    pa_stream_set_state(s, PA_STREAM_READY);

    if (s->requested_bytes && s->ref > 1 && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);

finish:
    pa_stream_unref(s);
}

static void create_stream(struct pa_stream *s, const char *dev, const struct pa_buffer_attr *attr) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(s && s->ref >= 1 && s->state == PA_STREAM_DISCONNECTED);

    pa_stream_ref(s);
    
    if (attr)
        s->buffer_attr = *attr;
    else {
        s->buffer_attr.maxlength = DEFAULT_MAXLENGTH;
        s->buffer_attr.tlength = DEFAULT_TLENGTH;
        s->buffer_attr.prebuf = DEFAULT_PREBUF;
        s->buffer_attr.minreq = DEFAULT_MINREQ;
        s->buffer_attr.fragsize = DEFAULT_FRAGSIZE;
    }

    pa_stream_set_state(s, PA_STREAM_CREATING);
    
    t = pa_tagstruct_new(NULL, 0);
    assert(t);

    if (!dev) {
        if (s->direction == PA_STREAM_PLAYBACK)
            dev = getenv(ENV_DEFAULT_SINK);
        else
            dev = getenv(ENV_DEFAULT_SOURCE);
    }
    
    pa_tagstruct_putu32(t, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CREATE_PLAYBACK_STREAM : PA_COMMAND_CREATE_RECORD_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_puts(t, s->name);
    pa_tagstruct_put_sample_spec(t, &s->sample_spec);
    pa_tagstruct_putu32(t, PA_INVALID_INDEX);
    pa_tagstruct_puts(t, dev ? dev : "");
    pa_tagstruct_putu32(t, s->buffer_attr.maxlength);
    if (s->direction == PA_STREAM_PLAYBACK) {
        pa_tagstruct_putu32(t, s->buffer_attr.tlength);
        pa_tagstruct_putu32(t, s->buffer_attr.prebuf);
        pa_tagstruct_putu32(t, s->buffer_attr.minreq);
    } else
        pa_tagstruct_putu32(t, s->buffer_attr.fragsize);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_create_stream_callback, s);

    pa_stream_unref(s);
}

void pa_stream_connect_playback(struct pa_stream *s, const char *dev, const struct pa_buffer_attr *attr) {
    assert(s && s->context->state == PA_CONTEXT_READY && s->ref >= 1);
    s->direction = PA_STREAM_PLAYBACK;
    create_stream(s, dev, attr);
}

void pa_stream_connect_record(struct pa_stream *s, const char *dev, const struct pa_buffer_attr *attr) {
    assert(s && s->context->state == PA_CONTEXT_READY && s->ref >= 1);
    s->direction = PA_STREAM_RECORD;
    create_stream(s, dev, attr);
}

void pa_stream_write(struct pa_stream *s, const void *data, size_t length, void (*free_cb)(void *p), size_t delta) {
    struct pa_memchunk chunk;
    assert(s && s->context && data && length && s->state == PA_STREAM_READY && s->ref >= 1);

    if (free_cb) {
        chunk.memblock = pa_memblock_new_user((void*) data, length, free_cb, s->context->memblock_stat);
        assert(chunk.memblock && chunk.memblock->data);
    } else {
        chunk.memblock = pa_memblock_new(length, s->context->memblock_stat);
        assert(chunk.memblock && chunk.memblock->data);
        memcpy(chunk.memblock->data, data, length);
    }
    chunk.index = 0;
    chunk.length = length;

    pa_pstream_send_memblock(s->context->pstream, s->channel, delta, &chunk);
    pa_memblock_unref(chunk.memblock);
    
    if (length < s->requested_bytes)
        s->requested_bytes -= length;
    else
        s->requested_bytes = 0;
}

size_t pa_stream_writable_size(struct pa_stream *s) {
    assert(s && s->state == PA_STREAM_READY && s->ref >= 1);
    return s->requested_bytes;
}

struct pa_operation * pa_stream_drain(struct pa_stream *s, void (*cb) (struct pa_stream*s, int success, void *userdata), void *userdata) {
    struct pa_operation *o;
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(s && s->ref >= 1 && s->state == PA_STREAM_READY);

    o = pa_operation_new(s->context, s);
    assert(o);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_DRAIN_PLAYBACK_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, o);

    return pa_operation_ref(o);
}

static void stream_get_latency_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_operation *o = userdata;
    struct pa_latency_info i, *p = NULL;
    struct timeval local, remote, now;
    assert(pd && o && o->stream && o->context);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

    } else if (pa_tagstruct_get_usec(t, &i.buffer_usec) < 0 ||
               pa_tagstruct_get_usec(t, &i.sink_usec) < 0 ||
               pa_tagstruct_get_boolean(t, &i.playing) < 0 ||
               pa_tagstruct_getu32(t, &i.queue_length) < 0 ||
               pa_tagstruct_get_timeval(t, &local) < 0 ||
               pa_tagstruct_get_timeval(t, &remote) < 0 ||
               !pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERROR_PROTOCOL);
        goto finish;
    } else
        p = &i;

    gettimeofday(&now, NULL);

    if (pa_timeval_cmp(&local, &remote) < 0 && pa_timeval_cmp(&remote, &now)) {
        /* local and remote seem to have synchronized clocks */
        i.transport_usec = pa_timeval_diff(&remote, &local);
        i.synchronized_clocks = 1;
        i.timestamp = remote;
    } else {
        /* clocks are not synchronized, let's estimate latency then */
        i.transport_usec = pa_timeval_diff(&now, &local)/2;
        i.synchronized_clocks = 0;
        i.timestamp = local;
        pa_timeval_add(&i.timestamp, i.transport_usec);
    }

    if (o->callback) {
        void (*cb)(struct pa_stream *s, const struct pa_latency_info *i, void *userdata) = o->callback;
        cb(o->stream, p, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

struct pa_operation* pa_stream_get_latency(struct pa_stream *s, void (*cb)(struct pa_stream *p, const struct pa_latency_info*i, void *userdata), void *userdata) {
    uint32_t tag;
    struct pa_operation *o;
    struct pa_tagstruct *t;
    struct timeval now;

    o = pa_operation_new(s->context, s);
    assert(o);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_GET_PLAYBACK_LATENCY);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);

    gettimeofday(&now, NULL);
    pa_tagstruct_put_timeval(t, &now);
    
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_latency_callback, o);

    return pa_operation_ref(o);
}

void pa_stream_disconnect_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_stream *s = userdata;
    assert(pd && s && s->ref >= 1);

    pa_stream_ref(s);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t) < 0)
            goto finish;

        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERROR_PROTOCOL);
        goto finish;
    }

    pa_stream_set_state(s, PA_STREAM_TERMINATED);

finish:
    pa_stream_unref(s);
}

void pa_stream_disconnect(struct pa_stream *s) {
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(s && s->ref >= 1);
    
    if (!s->channel_valid || !s->context->state == PA_CONTEXT_READY)
        return;

    pa_stream_ref(s);

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    
    pa_tagstruct_putu32(t, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_DELETE_PLAYBACK_STREAM :
                        (s->direction == PA_STREAM_RECORD ? PA_COMMAND_DELETE_RECORD_STREAM : PA_COMMAND_DELETE_UPLOAD_STREAM));
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_disconnect_callback, s);

    pa_stream_unref(s);
}

void pa_stream_set_read_callback(struct pa_stream *s, void (*cb)(struct pa_stream *p, const void*data, size_t length, void *userdata), void *userdata) {
    assert(s && s->ref >= 1);
    s->read_callback = cb;
    s->read_userdata = userdata;
}

void pa_stream_set_write_callback(struct pa_stream *s, void (*cb)(struct pa_stream *p, size_t length, void *userdata), void *userdata) {
    assert(s && s->ref >= 1);
    s->write_callback = cb;
    s->write_userdata = userdata;
}

void pa_stream_set_state_callback(struct pa_stream *s, void (*cb)(struct pa_stream *s, void *userdata), void *userdata) {
    assert(s && s->ref >= 1);
    s->state_callback = cb;
    s->state_userdata = userdata;
}

void pa_stream_simple_ack_callback(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata) {
    struct pa_operation *o = userdata;
    int success = 1;
    assert(pd && o && o->context && o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        success = 0;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERROR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        void (*cb)(struct pa_stream *s, int success, void *userdata) = o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

struct pa_operation* pa_stream_cork(struct pa_stream *s, int b, void (*cb) (struct pa_stream*s, int success, void *userdata), void *userdata) {
    struct pa_operation *o;
    struct pa_tagstruct *t;
    uint32_t tag;
    assert(s && s->ref >= 1 && s->state == PA_STREAM_READY);

    o = pa_operation_new(s->context, s);
    assert(o);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_CORK_PLAYBACK_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_putu32(t, !!b);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, o);

    return pa_operation_ref(o);
}

struct pa_operation* pa_stream_send_simple_command(struct pa_stream *s, uint32_t command, void (*cb)(struct pa_stream *s, int success, void *userdata), void *userdata) {
    struct pa_tagstruct *t;
    struct pa_operation *o;
    uint32_t tag;
    assert(s && s->ref >= 1 && s->state == PA_STREAM_READY);
    
    o = pa_operation_new(s->context, s);
    o->callback = cb;
    o->userdata = userdata;

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, command);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, o);

    return pa_operation_ref(o);
}


struct pa_operation* pa_stream_flush(struct pa_stream *s, void (*cb)(struct pa_stream *s, int success, void *userdata), void *userdata) {
    return pa_stream_send_simple_command(s, PA_COMMAND_FLUSH_PLAYBACK_STREAM, cb, userdata);
}

struct pa_operation* pa_stream_trigger(struct pa_stream *s, void (*cb)(struct pa_stream *s, int success, void *userdata), void *userdata) {
    return pa_stream_send_simple_command(s, PA_COMMAND_TRIGGER_PLAYBACK_STREAM, cb, userdata);
}
