#ifndef foopolyplibsimplehfoo
#define foopolyplibsimplehfoo

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

/** \file
 * A simple but limited synchronous playback and recording
 * API. This is synchronouse, simplified wrapper around the standard
 * asynchronous API. */

/** \example pacat-simple.c
 * A simple playback tool using the simple API */

/** \example parec-simple.c
 * A simple recording tool using the simple API */

PA_C_DECL_BEGIN

/** \struct pa_simple
 * An opaque simple connection object */
struct pa_simple;

/** Create a new connection to the server */
struct pa_simple* pa_simple_new(
    const char *server,                 /**< Server name, or NULL for default */
    const char *name,                   /**< A descriptive name for this client (application name, ...) */
    enum pa_stream_direction dir,       /**< Open this stream for recording or playback? */
    const char *dev,                    /**< Sink (resp. source) name, or NULL for default */
    const char *stream_name,            /**< A descriptive name for this client (application name, song title, ...) */
    const struct pa_sample_spec *ss,    /**< The sample type to use */
    const struct pa_buffer_attr *attr,  /**< Buffering attributes, or NULL for default */
    int *error                        /**< A pointer where the error code is stored when the routine returns NULL. It is OK to pass NULL here. */
    );

/** Close and free the connection to the server. The connection objects becomes invalid when this is called. */
void pa_simple_free(struct pa_simple *s);

/** Write some data to the server */
int pa_simple_write(struct pa_simple *s, const void*data, size_t length, int *error);

/** Wait until all data already written is played by the daemon */
int pa_simple_drain(struct pa_simple *s, int *error);

/** Read some data from the server */
int pa_simple_read(struct pa_simple *s, void*data, size_t length, int *error);

/** Return the playback latency. \since 0.5 */
pa_usec_t pa_simple_get_playback_latency(struct pa_simple *s, int *perror);

/** Flush the playback buffer. \since 0.5 */
int pa_simple_flush(struct pa_simple *s, int *perror);

PA_C_DECL_END

#endif
