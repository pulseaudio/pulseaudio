#ifndef foostreamhfoo
#define foostreamhfoo

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

#include <sys/types.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>
#include <pulse/def.h>
#include <pulse/cdecl.h>
#include <pulse/operation.h>

/** \page streams Audio Streams
 *
 * \section overv_sec Overview
 *
 * Audio streams form the central functionality of the sound server. Data is
 * routed, converted and mixed from several sources before it is passed along
 * to a final output. Currently, there are three forms of audio streams:
 *
 * \li Playback streams - Data flows from the client to the server.
 * \li Record streams - Data flows from the server to the client.
 * \li Upload streams - Similar to playback streams, but the data is stored in
 *                      the sample cache. See \ref scache for more information
 *                      about controlling the sample cache.
 *
 * \section create_sec Creating
 *
 * To access a stream, a pa_stream object must be created using
 * pa_stream_new(). At this point the audio sample format and mapping of
 * channels must be specified. See \ref sample and \ref channelmap for more
 * information about those structures.
 *
 * This first step will only create a client-side object, representing the
 * stream. To use the stream, a server-side object must be created and
 * associated with the local object. Depending on which type of stream is
 * desired, a different function is needed:
 *
 * \li Playback stream - pa_stream_connect_playback()
 * \li Record stream - pa_stream_connect_record()
 * \li Upload stream - pa_stream_connect_upload() (see \ref scache)
 *
 * Similar to how connections are done in contexts, connecting a stream will
 * not generate a pa_operation object. Also like contexts, the application
 * should register a state change callback, using
 * pa_stream_set_state_callback(), and wait for the stream to enter an active
 * state.
 *
 * \subsection bufattr_subsec Buffer Attributes
 *
 * Playback and record streams always have a server side buffer as
 * part of the data flow.  The size of this buffer strikes a
 * compromise between low latency and sensitivity for buffer
 * overflows/underruns.
 *
 * The buffer metrics may be controlled by the application. They are
 * described with a pa_buffer_attr structure which contains a number
 * of fields:
 *
 * \li maxlength - The absolute maximum number of bytes that can be stored in
 *                 the buffer. If this value is exceeded then data will be
 *                 lost.
 * \li tlength - The target length of a playback buffer. The server will only
 *               send requests for more data as long as the buffer has less
 *               than this number of bytes of data.
 * \li prebuf - Number of bytes that need to be in the buffer before
 * playback will commence. Start of playback can be forced using
 * pa_stream_trigger() even though the prebuffer size hasn't been
 * reached. If a buffer underrun occurs, this prebuffering will be
 * again enabled. If the playback shall never stop in case of a buffer
 * underrun, this value should be set to 0. In that case the read
 * index of the output buffer overtakes the write index, and hence the
 * fill level of the buffer is negative.
 * \li minreq - Minimum free number of the bytes in the playback buffer before
 *              the server will request more data.
 * \li fragsize - Maximum number of bytes that the server will push in one
 *                chunk for record streams.
 *
 * The server side playback buffers are indexed by a write and a read
 * index. The application writes to the write index and the sound
 * device reads from the read index. The read index is increased
 * monotonically, while the write index may be freely controlled by
 * the application. Substracting the read index from the write index
 * will give you the current fill level of the buffer. The read/write
 * indexes are 64bit values and measured in bytes, they will never
 * wrap. The current read/write index may be queried using
 * pa_stream_get_timing_info() (see below for more information). In
 * case of a buffer underrun the read index is equal or larger than
 * the write index. Unless the prebuf value is 0, PulseAudio will
 * temporarily pause playback in such a case, and wait until the
 * buffer is filled up to prebuf bytes again. If prebuf is 0, the
 * read index may be larger than the write index, in which case
 * silence is played. If the application writes data to indexes lower
 * than the read index, the data is immediately lost.
 *
 * \section transfer_sec Transferring Data
 *
 * Once the stream is up, data can start flowing between the client and the
 * server. Two different access models can be used to transfer the data:
 *
 * \li Asynchronous - The application register a callback using
 *                    pa_stream_set_write_callback() and
 *                    pa_stream_set_read_callback() to receive notifications
 *                    that data can either be written or read.
 * \li Polled - Query the library for available data/space using
 *              pa_stream_writable_size() and pa_stream_readable_size() and
 *              transfer data as needed. The sizes are stored locally, in the
 *              client end, so there is no delay when reading them.
 *
 * It is also possible to mix the two models freely.
 *
 * Once there is data/space available, it can be transferred using either
 * pa_stream_write() for playback, or pa_stream_peek() / pa_stream_drop() for
 * record. Make sure you do not overflow the playback buffers as data will be
 * dropped.
 *
 * \section bufctl_sec Buffer Control
 *
 * The transfer buffers can be controlled through a number of operations:
 *
 * \li pa_stream_cork() - Start or stop the playback or recording.
 * \li pa_stream_trigger() - Start playback immediatly and do not wait for
 *                           the buffer to fill up to the set trigger level.
 * \li pa_stream_prebuf() - Reenable the playback trigger level.
 * \li pa_stream_drain() - Wait for the playback buffer to go empty. Will
 *                         return a pa_operation object that will indicate when
 *                         the buffer is completely drained.
 * \li pa_stream_flush() - Drop all data from the playback buffer and do not
 *                         wait for it to finish playing.
 *
 * \section seek_modes Seeking in the Playback Buffer
 *
 * A client application may freely seek in the playback buffer. To
 * accomplish that the pa_stream_write() function takes a seek mode
 * and an offset argument. The seek mode is one of:
 *
 * \li PA_SEEK_RELATIVE - seek relative to the current write index
 * \li PA_SEEK_ABSOLUTE - seek relative to the beginning of the playback buffer, (i.e. the first that was ever played in the stream)
 * \li PA_SEEK_RELATIVE_ON_READ - seek relative to the current read index. Use this to write data to the output buffer that should be played as soon as possible
 * \li PA_SEEK_RELATIVE_END - seek relative to the last byte ever written.
 *
 * If an application just wants to append some data to the output
 * buffer, PA_SEEK_RELATIVE and an offset of 0 should be used.
 *
 * After a call to pa_stream_write() the write index will be left at
 * the position right after the last byte of the written data.
 *
 * \section latency_sec Latency
 *
 * A major problem with networked audio is the increased latency caused by
 * the network. To remedy this, PulseAudio supports an advanced system of
 * monitoring the current latency.
 *
 * To get the raw data needed to calculate latencies, call
 * pa_stream_get_timing_info(). This will give you a pa_timing_info
 * structure that contains everything that is known about the server
 * side buffer transport delays and the backend active in the
 * server. (Besides other things it contains the write and read index
 * values mentioned above.)
 *
 * This structure is updated every time a
 * pa_stream_update_timing_info() operation is executed. (i.e. before
 * the first call to this function the timing information structure is
 * not available!) Since it is a lot of work to keep this structure
 * up-to-date manually, PulseAudio can do that automatically for you:
 * if PA_STREAM_AUTO_TIMING_UPDATE is passed when connecting the
 * stream PulseAudio will automatically update the structure every
 * 100ms and every time a function is called that might invalidate the
 * previously known timing data (such as pa_stream_write() or
 * pa_stream_flush()). Please note however, that there always is a
 * short time window when the data in the timing information structure
 * is out-of-date. PulseAudio tries to mark these situations by
 * setting the write_index_corrupt and read_index_corrupt fields
 * accordingly.
 *
 * The raw timing data in the pa_timing_info structure is usually hard
 * to deal with. Therefore a more simplistic interface is available:
 * you can call pa_stream_get_time() or pa_stream_get_latency(). The
 * former will return the current playback time of the hardware since
 * the stream has been started. The latter returns the time a sample
 * that you write now takes to be played by the hardware. These two
 * functions base their calculations on the same data that is returned
 * by pa_stream_get_timing_info(). Hence the same rules for keeping
 * the timing data up-to-date apply here. In case the write or read
 * index is corrupted, these two functions will fail with
 * PA_ERR_NODATA set.
 *
 * Since updating the timing info structure usually requires a full
 * network round trip and some applications monitor the timing very
 * often PulseAudio offers a timing interpolation system. If
 * PA_STREAM_INTERPOLATE_TIMING is passed when connecting the stream,
 * pa_stream_get_time() and pa_stream_get_latency() will try to
 * interpolate the current playback time/latency by estimating the
 * number of samples that have been played back by the hardware since
 * the last regular timing update. It is espcially useful to combine
 * this option with PA_STREAM_AUTO_TIMING_UPDATE, which will enable
 * you to monitor the current playback time/latency very precisely and
 * very frequently without requiring a network round trip every time.
 *
 * \section flow_sec Overflow and underflow
 *
 * Even with the best precautions, buffers will sometime over - or
 * underflow.  To handle this gracefully, the application can be
 * notified when this happens. Callbacks are registered using
 * pa_stream_set_overflow_callback() and
 * pa_stream_set_underflow_callback().
 *
 * \section sync_streams Sychronizing Multiple Playback Streams
 *
 * PulseAudio allows applications to fully synchronize multiple
 * playback streams that are connected to the same output device. That
 * means the streams will always be played back sample-by-sample
 * synchronously. If stream operations like pa_stream_cork() are
 * issued on one of the synchronized streams, they are simultaneously
 * issued on the others.
 *
 * To synchronize a stream to another, just pass the "master" stream
 * as last argument to pa_stream_connect_playack(). To make sure that
 * the freshly created stream doesn't start playback right-away, make
 * sure to pass PA_STREAM_START_CORKED and - after all streams have
 * been created - uncork them all with a single call to
 * pa_stream_cork() for the master stream.
 *
 * To make sure that a particular stream doesn't stop to play when a
 * server side buffer underrun happens on it while the other
 * synchronized streams continue playing and hence deviate you need to
 * pass a "prebuf" pa_buffer_attr of 0 when connecting it.
 *
 * \section disc_sec Disconnecting
 *
 * When a stream has served is purpose it must be disconnected with
 * pa_stream_disconnect(). If you only unreference it, then it will live on
 * and eat resources both locally and on the server until you disconnect the
 * context.
 *
 */

/** \file
 * Audio streams for input, output and sample upload */

PA_C_DECL_BEGIN

/** An opaque stream for playback or recording */
typedef struct pa_stream pa_stream;

/** A generic callback for operation completion */
typedef void (*pa_stream_success_cb_t) (pa_stream*s, int success, void *userdata);

/** A generic request callback */
typedef void (*pa_stream_request_cb_t)(pa_stream *p, size_t length, void *userdata);

/** A generic notification callback */
typedef void (*pa_stream_notify_cb_t)(pa_stream *p, void *userdata);

/** Create a new, unconnected stream with the specified name and sample type */
pa_stream* pa_stream_new(
        pa_context *c                     /**< The context to create this stream in */,
        const char *name                  /**< A name for this stream */,
        const pa_sample_spec *ss          /**< The desired sample format */,
        const pa_channel_map *map         /**< The desired channel map, or NULL for default */);

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
int pa_stream_connect_playback(
        pa_stream *s                  /**< The stream to connect to a sink */,
        const char *dev               /**< Name of the sink to connect to, or NULL for default */ ,
        const pa_buffer_attr *attr    /**< Buffering attributes, or NULL for default */,
        pa_stream_flags_t flags       /**< Additional flags, or 0 for default */,
        pa_cvolume *volume            /**< Initial volume, or NULL for default */,
        pa_stream *sync_stream        /**< Synchronize this stream with the specified one, or NULL for a standalone stream*/);

/** Connect the stream to a source */
int pa_stream_connect_record(
        pa_stream *s                  /**< The stream to connect to a source */ ,
        const char *dev               /**< Name of the source to connect to, or NULL for default */,
        const pa_buffer_attr *attr    /**< Buffer attributes, or NULL for default */,
        pa_stream_flags_t flags       /**< Additional flags, or 0 for default */);

/** Disconnect a stream from a source/sink */
int pa_stream_disconnect(pa_stream *s);

/** Write some data to the server (for playback sinks), if free_cb is
 * non-NULL this routine is called when all data has been written out
 * and an internal reference to the specified data is kept, the data
 * is not copied. If NULL, the data is copied into an internal
 * buffer. The client my freely seek around in the output buffer. For
 * most applications passing 0 and PA_SEEK_RELATIVE as arguments for
 * offset and seek should be useful.*/
int pa_stream_write(
        pa_stream *p             /**< The stream to use */,
        const void *data         /**< The data to write */,
        size_t length            /**< The length of the data to write */,
        pa_free_cb_t free_cb     /**< A cleanup routine for the data or NULL to request an internal copy */,
        int64_t offset,          /**< Offset for seeking, must be 0 for upload streams */
        pa_seek_mode_t seek      /**< Seek mode, must be PA_SEEK_RELATIVE for upload streams */);

/** Read the next fragment from the buffer (for recording).
 * data will point to the actual data and length will contain the size
 * of the data in bytes (which can be less than a complete framgnet).
 * Use pa_stream_drop() to actually remove the data from the
 * buffer. If no data is available will return a NULL pointer  \since 0.8 */
int pa_stream_peek(
        pa_stream *p                 /**< The stream to use */,
        const void **data            /**< Pointer to pointer that will point to data */,
        size_t *length              /**< The length of the data read */);

/** Remove the current fragment on record streams. It is invalid to do this without first
 * calling pa_stream_peek(). \since 0.8 */
int pa_stream_drop(pa_stream *p);

/** Return the nember of bytes that may be written using pa_stream_write() */
size_t pa_stream_writable_size(pa_stream *p);

/** Return the number of bytes that may be read using pa_stream_read() \since 0.8 */
size_t pa_stream_readable_size(pa_stream *p);

/** Drain a playback stream. Use this for notification when the buffer is empty */
pa_operation* pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata);

/** Request a timing info structure update for a stream. Use
 * pa_stream_get_timing_info() to get access to the raw timing data,
 * or pa_stream_get_time() or pa_stream_get_latency() to get cleaned
 * up values. */
pa_operation* pa_stream_update_timing_info(pa_stream *p, pa_stream_success_cb_t cb, void *userdata);

/** Set the callback function that is called whenever the state of the stream changes */
void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata);

/** Set the callback function that is called when new data may be
 * written to the stream. */
void pa_stream_set_write_callback(pa_stream *p, pa_stream_request_cb_t cb, void *userdata);

/** Set the callback function that is called when new data is available from the stream.
 * Return the number of bytes read. \since 0.8 */
void pa_stream_set_read_callback(pa_stream *p, pa_stream_request_cb_t cb, void *userdata);

/** Set the callback function that is called when a buffer overflow happens. (Only for playback streams) \since 0.8 */
void pa_stream_set_overflow_callback(pa_stream *p, pa_stream_notify_cb_t cb, void *userdata);

/** Set the callback function that is called when a buffer underflow happens. (Only for playback streams) \since 0.8 */
void pa_stream_set_underflow_callback(pa_stream *p, pa_stream_notify_cb_t cb, void *userdata);

/** Set the callback function that is called whenever a latency information update happens. Useful on PA_STREAM_AUTO_TIMING_UPDATE streams only. (Only for playback streams) \since 0.8.2 */
void pa_stream_set_latency_update_callback(pa_stream *p, pa_stream_notify_cb_t cb, void *userdata);

/** Pause (or resume) playback of this stream temporarily. Available on both playback and recording streams. \since 0.3 */
pa_operation* pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata);

/** Flush the playback buffer of this stream. Most of the time you're
 * better off using the parameter delta of pa_stream_write() instead of this
 * function. Available on both playback and recording streams. \since 0.3 */
pa_operation* pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata);

/** Reenable prebuffering as specified in the pa_buffer_attr
 * structure. Available for playback streams only. \since 0.6 */
pa_operation* pa_stream_prebuf(pa_stream *s, pa_stream_success_cb_t cb, void *userdata);

/** Request immediate start of playback on this stream. This disables
 * prebuffering as specified in the pa_buffer_attr
 * structure, temporarily. Available for playback streams only. \since 0.3 */
pa_operation* pa_stream_trigger(pa_stream *s, pa_stream_success_cb_t cb, void *userdata);

/** Rename the stream. \since 0.5 */
pa_operation* pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb, void *userdata);

/** Return the current playback/recording time. This is based on the
 * data in the timing info structure returned by
 * pa_stream_get_timing_info(). This function will usually only return
 * new data if a timing info update has been recieved. Only if timing
 * interpolation has been requested (PA_STREAM_INTERPOLATE_TIMING)
 * the data from the last timing update is used for an estimation of
 * the current playback/recording time based on the local time that
 * passed since the timing info structure has been acquired. The time
 * value returned by this function is guaranteed to increase
 * monotonically. (that means: the returned value is always greater or
 * equal to the value returned on the last call) This behaviour can
 * be disabled by using PA_STREAM_NOT_MONOTONOUS. This may be
 * desirable to deal better with bad estimations of transport
 * latencies, but may have strange effects if the application is not
 * able to deal with time going 'backwards'. \since 0.6 */
int pa_stream_get_time(pa_stream *s, pa_usec_t *r_usec);

/** Return the total stream latency. This function is based on
 * pa_stream_get_time(). In case the stream is a monitoring stream the
 * result can be negative, i.e. the captured samples are not yet
 * played. In this case *negative is set to 1. \since 0.6 */
int pa_stream_get_latency(pa_stream *s, pa_usec_t *r_usec, int *negative);

/** Return the latest raw timing data structure. The returned pointer
 * points to an internal read-only instance of the timing
 * structure. The user should make a copy of this structure if he
 * wants to modify it. An in-place update to this data structure may
 * be requested using pa_stream_update_timing_info(). If no
 * pa_stream_update_timing_info() call was issued before, this
 * function will fail with PA_ERR_NODATA. Please note that the
 * write_index member field (and only this field) is updated on each
 * pa_stream_write() call, not just when a timing update has been
 * recieved. \since 0.8 */
const pa_timing_info* pa_stream_get_timing_info(pa_stream *s);

/** Return a pointer to the stream's sample specification. \since 0.6 */
const pa_sample_spec* pa_stream_get_sample_spec(pa_stream *s);

/** Return a pointer to the stream's channel map. \since 0.8 */
const pa_channel_map* pa_stream_get_channel_map(pa_stream *s);

/** Return the buffer metrics of the stream. Only valid after the
 * stream has been connected successfuly and if the server is at least
 * PulseAudio 0.9. \since 0.9.0 */
const pa_buffer_attr* pa_stream_get_buffer_attr(pa_stream *s);

PA_C_DECL_END

#endif
