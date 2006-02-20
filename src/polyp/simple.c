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
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include <polyp/polypaudio.h>
#include <polyp/mainloop.h>

#include <polypcore/native-common.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>

#include "simple.h"

struct pa_simple {
    pa_mainloop *mainloop;
    pa_context *context;
    pa_stream *stream;
    pa_stream_direction_t direction;

    int dead;

    const void *read_data;
    size_t read_index, read_length;
    pa_usec_t latency;
};

static int check_error(pa_simple *p, int *rerror) {
    pa_context_state_t cst;
    pa_stream_state_t sst;
    assert(p);
    
    if ((cst = pa_context_get_state(p->context)) == PA_CONTEXT_FAILED)
        goto fail;

    assert(cst != PA_CONTEXT_TERMINATED);

    if (p->stream) {
        if ((sst = pa_stream_get_state(p->stream)) == PA_STREAM_FAILED)
            goto fail;
        
        assert(sst != PA_STREAM_TERMINATED);
    }
    
    return 0;
    
fail:
    if (rerror)
        *rerror = pa_context_errno(p->context);

    p->dead = 1;
    
    return -1;
}

static int iterate(pa_simple *p, int block, int *rerror) {
    assert(p && p->context && p->mainloop);

    if (check_error(p, rerror) < 0)
        return -1;
    
    if (!block && !pa_context_is_pending(p->context))
        return 0;

    do {
        if (pa_mainloop_iterate(p->mainloop, 1, NULL) < 0) {
            if (rerror)
                *rerror = PA_ERR_INTERNAL;
            return -1;
        }

        if (check_error(p, rerror) < 0)
            return -1;
        
    } while (pa_context_is_pending(p->context));

    
    while (pa_mainloop_deferred_pending(p->mainloop)) {

        if (pa_mainloop_iterate(p->mainloop, 0, NULL) < 0) {
            if (rerror)
                *rerror = PA_ERR_INTERNAL;
            return -1;
        }

        if (check_error(p, rerror) < 0)
            return -1;
    }
    
    return 0;
}

pa_simple* pa_simple_new(
    const char *server,
    const char *name,
    pa_stream_direction_t dir,
    const char *dev,
    const char *stream_name,
    const pa_sample_spec *ss,
    const pa_buffer_attr *attr,
    int *rerror) {
    
    pa_simple *p;
    int error = PA_ERR_INTERNAL;
    assert(ss && (dir == PA_STREAM_PLAYBACK || dir == PA_STREAM_RECORD));

    p = pa_xmalloc(sizeof(pa_simple));
    p->context = NULL;
    p->stream = NULL;
    p->mainloop = pa_mainloop_new();
    assert(p->mainloop);
    p->dead = 0;
    p->direction = dir;
    p->read_data = NULL;
    p->read_index = p->read_length = 0;
    p->latency = 0;

    if (!(p->context = pa_context_new(pa_mainloop_get_api(p->mainloop), name)))
        goto fail;
    
    pa_context_connect(p->context, server, 1, NULL);

    /* Wait until the context is ready */
    while (pa_context_get_state(p->context) != PA_CONTEXT_READY) {
        if (iterate(p, 1, &error) < 0)
            goto fail;
    }

    if (!(p->stream = pa_stream_new(p->context, stream_name, ss, NULL)))
        goto fail;

    if (dir == PA_STREAM_PLAYBACK)
        pa_stream_connect_playback(p->stream, dev, attr, 0, NULL, NULL);
    else
        pa_stream_connect_record(p->stream, dev, attr, 0);

    /* Wait until the stream is ready */
    while (pa_stream_get_state(p->stream) != PA_STREAM_READY) {
        if (iterate(p, 1, &error) < 0)
            goto fail;
    }

    return p;
    
fail:
    if (rerror)
        *rerror = error;
    pa_simple_free(p);
    return NULL;
}

void pa_simple_free(pa_simple *s) {
    assert(s);

    if (s->stream)
        pa_stream_unref(s->stream);
    
    if (s->context)
        pa_context_unref(s->context);

    if (s->mainloop)
        pa_mainloop_free(s->mainloop);

    pa_xfree(s);
}

int pa_simple_write(pa_simple *p, const void*data, size_t length, int *rerror) {
    assert(p && data && p->direction == PA_STREAM_PLAYBACK);

    if (p->dead) {
        if (rerror)
            *rerror = pa_context_errno(p->context);
        
        return -1;
    }

    while (length > 0) {
        size_t l;
        
        while (!(l = pa_stream_writable_size(p->stream)))
            if (iterate(p, 1, rerror) < 0)
                return -1;

        if (l > length)
            l = length;

        pa_stream_write(p->stream, data, l, NULL, 0, PA_SEEK_RELATIVE);
        data = (const uint8_t*) data + l;
        length -= l;
    }

    /* Make sure that no data is pending for write */
    if (iterate(p, 0, rerror) < 0)
        return -1;

    return 0;
}

int pa_simple_read(pa_simple *p, void*data, size_t length, int *rerror) {
    assert(p && data && p->direction == PA_STREAM_RECORD);

    if (p->dead) {
        if (rerror)
            *rerror = pa_context_errno(p->context);
        
        return -1;
    }
    
    while (length > 0) {

        if (!p->read_data) 
            if (pa_stream_peek(p->stream, &p->read_data, &p->read_length) >= 0)
                p->read_index = 0;
        
        if (p->read_data) {
            size_t l = length;

            if (p->read_length <= l)
                l = p->read_length;

            memcpy(data, (const uint8_t*) p->read_data+p->read_index, l);

            data = (uint8_t*) data + l;
            length -= l;
            
            p->read_index += l;
            p->read_length -= l;

            if (!p->read_length) {
                pa_stream_drop(p->stream);
                p->read_data = NULL;
                p->read_length = 0;
                p->read_index = 0;
            }
            
            if (!length)
                return 0;

            assert(!p->read_data);
        }

        if (iterate(p, 1, rerror) < 0)
            return -1;
    }

    return 0;
}

static void drain_or_flush_complete(pa_stream *s, int success, void *userdata) {
    pa_simple *p = userdata;
    assert(s && p);
    if (!success)
        p->dead = 1;
}

int pa_simple_drain(pa_simple *p, int *rerror) {
    pa_operation *o;
    assert(p && p->direction == PA_STREAM_PLAYBACK);

    if (p->dead) {
        if (rerror)
            *rerror = pa_context_errno(p->context);
        
        return -1;
    }

    o = pa_stream_drain(p->stream, drain_or_flush_complete, p);

    while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
        if (iterate(p, 1, rerror) < 0) {
            pa_operation_cancel(o);
            pa_operation_unref(o);
            return -1;
        }
    }

    pa_operation_unref(o);

    if (p->dead && rerror)
        *rerror = pa_context_errno(p->context);

    return p->dead ? -1 : 0;
}

static void latency_complete(pa_stream *s, const pa_latency_info *l, void *userdata) {
    pa_simple *p = userdata;
    assert(s && p);

    if (!l)
        p->dead = 1;
    else {
        int negative = 0;
        p->latency = pa_stream_get_latency(s, l, &negative);
        if (negative)
            p->latency = 0;
    }
}

pa_usec_t pa_simple_get_playback_latency(pa_simple *p, int *rerror) {
    pa_operation *o;
    assert(p && p->direction == PA_STREAM_PLAYBACK);

    if (p->dead) {
        if (rerror)
            *rerror = pa_context_errno(p->context);
        
        return (pa_usec_t) -1;
    }

    p->latency = 0;
    o = pa_stream_get_latency_info(p->stream, latency_complete, p);
    
    while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {

        if (iterate(p, 1, rerror) < 0) {
            pa_operation_cancel(o);
            pa_operation_unref(o);
            return -1;
        }
    }

    pa_operation_unref(o);
    
    if (p->dead && rerror)
        *rerror = pa_context_errno(p->context);

    return p->dead ? (pa_usec_t) -1 : p->latency;
}

int pa_simple_flush(pa_simple *p, int *rerror) {
    pa_operation *o;
    assert(p && p->direction == PA_STREAM_PLAYBACK);

    if (p->dead) {
        if (rerror)
            *rerror = pa_context_errno(p->context);
        
        return -1;
    }

    o = pa_stream_flush(p->stream, drain_or_flush_complete, p);

    while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
        if (iterate(p, 1, rerror) < 0) {
            pa_operation_cancel(o);
            pa_operation_unref(o);
            return -1;
        }
    }

    pa_operation_unref(o);

    if (p->dead && rerror)
        *rerror = pa_context_errno(p->context);

    return p->dead ? -1 : 0;
}
