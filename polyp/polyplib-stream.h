#ifndef foopolyplibstreamhfoo
#define foopolyplibstreamhfoo

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

#include <sys/types.h>

#include "sample.h"
#include "polyplib-def.h"
#include "cdecl.h"
#include "polyplib-operation.h"

/** \file
 * Audio streams for input, output and sample upload */

PA_C_DECL_BEGIN

/** \struct pa_stream
 * An opaque stream for playback or recording */
struct pa_stream;

/** Create a new, unconnected stream with the specified name and sample type */
struct pa_stream* pa_stream_new(struct pa_context *c, const char *name, const struct pa_sample_spec *ss);

/** Decrease the reference counter by one */
void pa_stream_unref(struct pa_stream *s);

/** Increase the reference counter by one */
struct pa_stream *pa_stream_ref(struct pa_stream *s);

/** Return the current state of the stream */
enum pa_stream_state pa_stream_get_state(struct pa_stream *p);

/** Return the context this stream is attached to */
struct pa_context* pa_stream_get_context(struct pa_stream *p);

/** Return the device (sink input or source output) index this stream is connected to */
uint32_t pa_stream_get_index(struct pa_stream *s);

/** Connect the stream to a sink */
void pa_stream_connect_playback(struct pa_stream *s, const char *dev, const struct pa_buffer_attr *attr);

/** Connect the stream to a source */
void pa_stream_connect_record(struct pa_stream *s, const char *dev, const struct pa_buffer_attr *attr);

/** Disconnect a stream from a source/sink */
void pa_stream_disconnect(struct pa_stream *s);

/** Write some data to the server (for playback sinks), if free_cb is
 * non-NULL this routine is called when all data has been written out
 * and an internal reference to the specified data is kept, the data
 * is not copied. If NULL, the data is copied into an internal
 * buffer. */ 
void pa_stream_write(struct pa_stream *p, const void *data, size_t length, void (*free_cb)(void *p));

/** Return the amount of bytes that may be written using pa_stream_write() */
size_t pa_stream_writable_size(struct pa_stream *p);

/** Drain a playback stream */
struct pa_operation* pa_stream_drain(struct pa_stream *s, void (*cb) (struct pa_stream*s, int success, void *userdata), void *userdata);

/** Get the playback latency of a stream */
struct pa_operation* pa_stream_get_latency(struct pa_stream *p, void (*cb)(struct pa_stream *p, uint32_t latency, void *userdata), void *userdata);

/** Set the callback function that is called whenever the state of the stream changes */
void pa_stream_set_state_callback(struct pa_stream *s, void (*cb)(struct pa_stream *s, void *userdata), void *userdata);

/** Set the callback function that is called when new data may be written to the stream */
void pa_stream_set_write_callback(struct pa_stream *p, void (*cb)(struct pa_stream *p, size_t length, void *userdata), void *userdata);

/** Set the callback function that is called when new data is available from the stream */
void pa_stream_set_read_callback(struct pa_stream *p, void (*cb)(struct pa_stream *p, const void*data, size_t length, void *userdata), void *userdata);

PA_C_DECL_END

#endif
