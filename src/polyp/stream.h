#ifndef foostreamhfoo
#define foostreamhfoo

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

#include <sys/types.h>

#include <polyp/sample.h>
#include <polyp/channelmap.h>
#include <polyp/volume.h>
#include <polyp/def.h>
#include <polyp/cdecl.h>
#include <polyp/operation.h>

/** \file
 * Audio streams for input, output and sample upload */

PA_C_DECL_BEGIN

/** \struct pa_stream
 * An opaque stream for playback or recording */
typedef struct pa_stream pa_stream;

/** Create a new, unconnected stream with the specified name and sample type */
pa_stream* pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map);

/** Decrease the reference counter by one */
void pa_stream_unref(pa_stream *s);

/** Increase the reference counter by one */
pa_stream *pa_stream_ref(pa_stream *s);

/** Return the current state of the stream */
pa_stream_state_t pa_stream_get_state(pa_stream *p);

/** Return the context this stream is attached to */
pa_context* pa_stream_get_context(pa_stream *p);

/** Return the device (sink input or source output) index this stream is connected to */
uint32_t pa_stream_get_index(pa_stream *s);

/** Connect the stream to a sink */
void pa_stream_connect_playback(
    pa_stream *s,
    const char *dev,
    const pa_buffer_attr *attr,
    pa_stream_flags_t flags,
    pa_cvolume *volume);

/** Connect the stream to a source */
void pa_stream_connect_record(
    pa_stream *s,
    const char *dev,
    const pa_buffer_attr *attr,
    pa_stream_flags_t flags);

/** Disconnect a stream from a source/sink */
void pa_stream_disconnect(pa_stream *s);

/** Write some data to the server (for playback sinks), if free_cb is
 * non-NULL this routine is called when all data has been written out
 * and an internal reference to the specified data is kept, the data
 * is not copied. If NULL, the data is copied into an internal
 * buffer. */ 
void pa_stream_write(pa_stream *p      /**< The stream to use */,
                     const void *data         /**< The data to write */,
                     size_t length            /**< The length of the data to write */,
                     void (*free_cb)(void *p) /**< A cleanup routine for the data or NULL to request an internal copy */,
                     size_t delta             /**< Drop this many
                                                 bytes in the playback
                                                 buffer before writing
                                                 this data. Use
                                                 (size_t) -1 for
                                                 clearing the whole
                                                 playback
                                                 buffer. Normally you
                                                 will specify 0 here,
                                                 i.e. append to the
                                                 playback buffer. If
                                                 the value given here
                                                 is greater than the
                                                 buffered data length
                                                 the buffer is cleared
                                                 and the data is
                                                 written to the
                                                 buffer's start. This
                                                 value is ignored on
                                                 upload streams. */);

/** Read the next fragment from the buffer (for capture sources).
 * data will point to the actual data and length will contain the size
 * of the data in bytes (which can be less than a complete framgnet).
 * Use pa_stream_drop() to actually remove the data from the buffer.
 * \since 0.8 */ 
void pa_stream_peek(pa_stream *p                 /**< The stream to use */,
                     void **data                 /**< Pointer to pointer that will point to data */,
                     size_t *length              /**< The length of the data read */);

/** Remove the current fragment. It is invalid to do this without first
 * calling pa_stream_peek(). \since 0.8 */
void pa_stream_drop(pa_stream *p);

/** Return the amount of bytes that may be written using pa_stream_write() */
size_t pa_stream_writable_size(pa_stream *p);

/** Return the ammount of bytes that may be read using pa_stream_read() \since 0.8 */
size_t pa_stream_readable_size(pa_stream *p);

/** Drain a playback stream */
pa_operation* pa_stream_drain(pa_stream *s, void (*cb) (pa_stream*s, int success, void *userdata), void *userdata);

/** Get the playback latency of a stream */
pa_operation* pa_stream_get_latency_info(pa_stream *p, void (*cb)(pa_stream *p, const pa_latency_info *i, void *userdata), void *userdata);

/** Set the callback function that is called whenever the state of the stream changes */
void pa_stream_set_state_callback(pa_stream *s, void (*cb)(pa_stream *s, void *userdata), void *userdata);

/** Set the callback function that is called when new data may be
 * written to the stream. */
void pa_stream_set_write_callback(pa_stream *p, void (*cb)(pa_stream *p, size_t length, void *userdata), void *userdata);

/** Set the callback function that is called when new data is available from the stream.
 * Return the number of bytes read. \since 0.8
 */
void pa_stream_set_read_callback(pa_stream *p, void (*cb)(pa_stream *p, size_t length, void *userdata), void *userdata);

/** Pause (or resume) playback of this stream temporarily. Available on both playback and recording streams. \since 0.3 */
pa_operation* pa_stream_cork(pa_stream *s, int b, void (*cb) (pa_stream*s, int success, void *userdata), void *userdata);

/** Flush the playback buffer of this stream. Most of the time you're
 * better off using the parameter delta of pa_stream_write() instead of this
 * function. Available on both playback and recording streams. \since 0.3 */
pa_operation* pa_stream_flush(pa_stream *s, void (*cb)(pa_stream *s, int success, void *userdata), void *userdata);

/** Reenable prebuffering. Available for playback streams only. \since 0.6 */
pa_operation* pa_stream_prebuf(pa_stream *s, void (*cb)(pa_stream *s, int success, void *userdata), void *userdata);

/** Request immediate start of playback on this stream. This disables
 * prebuffering as specified in the pa_buffer_attr structure. Available for playback streams only. \since
 * 0.3 */
pa_operation* pa_stream_trigger(pa_stream *s, void (*cb)(pa_stream *s, int success, void *userdata), void *userdata);

/** Rename the stream. \since 0.5 */
pa_operation* pa_stream_set_name(pa_stream *s, const char *name, void(*cb)(pa_stream*c, int success,  void *userdata), void *userdata);

/** Return the total number of bytes written to/read from the
 * stream. This counter is not reset on pa_stream_flush(), you may do
 * this yourself using pa_stream_reset_counter(). \since 0.6 */
uint64_t pa_stream_get_counter(pa_stream *s);

/** Return the current playback/recording time. This is based on the
 * counter accessible with pa_stream_get_counter(). This function
 * requires a pa_latency_info structure as argument, which should be
 * acquired using pa_stream_get_latency(). \since 0.6 */
pa_usec_t pa_stream_get_time(pa_stream *s, const pa_latency_info *i);

/** Return the total stream latency. Thus function requires a
 * pa_latency_info structure as argument, which should be aquired
 * using pa_stream_get_latency(). In case the stream is a monitoring
 * stream the result can be negative, i.e. the captured samples are
 * not yet played. In this case *negative is set to 1. \since 0.6 */
pa_usec_t pa_stream_get_latency(pa_stream *s, const pa_latency_info *i, int *negative);

/** Return the interpolated playback/recording time. Requires the
 *  PA_STREAM_INTERPOLATE_LATENCY bit set when creating the stream. In
 *  contrast to pa_stream_get_latency() this function doesn't require
 *  a whole roundtrip for response. \since 0.6 */
pa_usec_t pa_stream_get_interpolated_time(pa_stream *s);

/** Return the interpolated playback/recording latency. Requires the
 * PA_STREAM_INTERPOLATE_LATENCY bit set when creating the
 * stream. \since 0.6 */
pa_usec_t pa_stream_get_interpolated_latency(pa_stream *s, int *negative);

/** Return a pointer to the streams sample specification. \since 0.6 */
const pa_sample_spec* pa_stream_get_sample_spec(pa_stream *s);

PA_C_DECL_END

#endif
