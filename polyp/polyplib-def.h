#ifndef foopolyplibdefhfoo
#define foopolyplibdefhfoo

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

#include <inttypes.h>
#include "cdecl.h"
#include "sample.h"

/** \file
 * Global definitions */

PA_C_DECL_BEGIN

/** The state of a connection context */
enum pa_context_state {
    PA_CONTEXT_UNCONNECTED,    /**< The context hasn't been connected yet */
    PA_CONTEXT_CONNECTING,     /**< A connection is being established */
    PA_CONTEXT_AUTHORIZING,    /**< The client is authorizing itself to the daemon */
    PA_CONTEXT_SETTING_NAME,   /**< The client is passing its application name to the daemon */
    PA_CONTEXT_READY,          /**< The connection is established, the context is ready to execute operations */
    PA_CONTEXT_FAILED,         /**< The connection failed or was disconnected */
    PA_CONTEXT_TERMINATED      /**< The connection was terminated cleanly */
};

/** The state of a stream */
enum pa_stream_state {
    PA_STREAM_DISCONNECTED, /**< The stream is not yet connected to any sink or source */
    PA_STREAM_CREATING,     /**< The stream is being created */
    PA_STREAM_READY,        /**< The stream is established, you may pass audio data to it now */
    PA_STREAM_FAILED,       /**< An error occured that made the stream invalid */
    PA_STREAM_TERMINATED    /**< The stream has been terminated cleanly */
};

/** The state of an operation */
enum pa_operation_state {
    PA_OPERATION_RUNNING,      /**< The operation is still running */
    PA_OPERATION_DONE,         /**< The operation has been completed */
    PA_OPERATION_CANCELED      /**< The operation has been canceled */
};

/** An invalid index */
#define PA_INVALID_INDEX ((uint32_t) -1)

/** The direction of a pa_stream object */ 
enum pa_stream_direction {
    PA_STREAM_NODIRECTION,   /**< Invalid direction */
    PA_STREAM_PLAYBACK,      /**< Playback stream */
    PA_STREAM_RECORD,        /**< Record stream */
    PA_STREAM_UPLOAD         /**< Sample upload stream */
};

/** Playback and record buffer metrics */
struct pa_buffer_attr{
    uint32_t maxlength;      /**< Maximum length of the buffer */
    uint32_t tlength;        /**< Playback only: target length of the buffer. The server tries to assure that at least tlength bytes are always available in the buffer */
    uint32_t prebuf;         /**< Playback only: pre-buffering. The server does not start with playback before at least prebug bytes are available in the buffer */
    uint32_t minreq;         /**< Playback only: minimum request. The server does not request less than minreq bytes from the client, instead waints until the buffer is free enough to request more bytes at once */
    uint32_t fragsize;       /**< Recording only: fragment size. The server sends data in blocks of fragsize bytes size. Large values deminish interactivity with other operations on the connection context but decrease control overhead. */
};

/** Error values as used by pa_context_errno(). Use pa_strerror() to convert these values to human readable strings */
enum {
    PA_ERROR_OK,                     /**< No error */
    PA_ERROR_ACCESS,                 /**< Access failure */
    PA_ERROR_COMMAND,                /**< Unknown command */
    PA_ERROR_INVALID,                /**< Invalid argument */
    PA_ERROR_EXIST,                  /**< Entity exists */
    PA_ERROR_NOENTITY,               /**< No such entity */
    PA_ERROR_CONNECTIONREFUSED,      /**< Connection refused */
    PA_ERROR_PROTOCOL,               /**< Protocol error */ 
    PA_ERROR_TIMEOUT,                /**< Timeout */
    PA_ERROR_AUTHKEY,                /**< No authorization key */
    PA_ERROR_INTERNAL,               /**< Internal error */
    PA_ERROR_CONNECTIONTERMINATED,   /**< Connection terminated */
    PA_ERROR_KILLED,                 /**< Entity killed */
    PA_ERROR_INVALIDSERVER,          /**< Invalid server */
    PA_ERROR_MAX                     /**< Not really an error but the first invalid error code */
};

/** Subscription event mask, as used by pa_context_subscribe() */
enum pa_subscription_mask {
    PA_SUBSCRIPTION_MASK_NULL = 0,               /**< No events */
    PA_SUBSCRIPTION_MASK_SINK = 1,               /**< Sink events */
    PA_SUBSCRIPTION_MASK_SOURCE = 2,             /**< Source events */
    PA_SUBSCRIPTION_MASK_SINK_INPUT = 4,         /**< Sink input events */
    PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT = 8,      /**< Source output events */
    PA_SUBSCRIPTION_MASK_MODULE = 16,            /**< Module events */
    PA_SUBSCRIPTION_MASK_CLIENT = 32,            /**< Client events */
    PA_SUBSCRIPTION_MASK_SAMPLE_CACHE = 64       /**< Sample cache events */
};

/** Subscription event types, as used by pa_context_subscribe() */
enum pa_subscription_event_type {
    PA_SUBSCRIPTION_EVENT_SINK = 0,           /**< Event type: Sink */
    PA_SUBSCRIPTION_EVENT_SOURCE = 1,         /**< Event type: Source */
    PA_SUBSCRIPTION_EVENT_SINK_INPUT = 2,     /**< Event type: Sink input */
    PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT = 3,  /**< Event type: Source output */
    PA_SUBSCRIPTION_EVENT_MODULE = 4,         /**< Event type: Module */
    PA_SUBSCRIPTION_EVENT_CLIENT = 5,         /**< Event type: Client */
    PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE = 6,   /**< Event type: Sample cache item */
    PA_SUBSCRIPTION_EVENT_FACILITY_MASK = 7,  /**< A mask to extract the event type from an event value */

    PA_SUBSCRIPTION_EVENT_NEW = 0,            /**< A new object was created */
    PA_SUBSCRIPTION_EVENT_CHANGE = 16,        /**< A property of the object was modified */
    PA_SUBSCRIPTION_EVENT_REMOVE = 32,        /**< An object was removed */
    PA_SUBSCRIPTION_EVENT_TYPE_MASK = 16+32   /**< A mask to extract the event operation from an event value */
};

/** Return one if an event type t matches an event mask bitfield */
#define pa_subscription_match_flags(m, t) (!!((m) & (1 << ((t) & PA_SUBSCRIPTION_EVENT_FACILITY_MASK))))

/** A structure for latency info. See pa_stream_get_latency().  */
struct pa_latency_info {
    pa_usec_t buffer_usec;    /**< Time in usecs the current buffer takes to play */
    pa_usec_t sink_usec;      /**< Time in usecs a sample takes to be played on the sink. The total latency is buffer_usec+sink_usec. */
    int playing;              /**< Non-zero when the stream is currently playing */
    int queue_length;         /**< Queue size in bytes. */  
};

PA_C_DECL_END

#endif
