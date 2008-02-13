/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <string.h>
#include <stdio.h>
#include <string.h>

#include <pulse/def.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/pstream-util.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/macro.h>

#include "internal.h"

#define LATENCY_IPOL_INTERVAL_USEC (100000L)

pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map) {
    pa_stream *s;
    int i;
    pa_channel_map tmap;

    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(c, ss && pa_sample_spec_valid(ss), PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, c->version >= 12 || (ss->format != PA_SAMPLE_S32LE || ss->format != PA_SAMPLE_S32NE), PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !map || (pa_channel_map_valid(map) && map->channels == ss->channels), PA_ERR_INVALID);

    if (!map)
        PA_CHECK_VALIDITY_RETURN_NULL(c, map = pa_channel_map_init_auto(&tmap, ss->channels, PA_CHANNEL_MAP_DEFAULT), PA_ERR_INVALID);

    s = pa_xnew(pa_stream, 1);
    PA_REFCNT_INIT(s);
    s->context = c;
    s->mainloop = c->mainloop;

    s->buffer_attr_not_ready = s->timing_info_not_ready = FALSE;

    s->read_callback = NULL;
    s->read_userdata = NULL;
    s->write_callback = NULL;
    s->write_userdata = NULL;
    s->state_callback = NULL;
    s->state_userdata = NULL;
    s->overflow_callback = NULL;
    s->overflow_userdata = NULL;
    s->underflow_callback = NULL;
    s->underflow_userdata = NULL;
    s->latency_update_callback = NULL;
    s->latency_update_userdata = NULL;
    s->moved_callback = NULL;
    s->moved_userdata = NULL;
    s->suspended_callback = NULL;
    s->suspended_userdata = NULL;

    s->direction = PA_STREAM_NODIRECTION;
    s->name = pa_xstrdup(name);
    s->sample_spec = *ss;
    s->channel_map = *map;
    s->flags = 0;

    s->channel = 0;
    s->channel_valid = 0;
    s->syncid = c->csyncid++;
    s->stream_index = PA_INVALID_INDEX;
    s->requested_bytes = 0;
    s->state = PA_STREAM_UNCONNECTED;

    s->manual_buffer_attr = FALSE;
    memset(&s->buffer_attr, 0, sizeof(s->buffer_attr));

    s->device_index = PA_INVALID_INDEX;
    s->device_name = NULL;
    s->suspended = FALSE;

    s->peek_memchunk.index = 0;
    s->peek_memchunk.length = 0;
    s->peek_memchunk.memblock = NULL;
    s->peek_data = NULL;

    s->record_memblockq = NULL;

    s->previous_time = 0;
    s->timing_info_valid = 0;
    s->read_index_not_before = 0;
    s->write_index_not_before = 0;

    for (i = 0; i < PA_MAX_WRITE_INDEX_CORRECTIONS; i++)
        s->write_index_corrections[i].valid = 0;
    s->current_write_index_correction = 0;

    s->corked = 0;

    s->cached_time_valid = 0;

    s->auto_timing_update_event = NULL;
    s->auto_timing_update_requested = 0;

    /* Refcounting is strictly one-way: from the "bigger" to the "smaller" object. */
    PA_LLIST_PREPEND(pa_stream, c->streams, s);
    pa_stream_ref(s);

    return s;
}

static void stream_free(pa_stream *s) {
    pa_assert(s);
    pa_assert(!s->context);
    pa_assert(!s->channel_valid);

    if (s->auto_timing_update_event) {
        pa_assert(s->mainloop);
        s->mainloop->time_free(s->auto_timing_update_event);
    }

    if (s->peek_memchunk.memblock) {
        if (s->peek_data)
            pa_memblock_release(s->peek_memchunk.memblock);
        pa_memblock_unref(s->peek_memchunk.memblock);
    }

    if (s->record_memblockq)
        pa_memblockq_free(s->record_memblockq);

    pa_xfree(s->name);
    pa_xfree(s->device_name);
    pa_xfree(s);
}

void pa_stream_unref(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (PA_REFCNT_DEC(s) <= 0)
        stream_free(s);
}

pa_stream* pa_stream_ref(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_REFCNT_INC(s);
    return s;
}

pa_stream_state_t pa_stream_get_state(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return s->state;
}

pa_context* pa_stream_get_context(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return s->context;
}

uint32_t pa_stream_get_index(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, PA_INVALID_INDEX);

    return s->stream_index;
}

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (s->state == st)
        return;

    pa_stream_ref(s);

    s->state = st;
    if (s->state_callback)
        s->state_callback(s, s->state_userdata);

    if ((st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) && s->context) {

        /* Detach from context */
        pa_operation *o, *n;

        /* Unref all operatio object that point to us */
        for (o = s->context->operations; o; o = n) {
            n = o->next;

            if (o->stream == s)
                pa_operation_cancel(o);
        }

        /* Drop all outstanding replies for this stream */
        if (s->context->pdispatch)
            pa_pdispatch_unregister_reply(s->context->pdispatch, s);

        if (s->channel_valid)
            pa_dynarray_put((s->direction == PA_STREAM_PLAYBACK) ? s->context->playback_streams : s->context->record_streams, s->channel, NULL);

        PA_LLIST_REMOVE(pa_stream, s->context->streams, s);
        pa_stream_unref(s);

        s->channel = 0;
        s->channel_valid = 0;

        s->context = NULL;

        s->read_callback = NULL;
        s->write_callback = NULL;
        s->state_callback = NULL;
        s->overflow_callback = NULL;
        s->underflow_callback = NULL;
        s->latency_update_callback = NULL;
    }

    pa_stream_unref(s);
}

void pa_command_stream_killed(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_PLAYBACK_STREAM_KILLED || command == PA_COMMAND_RECORD_STREAM_KILLED);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_KILLED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    pa_context_set_error(c, PA_ERR_KILLED);
    pa_stream_set_state(s, PA_STREAM_FAILED);

finish:
    pa_context_unref(c);
}

void pa_command_stream_moved(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;
    const char *dn;
    int suspended;
    uint32_t di;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_PLAYBACK_STREAM_MOVED || command == PA_COMMAND_RECORD_STREAM_MOVED);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (c->version < 12) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &di) < 0 ||
        pa_tagstruct_gets(t, &dn) < 0 ||
        pa_tagstruct_get_boolean(t, &suspended) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!dn || di == PA_INVALID_INDEX) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_MOVED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    pa_xfree(s->device_name);
    s->device_name = pa_xstrdup(dn);
    s->device_index = di;

    s->suspended = suspended;

    if (s->moved_callback)
        s->moved_callback(s, s->moved_userdata);

finish:
    pa_context_unref(c);
}

void pa_command_stream_suspended(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;
    int suspended;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_PLAYBACK_STREAM_SUSPENDED || command == PA_COMMAND_RECORD_STREAM_SUSPENDED);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (c->version < 12) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_get_boolean(t, &suspended) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_SUSPENDED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    s->suspended = suspended;

    if (s->suspended_callback)
        s->suspended_callback(s, s->suspended_userdata);

finish:
    pa_context_unref(c);
}

void pa_command_request(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s;
    pa_context *c = userdata;
    uint32_t bytes, channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_REQUEST);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state == PA_STREAM_READY) {
        s->requested_bytes += bytes;

        if (s->requested_bytes > 0 && s->write_callback)
            s->write_callback(s, s->requested_bytes, s->write_userdata);
    }

finish:
    pa_context_unref(c);
}

void pa_command_overflow_or_underflow(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s;
    pa_context *c = userdata;
    uint32_t channel;

    pa_assert(pd);
    pa_assert(command == PA_COMMAND_OVERFLOW || command == PA_COMMAND_UNDERFLOW);
    pa_assert(t);
    pa_assert(c);
    pa_assert(PA_REFCNT_VALUE(c) >= 1);

    pa_context_ref(c);

    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state == PA_STREAM_READY) {

        if (command == PA_COMMAND_OVERFLOW) {
            if (s->overflow_callback)
                s->overflow_callback(s, s->overflow_userdata);
        } else if (command == PA_COMMAND_UNDERFLOW) {
            if (s->underflow_callback)
                s->underflow_callback(s, s->underflow_userdata);
        }
    }

 finish:
    pa_context_unref(c);
}

static void request_auto_timing_update(pa_stream *s, int force) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (!(s->flags & PA_STREAM_AUTO_TIMING_UPDATE))
        return;

    if (s->state == PA_STREAM_READY &&
        (force || !s->auto_timing_update_requested)) {
        pa_operation *o;

/*         pa_log("automatically requesting new timing data");   */

        if ((o = pa_stream_update_timing_info(s, NULL, NULL))) {
            pa_operation_unref(o);
            s->auto_timing_update_requested = 1;
        }
    }

    if (s->auto_timing_update_event) {
        struct timeval next;
        pa_gettimeofday(&next);
        pa_timeval_add(&next, LATENCY_IPOL_INTERVAL_USEC);
        s->mainloop->time_restart(s->auto_timing_update_event, &next);
    }
}

static void invalidate_indexes(pa_stream *s, int r, int w) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

/*     pa_log("invalidate r:%u w:%u tag:%u", r, w, s->context->ctag); */

    if (s->state != PA_STREAM_READY)
        return;

    if (w) {
        s->write_index_not_before = s->context->ctag;

        if (s->timing_info_valid)
            s->timing_info.write_index_corrupt = 1;

/*         pa_log("write_index invalidated"); */
    }

    if (r) {
        s->read_index_not_before = s->context->ctag;

        if (s->timing_info_valid)
            s->timing_info.read_index_corrupt = 1;

/*         pa_log("read_index invalidated"); */
    }

    if ((s->direction == PA_STREAM_PLAYBACK && r) ||
        (s->direction == PA_STREAM_RECORD && w))
        s->cached_time_valid = 0;

    request_auto_timing_update(s, 1);
}

static void auto_timing_update_callback(PA_GCC_UNUSED pa_mainloop_api *m, PA_GCC_UNUSED pa_time_event *e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    pa_stream *s = userdata;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

/*     pa_log("time event");    */

    pa_stream_ref(s);
    request_auto_timing_update(s, 0);
    pa_stream_unref(s);
}

static void create_stream_complete(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(s->state == PA_STREAM_CREATING);

    if (s->buffer_attr_not_ready || s->timing_info_not_ready)
        return;

    pa_stream_set_state(s, PA_STREAM_READY);

    if (s->requested_bytes > 0 && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);

    if (s->flags & PA_STREAM_AUTO_TIMING_UPDATE) {
        struct timeval tv;
        pa_gettimeofday(&tv);
        tv.tv_usec += LATENCY_IPOL_INTERVAL_USEC; /* every 100 ms */
        pa_assert(!s->auto_timing_update_event);
        s->auto_timing_update_event = s->mainloop->time_new(s->mainloop, &tv, &auto_timing_update_callback, s);
    }
}

static void automatic_buffer_attr(pa_buffer_attr *attr, pa_sample_spec *ss) {
    pa_assert(attr);
    pa_assert(ss);

    attr->tlength = pa_bytes_per_second(ss)/2;
    attr->maxlength = (attr->tlength*3)/2;
    attr->minreq = attr->tlength/50;
    attr->prebuf = attr->tlength - attr->minreq;
    attr->fragsize = attr->tlength/50;
}

void pa_create_stream_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s = userdata;

    pa_assert(pd);
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(s->state == PA_STREAM_CREATING);

    pa_stream_ref(s);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t) < 0)
            goto finish;

        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &s->channel) < 0 ||
        ((s->direction != PA_STREAM_UPLOAD) && pa_tagstruct_getu32(t, &s->stream_index) < 0) ||
        ((s->direction != PA_STREAM_RECORD) && pa_tagstruct_getu32(t, &s->requested_bytes) < 0)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (s->context->version >= 9) {
        if (s->direction == PA_STREAM_PLAYBACK) {
            if (pa_tagstruct_getu32(t, &s->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.tlength) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.prebuf) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.minreq) < 0) {
                pa_context_fail(s->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        } else if (s->direction == PA_STREAM_RECORD) {
            if (pa_tagstruct_getu32(t, &s->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &s->buffer_attr.fragsize) < 0) {
                pa_context_fail(s->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        }
    }

    if (s->context->version >= 12 && s->direction != PA_STREAM_UPLOAD) {
        pa_sample_spec ss;
        pa_channel_map cm;
        const char *dn = NULL;
        int suspended;

        if (pa_tagstruct_get_sample_spec(t, &ss) < 0 ||
            pa_tagstruct_get_channel_map(t, &cm) < 0 ||
            pa_tagstruct_getu32(t, &s->device_index) < 0 ||
            pa_tagstruct_gets(t, &dn) < 0 ||
            pa_tagstruct_get_boolean(t, &suspended) < 0) {
            pa_context_fail(s->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        if (!dn || s->device_index == PA_INVALID_INDEX ||
            ss.channels != cm.channels ||
            !pa_channel_map_valid(&cm) ||
            !pa_sample_spec_valid(&ss) ||
            (!(s->flags & PA_STREAM_FIX_FORMAT) && ss.format != s->sample_spec.format) ||
            (!(s->flags & PA_STREAM_FIX_RATE) && ss.rate != s->sample_spec.rate) ||
            (!(s->flags & PA_STREAM_FIX_CHANNELS) && !pa_channel_map_equal(&cm, &s->channel_map))) {
            pa_context_fail(s->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        pa_xfree(s->device_name);
        s->device_name = pa_xstrdup(dn);
        s->suspended = suspended;

        if (!s->manual_buffer_attr && pa_bytes_per_second(&ss) != pa_bytes_per_second(&s->sample_spec)) {
            pa_buffer_attr attr;
            pa_operation *o;

            automatic_buffer_attr(&attr, &ss);

            /* If we need to update the buffer metrics, we wait for
             * the the OK for that call before we go to
             * PA_STREAM_READY */

            s->state = PA_STREAM_READY;
            pa_assert_se(o = pa_stream_set_buffer_attr(s, &attr, NULL, NULL));
            pa_operation_unref(o);
            s->state = PA_STREAM_CREATING;

            s->buffer_attr_not_ready = TRUE;
        }

        s->channel_map = cm;
        s->sample_spec = ss;
    }

    if (!pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (s->direction == PA_STREAM_RECORD) {
        pa_assert(!s->record_memblockq);

        s->record_memblockq = pa_memblockq_new(
                0,
                s->buffer_attr.maxlength,
                0,
                pa_frame_size(&s->sample_spec),
                1,
                0,
                NULL);
    }

    s->channel_valid = 1;
    pa_dynarray_put((s->direction == PA_STREAM_RECORD) ? s->context->record_streams : s->context->playback_streams, s->channel, s);

    if (s->direction != PA_STREAM_UPLOAD && s->flags & PA_STREAM_AUTO_TIMING_UPDATE) {

        /* If automatic timing updates are active, we wait for the
         * first timing update before going to PA_STREAM_READY
         * state */

        s->state = PA_STREAM_READY;
        request_auto_timing_update(s, 1);
        s->state = PA_STREAM_CREATING;

        s->timing_info_not_ready = TRUE;
    }

    create_stream_complete(s);

finish:
    pa_stream_unref(s);
}

static int create_stream(
        pa_stream_direction_t direction,
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        const pa_cvolume *volume,
        pa_stream *sync_stream) {

    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, !(flags & ~((direction != PA_STREAM_UPLOAD ?
                                               PA_STREAM_START_CORKED|
                                               PA_STREAM_INTERPOLATE_TIMING|
                                               PA_STREAM_NOT_MONOTONOUS|
                                               PA_STREAM_AUTO_TIMING_UPDATE|
                                               PA_STREAM_NO_REMAP_CHANNELS|
                                               PA_STREAM_NO_REMIX_CHANNELS|
                                               PA_STREAM_FIX_FORMAT|
                                               PA_STREAM_FIX_RATE|
                                               PA_STREAM_FIX_CHANNELS|
                                               PA_STREAM_DONT_MOVE : 0))), PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, !volume || volume->channels == s->sample_spec.channels, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, !sync_stream || (direction == PA_STREAM_PLAYBACK && sync_stream->direction == PA_STREAM_PLAYBACK), PA_ERR_INVALID);

    pa_stream_ref(s);

    s->direction = direction;
    s->flags = flags;

    if (sync_stream)
        s->syncid = sync_stream->syncid;

    if (attr) {
        s->buffer_attr = *attr;
        s->manual_buffer_attr = TRUE;
    } else {
        /* half a second, with minimum request of 10 ms */
        s->buffer_attr.tlength = pa_bytes_per_second(&s->sample_spec)/2;
        s->buffer_attr.maxlength = (s->buffer_attr.tlength*3)/2;
        s->buffer_attr.minreq = s->buffer_attr.tlength/50;
        s->buffer_attr.prebuf = s->buffer_attr.tlength - s->buffer_attr.minreq;
        s->buffer_attr.fragsize = s->buffer_attr.tlength/50;
        s->manual_buffer_attr = FALSE;
    }

    if (!dev)
        dev = s->direction == PA_STREAM_PLAYBACK ? s->context->conf->default_sink : s->context->conf->default_source;

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CREATE_PLAYBACK_STREAM : PA_COMMAND_CREATE_RECORD_STREAM,
            &tag);

    pa_tagstruct_put(
            t,
            PA_TAG_STRING, s->name,
            PA_TAG_SAMPLE_SPEC, &s->sample_spec,
            PA_TAG_CHANNEL_MAP, &s->channel_map,
            PA_TAG_U32, PA_INVALID_INDEX,
            PA_TAG_STRING, dev,
            PA_TAG_U32, s->buffer_attr.maxlength,
            PA_TAG_BOOLEAN, !!(flags & PA_STREAM_START_CORKED),
            PA_TAG_INVALID);

    if (s->direction == PA_STREAM_PLAYBACK) {
        pa_cvolume cv;

        pa_tagstruct_put(
                t,
                PA_TAG_U32, s->buffer_attr.tlength,
                PA_TAG_U32, s->buffer_attr.prebuf,
                PA_TAG_U32, s->buffer_attr.minreq,
                PA_TAG_U32, s->syncid,
                PA_TAG_INVALID);

        if (!volume)
            volume = pa_cvolume_reset(&cv, s->sample_spec.channels);

        pa_tagstruct_put_cvolume(t, volume);
    } else
        pa_tagstruct_putu32(t, s->buffer_attr.fragsize);

    if (s->context->version >= 12 && s->direction != PA_STREAM_UPLOAD) {
        pa_tagstruct_put(
                t,
                PA_TAG_BOOLEAN, flags & PA_STREAM_NO_REMAP_CHANNELS,
                PA_TAG_BOOLEAN, flags & PA_STREAM_NO_REMIX_CHANNELS,
                PA_TAG_BOOLEAN, flags & PA_STREAM_FIX_FORMAT,
                PA_TAG_BOOLEAN, flags & PA_STREAM_FIX_RATE,
                PA_TAG_BOOLEAN, flags & PA_STREAM_FIX_CHANNELS,
                PA_TAG_BOOLEAN, flags & PA_STREAM_DONT_MOVE,
                PA_TAG_BOOLEAN, flags & PA_STREAM_VARIABLE_RATE,
                PA_TAG_INVALID);
    }

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_create_stream_callback, s, NULL);

    pa_stream_set_state(s, PA_STREAM_CREATING);

    pa_stream_unref(s);
    return 0;
}

int pa_stream_connect_playback(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        pa_cvolume *volume,
        pa_stream *sync_stream) {

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return create_stream(PA_STREAM_PLAYBACK, s, dev, attr, flags, volume, sync_stream);
}

int pa_stream_connect_record(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags) {

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return create_stream(PA_STREAM_RECORD, s, dev, attr, flags, NULL, NULL);
}

int pa_stream_write(
        pa_stream *s,
        const void *data,
        size_t length,
        void (*free_cb)(void *p),
        int64_t offset,
        pa_seek_mode_t seek) {

    pa_memchunk chunk;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(data);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK || s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, seek <= PA_SEEK_RELATIVE_END, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK || (seek == PA_SEEK_RELATIVE && offset == 0), PA_ERR_INVALID);

    if (length <= 0)
        return 0;

    if (free_cb)
        chunk.memblock = pa_memblock_new_user(s->context->mempool, (void*) data, length, free_cb, 1);
    else {
        void *tdata;
        chunk.memblock = pa_memblock_new(s->context->mempool, length);
        tdata = pa_memblock_acquire(chunk.memblock);
        memcpy(tdata, data, length);
        pa_memblock_release(chunk.memblock);
    }

    chunk.index = 0;
    chunk.length = length;

    pa_pstream_send_memblock(s->context->pstream, s->channel, offset, seek, &chunk);
    pa_memblock_unref(chunk.memblock);

    if (length < s->requested_bytes)
        s->requested_bytes -= length;
    else
        s->requested_bytes = 0;

    if (s->direction == PA_STREAM_PLAYBACK) {

        /* Update latency request correction */
        if (s->write_index_corrections[s->current_write_index_correction].valid) {

            if (seek == PA_SEEK_ABSOLUTE) {
                s->write_index_corrections[s->current_write_index_correction].corrupt = 0;
                s->write_index_corrections[s->current_write_index_correction].absolute = 1;
                s->write_index_corrections[s->current_write_index_correction].value = offset + length;
            } else if (seek == PA_SEEK_RELATIVE) {
                if (!s->write_index_corrections[s->current_write_index_correction].corrupt)
                    s->write_index_corrections[s->current_write_index_correction].value += offset + length;
            } else
                s->write_index_corrections[s->current_write_index_correction].corrupt = 1;
        }

        /* Update the write index in the already available latency data */
        if (s->timing_info_valid) {

            if (seek == PA_SEEK_ABSOLUTE) {
                s->timing_info.write_index_corrupt = 0;
                s->timing_info.write_index = offset + length;
            } else if (seek == PA_SEEK_RELATIVE) {
                if (!s->timing_info.write_index_corrupt)
                    s->timing_info.write_index += offset + length;
            } else
                s->timing_info.write_index_corrupt = 1;
        }

        if (!s->timing_info_valid || s->timing_info.write_index_corrupt)
            request_auto_timing_update(s, 1);
    }

    return 0;
}

int pa_stream_peek(pa_stream *s, const void **data, size_t *length) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(data);
    pa_assert(length);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);

    if (!s->peek_memchunk.memblock) {

        if (pa_memblockq_peek(s->record_memblockq, &s->peek_memchunk) < 0) {
            *data = NULL;
            *length = 0;
            return 0;
        }

        s->peek_data = pa_memblock_acquire(s->peek_memchunk.memblock);
    }

    pa_assert(s->peek_data);
    *data = (uint8_t*) s->peek_data + s->peek_memchunk.index;
    *length = s->peek_memchunk.length;
    return 0;
}

int pa_stream_drop(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->peek_memchunk.memblock, PA_ERR_BADSTATE);

    pa_memblockq_drop(s->record_memblockq, s->peek_memchunk.length);

    /* Fix the simulated local read index */
    if (s->timing_info_valid && !s->timing_info.read_index_corrupt)
        s->timing_info.read_index += s->peek_memchunk.length;

    pa_assert(s->peek_data);
    pa_memblock_release(s->peek_memchunk.memblock);
    pa_memblock_unref(s->peek_memchunk.memblock);
    s->peek_memchunk.length = 0;
    s->peek_memchunk.index = 0;
    s->peek_memchunk.memblock = NULL;

    return 0;
}

size_t pa_stream_writable_size(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (size_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_RECORD, PA_ERR_BADSTATE, (size_t) -1);

    return s->requested_bytes;
}

size_t pa_stream_readable_size(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (size_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE, (size_t) -1);

    return pa_memblockq_get_length(s->record_memblockq);
}

pa_operation * pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(s->context, PA_COMMAND_DRAIN_PLAYBACK_STREAM, &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

static void stream_get_timing_info_callback(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    struct timeval local, remote, now;
    pa_timing_info *i;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context || !o->stream)
        goto finish;

    i = &o->stream->timing_info;

/*     pa_log("pre corrupt w:%u r:%u\n", !o->stream->timing_info_valid || i->write_index_corrupt,!o->stream->timing_info_valid || i->read_index_corrupt); */

    o->stream->timing_info_valid = 0;
    i->write_index_corrupt = 0;
    i->read_index_corrupt = 0;

/*     pa_log("timing update %u\n", tag); */

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

    } else if (pa_tagstruct_get_usec(t, &i->sink_usec) < 0 ||
               pa_tagstruct_get_usec(t, &i->source_usec) < 0 ||
               pa_tagstruct_get_boolean(t, &i->playing) < 0 ||
               pa_tagstruct_get_timeval(t, &local) < 0 ||
               pa_tagstruct_get_timeval(t, &remote) < 0 ||
               pa_tagstruct_gets64(t, &i->write_index) < 0 ||
               pa_tagstruct_gets64(t, &i->read_index) < 0 ||
               !pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;

    } else {
        o->stream->timing_info_valid = 1;

        pa_gettimeofday(&now);

        /* Calculcate timestamps */
        if (pa_timeval_cmp(&local, &remote) <= 0 && pa_timeval_cmp(&remote, &now) <= 0) {
            /* local and remote seem to have synchronized clocks */

            if (o->stream->direction == PA_STREAM_PLAYBACK)
                i->transport_usec = pa_timeval_diff(&remote, &local);
            else
                i->transport_usec = pa_timeval_diff(&now, &remote);

            i->synchronized_clocks = 1;
            i->timestamp = remote;
        } else {
            /* clocks are not synchronized, let's estimate latency then */
            i->transport_usec = pa_timeval_diff(&now, &local)/2;
            i->synchronized_clocks = 0;
            i->timestamp = local;
            pa_timeval_add(&i->timestamp, i->transport_usec);
        }

        /* Invalidate read and write indexes if necessary */
        if (tag < o->stream->read_index_not_before)
            i->read_index_corrupt = 1;

        if (tag < o->stream->write_index_not_before)
            i->write_index_corrupt = 1;

        if (o->stream->direction == PA_STREAM_PLAYBACK) {
            /* Write index correction */

            int n, j;
            uint32_t ctag = tag;

            /* Go through the saved correction values and add up the total correction.*/

            for (n = 0, j = o->stream->current_write_index_correction+1;
                 n < PA_MAX_WRITE_INDEX_CORRECTIONS;
                 n++, j = (j + 1) % PA_MAX_WRITE_INDEX_CORRECTIONS) {

                /* Step over invalid data or out-of-date data */
                if (!o->stream->write_index_corrections[j].valid ||
                    o->stream->write_index_corrections[j].tag < ctag)
                    continue;

                /* Make sure that everything is in order */
                ctag = o->stream->write_index_corrections[j].tag+1;

                /* Now fix the write index */
                if (o->stream->write_index_corrections[j].corrupt) {
                    /* A corrupting seek was made */
                    i->write_index = 0;
                    i->write_index_corrupt = 1;
                } else if (o->stream->write_index_corrections[j].absolute) {
                    /* An absolute seek was made */
                    i->write_index = o->stream->write_index_corrections[j].value;
                    i->write_index_corrupt = 0;
                } else if (!i->write_index_corrupt) {
                    /* A relative seek was made */
                    i->write_index += o->stream->write_index_corrections[j].value;
                }
            }
        }

        if (o->stream->direction == PA_STREAM_RECORD) {
            /* Read index correction */

            if (!i->read_index_corrupt)
                i->read_index -= pa_memblockq_get_length(o->stream->record_memblockq);
        }

        o->stream->cached_time_valid = 0;
    }

    o->stream->auto_timing_update_requested = 0;
/*     pa_log("post corrupt w:%u r:%u\n", i->write_index_corrupt || !o->stream->timing_info_valid, i->read_index_corrupt || !o->stream->timing_info_valid); */

    /* Clear old correction entries */
    if (o->stream->direction == PA_STREAM_PLAYBACK) {
        int n;

        for (n = 0; n < PA_MAX_WRITE_INDEX_CORRECTIONS; n++) {
            if (!o->stream->write_index_corrections[n].valid)
                continue;

            if (o->stream->write_index_corrections[n].tag <= tag)
                o->stream->write_index_corrections[n].valid = 0;
        }
    }

    /* First, let's complete the initialization, if necessary. */
    if (o->stream->state == PA_STREAM_CREATING) {
        o->stream->timing_info_not_ready = FALSE;
        create_stream_complete(o->stream);
    }

    if (o->stream->latency_update_callback)
        o->stream->latency_update_callback(o->stream, o->stream->latency_update_userdata);

    if (o->callback && o->stream && o->stream->state == PA_STREAM_READY) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, o->stream->timing_info_valid, o->userdata);
    }

finish:

    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    uint32_t tag;
    pa_operation *o;
    pa_tagstruct *t;
    struct timeval now;
    int cidx = 0;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    if (s->direction == PA_STREAM_PLAYBACK) {
        /* Find a place to store the write_index correction data for this entry */
        cidx = (s->current_write_index_correction + 1) % PA_MAX_WRITE_INDEX_CORRECTIONS;

        /* Check if we could allocate a correction slot. If not, there are too many outstanding queries */
        PA_CHECK_VALIDITY_RETURN_NULL(s->context, !s->write_index_corrections[cidx].valid, PA_ERR_INTERNAL);
    }
    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_GET_PLAYBACK_LATENCY : PA_COMMAND_GET_RECORD_LATENCY,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_put_timeval(t, pa_gettimeofday(&now));

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_timing_info_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    if (s->direction == PA_STREAM_PLAYBACK) {
        /* Fill in initial correction data */
        o->stream->current_write_index_correction = cidx;
        o->stream->write_index_corrections[cidx].valid = 1;
        o->stream->write_index_corrections[cidx].tag = tag;
        o->stream->write_index_corrections[cidx].absolute = 0;
        o->stream->write_index_corrections[cidx].value = 0;
        o->stream->write_index_corrections[cidx].corrupt = 0;
    }

/*     pa_log("requesting update %u\n", tag); */

    return o;
}

void pa_stream_disconnect_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s = userdata;

    pa_assert(pd);
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    pa_stream_ref(s);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t) < 0)
            goto finish;

        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    pa_stream_set_state(s, PA_STREAM_TERMINATED);

finish:
    pa_stream_unref(s);
}

int pa_stream_disconnect(pa_stream *s) {
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->channel_valid, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->context->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    pa_stream_ref(s);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_DELETE_PLAYBACK_STREAM :
            (s->direction == PA_STREAM_RECORD ? PA_COMMAND_DELETE_RECORD_STREAM : PA_COMMAND_DELETE_UPLOAD_STREAM),
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_disconnect_callback, s, NULL);

    pa_stream_unref(s);
    return 0;
}

void pa_stream_set_read_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->read_callback = cb;
    s->read_userdata = userdata;
}

void pa_stream_set_write_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->write_callback = cb;
    s->write_userdata = userdata;
}

void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->state_callback = cb;
    s->state_userdata = userdata;
}

void pa_stream_set_overflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->overflow_callback = cb;
    s->overflow_userdata = userdata;
}

void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->underflow_callback = cb;
    s->underflow_userdata = userdata;
}

void pa_stream_set_latency_update_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->latency_update_callback = cb;
    s->latency_update_userdata = userdata;
}

void pa_stream_set_moved_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->moved_callback = cb;
    s->moved_userdata = userdata;
}

void pa_stream_set_suspended_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    s->suspended_callback = cb;
    s->suspended_userdata = userdata;
}

void pa_stream_simple_ack_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int success = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        success = 0;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    s->corked = b;

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CORK_PLAYBACK_STREAM : PA_COMMAND_CORK_RECORD_STREAM,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_put_boolean(t, !!b);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    if (s->direction == PA_STREAM_PLAYBACK)
        invalidate_indexes(s, 1, 0);

    return o;
}

static pa_operation* stream_send_simple_command(pa_stream *s, uint32_t command, pa_stream_success_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(s->context, command, &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

pa_operation* pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_FLUSH_PLAYBACK_STREAM : PA_COMMAND_FLUSH_RECORD_STREAM, cb, userdata))) {

        if (s->direction == PA_STREAM_PLAYBACK) {
            if (s->write_index_corrections[s->current_write_index_correction].valid)
                s->write_index_corrections[s->current_write_index_correction].corrupt = 1;

            if (s->timing_info_valid)
                s->timing_info.write_index_corrupt = 1;

            if (s->buffer_attr.prebuf > 0)
                invalidate_indexes(s, 1, 0);
            else
                request_auto_timing_update(s, 1);
        } else
            invalidate_indexes(s, 0, 1);
    }

    return o;
}

pa_operation* pa_stream_prebuf(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, PA_COMMAND_PREBUF_PLAYBACK_STREAM, cb, userdata)))
        invalidate_indexes(s, 1, 0);

    return o;
}

pa_operation* pa_stream_trigger(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, PA_COMMAND_TRIGGER_PLAYBACK_STREAM, cb, userdata)))
        invalidate_indexes(s, 1, 0);

    return o;
}

pa_operation* pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(name);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_RECORD ? PA_COMMAND_SET_RECORD_STREAM_NAME : PA_COMMAND_SET_PLAYBACK_STREAM_NAME,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

int pa_stream_get_time(pa_stream *s, pa_usec_t *r_usec) {
    pa_usec_t usec = 0;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_PLAYBACK || !s->timing_info.read_index_corrupt, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_RECORD || !s->timing_info.write_index_corrupt, PA_ERR_NODATA);

    if (s->cached_time_valid)
        /* We alredy calculated the time value for this timing info, so let's reuse it */
        usec = s->cached_time;
    else {
        if (s->direction == PA_STREAM_PLAYBACK) {
            /* The last byte that was written into the output device
             * had this time value associated */
            usec = pa_bytes_to_usec(s->timing_info.read_index < 0 ? 0 : (uint64_t) s->timing_info.read_index, &s->sample_spec);

            if (!s->corked) {
                /* Because the latency info took a little time to come
                 * to us, we assume that the real output time is actually
                 * a little ahead */
                usec += s->timing_info.transport_usec;

                /* However, the output device usually maintains a buffer
                   too, hence the real sample currently played is a little
                   back  */
                if (s->timing_info.sink_usec >= usec)
                    usec = 0;
                else
                    usec -= s->timing_info.sink_usec;
            }

        } else if (s->direction == PA_STREAM_RECORD) {
            /* The last byte written into the server side queue had
             * this time value associated */
            usec = pa_bytes_to_usec(s->timing_info.write_index < 0 ? 0 : (uint64_t) s->timing_info.write_index, &s->sample_spec);

            if (!s->corked) {
                /* Add transport latency */
                usec += s->timing_info.transport_usec;

                /* Add latency of data in device buffer */
                usec += s->timing_info.source_usec;

                /* If this is a monitor source, we need to correct the
                 * time by the playback device buffer */
                if (s->timing_info.sink_usec >= usec)
                    usec = 0;
                else
                    usec -= s->timing_info.sink_usec;
            }
        }

        s->cached_time = usec;
        s->cached_time_valid = 1;
    }

    /* Interpolate if requested */
    if (s->flags & PA_STREAM_INTERPOLATE_TIMING) {

        /* We just add the time that passed since the latency info was
         * current */
        if (!s->corked && s->timing_info.playing) {
            struct timeval now;
            usec += pa_timeval_diff(pa_gettimeofday(&now), &s->timing_info.timestamp);
        }
    }

    /* Make sure the time runs monotonically */
    if (!(s->flags & PA_STREAM_NOT_MONOTONOUS)) {
        if (usec < s->previous_time)
            usec = s->previous_time;
        else
            s->previous_time = usec;
    }

    if (r_usec)
        *r_usec = usec;

    return 0;
}

static pa_usec_t time_counter_diff(pa_stream *s, pa_usec_t a, pa_usec_t b, int *negative) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (negative)
        *negative = 0;

    if (a >= b)
        return a-b;
    else {
        if (negative && s->direction == PA_STREAM_RECORD) {
            *negative = 1;
            return b-a;
        } else
            return 0;
    }
}

int pa_stream_get_latency(pa_stream *s, pa_usec_t *r_usec, int *negative) {
    pa_usec_t t, c;
    int r;
    int64_t cindex;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(r_usec);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_PLAYBACK || !s->timing_info.write_index_corrupt, PA_ERR_NODATA);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_RECORD || !s->timing_info.read_index_corrupt, PA_ERR_NODATA);

    if ((r = pa_stream_get_time(s, &t)) < 0)
        return r;

    if (s->direction == PA_STREAM_PLAYBACK)
        cindex = s->timing_info.write_index;
    else
        cindex = s->timing_info.read_index;

    if (cindex < 0)
        cindex = 0;

    c = pa_bytes_to_usec(cindex, &s->sample_spec);

    if (s->direction == PA_STREAM_PLAYBACK)
        *r_usec = time_counter_diff(s, c, t, negative);
    else
        *r_usec = time_counter_diff(s, t, c, negative);

    return 0;
}

const pa_timing_info* pa_stream_get_timing_info(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->timing_info_valid, PA_ERR_BADSTATE);

    return &s->timing_info;
}

const pa_sample_spec* pa_stream_get_sample_spec(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return &s->sample_spec;
}

const pa_channel_map* pa_stream_get_channel_map(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    return &s->channel_map;
}

const pa_buffer_attr* pa_stream_get_buffer_attr(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 9, PA_ERR_NODATA);

    return &s->buffer_attr;
}

static void stream_set_buffer_attr_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int success = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        success = 0;
    } else {

        if (o->stream->direction == PA_STREAM_PLAYBACK) {
            if (pa_tagstruct_getu32(t, &o->stream->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.tlength) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.prebuf) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.minreq) < 0) {
                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        } else if (o->stream->direction == PA_STREAM_RECORD) {
            if (pa_tagstruct_getu32(t, &o->stream->buffer_attr.maxlength) < 0 ||
                pa_tagstruct_getu32(t, &o->stream->buffer_attr.fragsize) < 0) {
                pa_context_fail(o->context, PA_ERR_PROTOCOL);
                goto finish;
            }
        }

        if (!pa_tagstruct_eof(t)) {
            pa_context_fail(o->context, PA_ERR_PROTOCOL);
            goto finish;
        }

        o->stream->manual_buffer_attr = TRUE;
    }

    if (o->stream->state == PA_STREAM_CREATING) {
        o->stream->buffer_attr_not_ready = FALSE;
        create_stream_complete(o->stream);
    }

    if (o->callback) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}


pa_operation* pa_stream_set_buffer_attr(pa_stream *s, const pa_buffer_attr *attr, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);
    pa_assert(attr);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_RECORD ? PA_COMMAND_SET_RECORD_STREAM_BUFFER_ATTR : PA_COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR,
            &tag);
    pa_tagstruct_putu32(t, s->channel);

    pa_tagstruct_putu32(t, attr->maxlength);

    if (s->direction == PA_STREAM_PLAYBACK)
        pa_tagstruct_put(
                t,
                PA_TAG_U32, attr->tlength,
                PA_TAG_U32, attr->prebuf,
                PA_TAG_U32, attr->minreq,
                PA_TAG_INVALID);
    else
        pa_tagstruct_putu32(t, attr->fragsize);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_set_buffer_attr_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;
}

uint32_t pa_stream_get_device_index(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, PA_INVALID_INDEX);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE, PA_INVALID_INDEX);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED, PA_INVALID_INDEX);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->device_index != PA_INVALID_INDEX, PA_ERR_BADSTATE, PA_INVALID_INDEX);

    return s->device_index;
}

const char *pa_stream_get_device_name(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->device_name, PA_ERR_BADSTATE);

    return s->device_name;
}

int pa_stream_is_suspended(pa_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);

    return s->suspended;
}

static void stream_update_sample_rate_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int success = 1;

    pa_assert(pd);
    pa_assert(o);
    pa_assert(PA_REFCNT_VALUE(o) >= 1);

    if (!o->context)
        goto finish;

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        success = 0;
    } else {

        if (!pa_tagstruct_eof(t)) {
            pa_context_fail(o->context, PA_ERR_PROTOCOL);
            goto finish;
        }
    }

    o->stream->sample_spec.rate = PA_PTR_TO_UINT(o->private);
    pa_assert(pa_sample_spec_valid(&o->stream->sample_spec));

    if (o->callback) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}


pa_operation *pa_stream_update_sample_rate(pa_stream *s, uint32_t rate, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;

    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, rate > 0 && rate <= PA_RATE_MAX, PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->flags & PA_STREAM_VARIABLE_RATE, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->context->version >= 12, PA_ERR_NOTSUPPORTED);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);
    o->private = PA_UINT_TO_PTR(rate);

    t = pa_tagstruct_command(
            s->context,
            s->direction == PA_STREAM_RECORD ? PA_COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE : PA_COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE,
            &tag);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_putu32(t, rate);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_update_sample_rate_callback, pa_operation_ref(o), (pa_free_cb_t) pa_operation_unref);

    return o;

}
