#ifndef fooglibmainloophfoo
#define fooglibmainloophfoo

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

#include <glib.h>

#include "mainloop-api.h"
#include "cdecl.h"

/** \file
 * GLIB main loop support */

PA_C_DECL_BEGIN

/** \pa_glib_mainloop
 * An opaque GLIB main loop object */
typedef struct pa_glib_mainloop pa_glib_mainloop;

/** Create a new GLIB main loop object for the specified GLIB main loop context. If c is NULL the default context is used. */
#if GLIB_MAJOR_VERSION >= 2
pa_glib_mainloop *pa_glib_mainloop_new(GMainContext *c);
#else
pa_glib_mainloop *pa_glib_mainloop_new(void);
#endif


/** Free the GLIB main loop object */
void pa_glib_mainloop_free(pa_glib_mainloop* g);

/** Return the abstract main loop API vtable for the GLIB main loop object */
pa_mainloop_api* pa_glib_mainloop_get_api(pa_glib_mainloop *g);

PA_C_DECL_END

#endif
