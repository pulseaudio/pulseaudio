#ifndef foodefhfoo
#define foodefhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>
#include <sys/time.h>
#include <time.h>

#include <polyp/cdecl.h>
#include <polyp/sample.h>

/** \file
 * Global definitions */

PA_C_DECL_BEGIN

/** The state of a connection context */
typedef enum pa_context_state {
    PA_CONTEXT_UNCONNECTED,    /**< The context hasn't been connected yet */
    PA_CONTEXT_CONNECTING,     /**< A connection is being established */
    PA_CONTEXT_AUTHORIZING,    /**< The client is authorizing itself to the daemon */
    PA_CONTEXT_SETTING_NAME,   /**< The client is passing its application name to the daemon */
    PA_CONTEXT_READY,          /**< The connection is established, the context is ready to execute operations */
    PA_CONTEXT_FAILED,         /**< The connection failed or was disconnected */
    PA_CONTEXT_TERMINATED      /**< The connection was terminated cleanly */
} pa_context_state_t;

/** The state of a stream */
typedef enum pa_stream_state {
    PA_STREAM_UNCONNECTED, /**< The stream is not yet connected to any sink or source */
    PA_STREAM_CREATING,     /**< The stream is being created */
    PA_STREAM_READY,        /**< The stream is established, you may pass audio data to it now */
    PA_STREAM_FAILED,       /**< An error occured that made the stream invalid */
    PA_STREAM_TERMINATED    /**< The stream has been terminated cleanly */
} pa_stream_state_t;

/** The state of an operation */
typedef enum pa_operation_state {
    PA_OPERATION_RUNNING,      /**< The operation is still running */
    PA_OPERATION_DONE,         /**< The operation has been completed */
    PA_OPERATION_CANCELED      /**< The operation has been canceled */
} pa_operation_state_t;

/** An invalid index */
#define PA_INVALID_INDEX ((uint32_t) -1)

/** Some special flags for contexts. \since 0.8 */
typedef enum pa_context_flags {
    PA_CONTEXT_NOAUTOSPAWN = 1 /**< Disabled autospawning of the polypaudio daemon if required */
} pa_context_flags_t;

/** The direction of a pa_stream object */ 
typedef enum pa_stream_direction {
    PA_STREAM_NODIRECTION,   /**< Invalid direction */
    PA_STREAM_PLAYBACK,      /**< Playback stream */
    PA_STREAM_RECORD,        /**< Record stream */
    PA_STREAM_UPLOAD         /**< Sample upload stream */
} pa_stream_direction_t;

/** Some special flags for stream connections. \since 0.6 */
typedef enum pa_stream_flags {
    PA_STREAM_START_CORKED = 1,       /**< Create the stream corked, requiring an explicit pa_stream_cork() call to uncork it. */
    PA_STREAM_INTERPOLATE_LATENCY = 2, /**< Interpolate the latency for
                                       * this stream. When enabled,
                                       * you can use
                                       * pa_stream_interpolated_xxx()
                                       * for synchronization. Using
                                       * these functions instead of
                                       * pa_stream_get_latency() has
                                       * the advantage of not
                                       * requiring a whole roundtrip
                                       * for responses. Consider using
                                       * this option when frequently
                                       * requesting latency
                                       * information. This is
                                       * especially useful on long latency
                                       * network connections. */
    PA_STREAM_NOT_MONOTONOUS = 4,    /**< Don't force the time to run monotonically */
} pa_stream_flags_t;

/** Playback and record buffer metrics */
typedef struct pa_buffer_attr {
    uint32_t maxlength;      /**< Maximum length of the buffer */
    uint32_t tlength;        /**< Playback only: target length of the buffer. The server tries to assure that at least tlength bytes are always available in the buffer */
    uint32_t prebuf;         /**< Playback only: pre-buffering. The server does not start with playback before at least prebug bytes are available in the buffer */
    uint32_t minreq;         /**< Playback only: minimum request. The server does not request less than minreq bytes from the client, instead waints until the buffer is free enough to request more bytes at once */
    uint32_t fragsize;       /**< Recording only: fragment size. The server sends data in blocks of fragsize bytes size. Large values deminish interactivity with other operations on the connection context but decrease control overhead. */
} pa_buffer_attr;

/** Error values as used by pa_context_errno(). Use pa_strerror() to convert these values to human readable strings */
enum {
    PA_OK = 0,                     /**< No error */
    PA_ERR_ACCESS,                 /**< Access failure */
    PA_ERR_COMMAND,                /**< Unknown command */
    PA_ERR_INVALID,                /**< Invalid argument */
    PA_ERR_EXIST,                  /**< Entity exists */
    PA_ERR_NOENTITY,               /**< No such entity */
    PA_ERR_CONNECTIONREFUSED,      /**< Connection refused */
    PA_ERR_PROTOCOL,               /**< Protocol error */ 
    PA_ERR_TIMEOUT,                /**< Timeout */
    PA_ERR_AUTHKEY,                /**< No authorization key */
    PA_ERR_INTERNAL,               /**< Internal error */
    PA_ERR_CONNECTIONTERMINATED,   /**< Connection terminated */
    PA_ERR_KILLED,                 /**< Entity killed */
    PA_ERR_INVALIDSERVER,          /**< Invalid server */
    PA_ERR_MODINITFAILED,          /**< Module initialization failed */
    PA_ERR_BADSTATE,               /**< Bad state */
    PA_ERR_NODATA,                 /**< No data */
    PA_ERR_VERSION,                /**< Incompatible protocol version \since 0.8 */
    PA_ERR_MAX                     /**< Not really an error but the first invalid error code */
};

/** Subscription event mask, as used by pa_context_subscribe() */
typedef enum pa_subscription_mask {
    PA_SUBSCRIPTION_MASK_NULL = 0,               /**< No events */
    PA_SUBSCRIPTION_MASK_SINK = 1,               /**< Sink events */
    PA_SUBSCRIPTION_MASK_SOURCE = 2,             /**< Source events */
    PA_SUBSCRIPTION_MASK_SINK_INPUT = 4,         /**< Sink input events */
    PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT = 8,      /**< Source output events */
    PA_SUBSCRIPTION_MASK_MODULE = 16,            /**< Module events */
    PA_SUBSCRIPTION_MASK_CLIENT = 32,            /**< Client events */
    PA_SUBSCRIPTION_MASK_SAMPLE_CACHE = 64,      /**< Sample cache events */
    PA_SUBSCRIPTION_MASK_SERVER = 128,           /**< Other global server changes. \since 0.4 */
    PA_SUBSCRIPTION_MASK_AUTOLOAD = 256,         /**< Autoload table events. \since 0.5 */
    PA_SUBSCRIPTION_MASK_ALL = 511               /**< Catch all events \since 0.8 */
} pa_subscription_mask_t;

/** Subscription event types, as used by pa_context_subscribe() */
typedef enum pa_subscription_event_type {
    PA_SUBSCRIPTION_EVENT_SINK = 0,           /**< Event type: Sink */
    PA_SUBSCRIPTION_EVENT_SOURCE = 1,         /**< Event type: Source */
    PA_SUBSCRIPTION_EVENT_SINK_INPUT = 2,     /**< Event type: Sink input */
    PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT = 3,  /**< Event type: Source output */
    PA_SUBSCRIPTION_EVENT_MODULE = 4,         /**< Event type: Module */
    PA_SUBSCRIPTION_EVENT_CLIENT = 5,         /**< Event type: Client */
    PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE = 6,   /**< Event type: Sample cache item */
    PA_SUBSCRIPTION_EVENT_SERVER = 7,         /**< Event type: Global server change, only occuring with PA_SUBSCRIPTION_EVENT_CHANGE. \since 0.4  */
    PA_SUBSCRIPTION_EVENT_AUTOLOAD = 8,       /**< Event type: Autoload table changes. \since 0.5 */
    PA_SUBSCRIPTION_EVENT_FACILITY_MASK = 15, /**< A mask to extract the event type from an event value */

    PA_SUBSCRIPTION_EVENT_NEW = 0,            /**< A new object was created */
    PA_SUBSCRIPTION_EVENT_CHANGE = 16,        /**< A property of the object was modified */
    PA_SUBSCRIPTION_EVENT_REMOVE = 32,        /**< An object was removed */
    PA_SUBSCRIPTION_EVENT_TYPE_MASK = 16+32   /**< A mask to extract the event operation from an event value */
} pa_subscription_event_type_t;

/** Return one if an event type t matches an event mask bitfield */
#define pa_subscription_match_flags(m, t) (!!((m) & (1 << ((t) & PA_SUBSCRIPTION_EVENT_FACILITY_MASK))))

/** A structure for latency info. See pa_stream_get_latency(). The
 * total output latency a sample that is written with
 * pa_stream_write() takes to be played may be estimated by
 * sink_usec+buffer_usec+transport_usec. The output buffer to which
 * buffer_usec relates may be manipulated freely (with
 * pa_stream_write()'s seek argument, pa_stream_flush() and friends),
 * the buffers sink_usec/source_usec relates to is a first-in
 * first-out buffer which cannot be flushed or manipulated in any
 * way. The total input latency a sample that is recorded takes to be
 * delivered to the application is:
 * source_usec+buffer_usec+transport_usec-sink_usec. (Take care of
 * sign issues!) When connected to a monitor source sink_usec contains
 * the latency of the owning sink.*/
typedef struct pa_latency_info {
    struct timeval timestamp; /**< The time when this latency info was current */
    int synchronized_clocks;  /**< Non-zero if the local and the
                               * remote machine have synchronized
                               * clocks. If synchronized clocks are
                               * detected transport_usec becomes much
                               * more reliable. However, the code that
                               * detects synchronized clocks is very
                               * limited und unreliable itself. \since
                               * 0.5 */

    pa_usec_t buffer_usec;    /**< Time in usecs the current buffer takes to play. For both playback and record streams. */
    pa_usec_t sink_usec;      /**< Time in usecs a sample takes to be played on the sink. For playback streams and record streams connected to a monitor source. */
    pa_usec_t source_usec;    /**< Time in usecs a sample takes from being recorded to being delivered to the application. Only for record streams. \since 0.5*/
    pa_usec_t transport_usec; /**< Estimated time in usecs a sample takes to be transferred to/from the daemon. For both playback and record streams. \since 0.5 */

    int playing;              /**< Non-zero when the stream is currently playing. Only for playback streams. */

    int write_index_corrupt;  /**< Non-Zero if the write_index is not up to date because a local write command corrupted it */
    int64_t write_index;      /**< Current write index into the
                               * playback buffer in bytes. Think twice before
                               * using this for seeking purposes: it
                               * might be out of date a the time you
                               * want to use it. Consider using
                               * PA_SEEK_RELATIVE instead. \since
                               * 0.8 */ 
    int64_t read_index;       /**< Current read index into the
                               * playback buffer in bytes. Think twice before
                               * using this for seeking purposes: it
                               * might be out of date a the time you
                               * want to use it. Consider using
                               * PA_SEEK_RELATIVE_ON_READ
                               * instead. \since 0.8 */

    uint32_t buffer_length;   /* Current buffer length. This is usually identical to write_index-read_index. */
} pa_latency_info;

/** A structure for the spawn api. This may be used to integrate auto
 * spawned daemons into your application. For more information see
 * pa_context_connect(). When spawning a new child process the
 * waitpid() is used on the child's PID. The spawn routine will not
 * block or ignore SIGCHLD signals, since this cannot be done in a
 * thread compatible way. You might have to do this in
 * prefork/postfork. \since 0.4 */
typedef struct pa_spawn_api {
    void (*prefork)(void);     /**< Is called just before the fork in the parent process. May be NULL. */
    void (*postfork)(void);    /**< Is called immediately after the fork in the parent process. May be NULL.*/
    void (*atfork)(void);      /**< Is called immediately after the
                                * fork in the child process. May be
                                * NULL. It is not safe to close all
                                * file descriptors in this function
                                * unconditionally, since a UNIX socket
                                * (created using socketpair()) is
                                * passed to the new process. */
} pa_spawn_api;

/** Seek type \since 0.8*/
typedef enum pa_seek_mode {
    PA_SEEK_RELATIVE = 0,           /**< Seek relatively to the write index */
    PA_SEEK_ABSOLUTE = 1,           /**< Seek relatively to the start of the buffer queue */  
    PA_SEEK_RELATIVE_ON_READ = 2,   /**< Seek relatively to the read index */
    PA_SEEK_RELATIVE_END = 3,       /**< Seek relatively to the current end of the buffer queue */
} pa_seek_mode_t;

PA_C_DECL_END

#endif
