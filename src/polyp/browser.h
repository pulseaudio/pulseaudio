#ifndef foobrowserhfoo
#define foobrowserhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <polyp/mainloop-api.h>
#include <polyp/sample.h>
#include <polyp/cdecl.h>
#include <polyp/typeid.h>

PA_C_DECL_BEGIN

pa_browser;

pa_browse_opcode {
    PA_BROWSE_NEW_SERVER,
    PA_BROWSE_NEW_SINK,
    PA_BROWSE_NEW_SOURCE,
    PA_BROWSE_REMOVE
};

pa_browser *pa_browser_new(pa_mainloop_api *mainloop);
pa_browser *pa_browser_ref(pa_browser *z);
void pa_browser_unref(pa_browser *z);

pa_browse_info {
    /* Unique service name */
    const char *name;  /* always available */

    /* Server info */
    const char *server; /* always available */
    const char *server_version, *user_name, *fqdn; /* optional */
    const uint32_t *cookie;  /* optional */

    /* Device info */
    const char *device; /* always available when this information is of a sink/source */
    const char *description;  /* optional */
    const pa_typeid_t *typeid;  /* optional */
    const pa_sample_spec *sample_spec;  /* optional */
};

void pa_browser_set_callback(pa_browser *z, void (*cb)(pa_browser *z, pa_browse_opcode c, const pa_browse_info *i, void *userdata), void *userdata);

PA_C_DECL_END

#endif
