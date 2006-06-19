#ifndef foobrowserhfoo
#define foobrowserhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
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

PA_C_DECL_BEGIN

typedef struct pa_browser pa_browser;

typedef enum pa_browse_opcode {
    PA_BROWSE_NEW_SERVER = 0,
    PA_BROWSE_NEW_SINK,
    PA_BROWSE_NEW_SOURCE,
    PA_BROWSE_REMOVE_SERVER,
    PA_BROWSE_REMOVE_SINK,
    PA_BROWSE_REMOVE_SOURCE
} pa_browse_opcode_t;

pa_browser *pa_browser_new(pa_mainloop_api *mainloop);
pa_browser *pa_browser_ref(pa_browser *z);
void pa_browser_unref(pa_browser *z);

typedef struct pa_browse_info {
    /* Unique service name */
    const char *name;  /* always available */

    /* Server info */
    const char *server; /* always available */
    const char *server_version, *user_name, *fqdn; /* optional */
    const uint32_t *cookie;  /* optional */

    /* Device info */
    const char *device; /* always available when this information is of a sink/source */
    const char *description;  /* optional */
    const pa_sample_spec *sample_spec;  /* optional */
} pa_browse_info;

typedef void (*pa_browse_cb_t)(pa_browser *z, pa_browse_opcode_t c, const pa_browse_info *i, void *userdata);

void pa_browser_set_callback(pa_browser *z, pa_browse_cb_t cb, void *userdata);

PA_C_DECL_END

#endif
