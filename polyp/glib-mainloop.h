#ifndef fooglibmainloophfoo
#define fooglibmainloophfoo

#include <glib.h>

#include "mainloop-api.h"
#include "cdecl.h"

/** \file
 * GLIB main loop support */

PA_C_DECL_BEGIN

/** \struct pa_glib_mainloop
 * An opaque GLIB main loop object */
struct pa_glib_mainloop;

/** Create a new GLIB main loop object for the specified GLIB main loop context. If c is NULL the default context is used. */
struct pa_glib_mainloop *pa_glib_mainloop_new(GMainContext *c);

/** Free the GLIB main loop object */
void pa_glib_mainloop_free(struct pa_glib_mainloop* g);

/** Return the abstract main loop API vtable for the GLIB main loop object */
struct pa_mainloop_api* pa_glib_mainloop_get_api(struct pa_glib_mainloop *g);

PA_C_DECL_END

#endif
