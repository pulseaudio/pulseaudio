#ifndef fooglibmainloophfoo
#define fooglibmainloophfoo

#include <glib.h>

#include "mainloop-api.h"

struct pa_glib_mainloop;

struct pa_glib_mainloop *pa_glib_mainloop_new(GMainLoop *ml);
void pa_glib_mainloop_free(struct pa_glib_mainloop* g);
struct pa_mainloop_api* pa_glib_mainloop_get_api(struct pa_glib_mainloop *g);

#endif
