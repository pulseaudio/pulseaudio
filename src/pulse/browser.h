#ifndef foobrowserhfoo
#define foobrowserhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/mainloop-api.h>
#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/cdecl.h>
#include <pulse/version.h>

/** \file
 * An abstract interface for Zeroconf browsing of PulseAudio servers */

PA_C_DECL_BEGIN

/** An opaque Zeroconf service browser object */
typedef struct pa_browser pa_browser;

/** Opcodes for pa_browser_cb_t callbacks */
typedef enum pa_browse_opcode {
    PA_BROWSE_NEW_SERVER = 0, /**< New server found */
    PA_BROWSE_NEW_SINK,       /**< New sink found */
    PA_BROWSE_NEW_SOURCE,     /**< New source found */
    PA_BROWSE_REMOVE_SERVER,  /**< Server disappeared */
    PA_BROWSE_REMOVE_SINK,    /**< Sink disappeared */
    PA_BROWSE_REMOVE_SOURCE   /**< Source disappeared */
} pa_browse_opcode_t;

typedef enum pa_browse_flags {
    PA_BROWSE_FOR_SERVERS = 1, /**< Browse for servers */
    PA_BROWSE_FOR_SINKS = 2, /**< Browse for sinks */
    PA_BROWSE_FOR_SOURCES = 4 /** Browse for sources */
} pa_browse_flags_t;

/** Create a new browser object on the specified main loop */
pa_browser *pa_browser_new(pa_mainloop_api *mainloop);

/** Same pa_browser_new, but pass additional flags parameter. */
pa_browser *pa_browser_new_full(pa_mainloop_api *mainloop, pa_browse_flags_t flags, const char **error_string);

/** Increase reference counter of the specified browser object */
pa_browser *pa_browser_ref(pa_browser *z);

/** Decrease reference counter of the specified browser object */
void pa_browser_unref(pa_browser *z);

/** Information about a sink/source/server found with Zeroconf */
typedef struct pa_browse_info {
    const char *name;  /**< Unique service name; always available */

    const char *server; /**< Server name; always available */
    const char *server_version; /**< Server version string; optional */
    const char *user_name; /**< User name of the server process; optional */
    const char *fqdn; /* Server version; optional */
    const uint32_t *cookie;  /* Server cookie; optional */

    const char *device; /* Device name; always available when this information is of a sink/source */
    const char *description;  /* Device description; optional */
    const pa_sample_spec *sample_spec;  /* Sample specification of the device; optional */
} pa_browse_info;

/** Callback prototype */
typedef void (*pa_browse_cb_t)(pa_browser *z, pa_browse_opcode_t c, const pa_browse_info *i, void *userdata);

/** Set the callback pointer for the browser object */
void pa_browser_set_callback(pa_browser *z, pa_browse_cb_t cb, void *userdata);

/** Callback prototype for errors */
typedef void (*pa_browser_error_cb_t)(pa_browser *z, const char *error_string, void *userdata);

/** Set a callback function that is called whenever the browser object
 * becomes invalid due to an error. After this function has been
 * called the browser object has become invalid and should be
 * freed. */
void pa_browser_set_error_callback(pa_browser *z, pa_browser_error_cb_t, void *userdata);

PA_C_DECL_END

#endif
