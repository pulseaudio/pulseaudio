/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <locale.h>
#include <dlfcn.h>

#include <pulse/proplist.h>
#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/core-util.h>

#include "proplist-util.h"

void pa_init_proplist(pa_proplist *p) {
#if !HAVE_DECL_ENVIRON
    extern char **environ;
#endif
    char **e;
    const char *pp;

    pa_assert(p);

    if (environ) {

        /* Some applications seem to reset environ to NULL for various
         * reasons, hence we need to check for this explicitly. See
         * rhbz #473080 */

        for (e = environ; *e; e++) {

            if (pa_startswith(*e, "PULSE_PROP_")) {
                size_t kl = strcspn(*e+11, "=");
                char *k;

                if ((*e)[11+kl] != '=')
                    continue;

                if (!pa_utf8_valid(*e+11+kl+1))
                    continue;

                k = pa_xstrndup(*e+11, kl);

                if (pa_proplist_contains(p, k)) {
                    pa_xfree(k);
                    continue;
                }

                pa_proplist_sets(p, k, *e+11+kl+1);
                pa_xfree(k);
            }
        }
    }

    if ((pp = getenv("PULSE_PROP"))) {
        pa_proplist *t;

        if ((t = pa_proplist_from_string(pp))) {
            pa_proplist_update(p, PA_UPDATE_MERGE, t);
            pa_proplist_free(t);
        }
    }

    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_PROCESS_ID)) {
        char t[32];
        pa_snprintf(t, sizeof(t), "%lu", (unsigned long) getpid());
        pa_proplist_sets(p, PA_PROP_APPLICATION_PROCESS_ID, t);
    }

    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_PROCESS_USER)) {
        char t[64];
        if (pa_get_user_name(t, sizeof(t))) {
            char *c = pa_utf8_filter(t);
            pa_proplist_sets(p, PA_PROP_APPLICATION_PROCESS_USER, c);
            pa_xfree(c);
        }
    }

    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_PROCESS_HOST)) {
        char t[64];
        if (pa_get_host_name(t, sizeof(t))) {
            char *c = pa_utf8_filter(t);
            pa_proplist_sets(p, PA_PROP_APPLICATION_PROCESS_HOST, c);
            pa_xfree(c);
        }
    }

    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_PROCESS_BINARY)) {
        char t[PATH_MAX];
        if (pa_get_binary_name(t, sizeof(t))) {
            char *c = pa_utf8_filter(t);
            pa_proplist_sets(p, PA_PROP_APPLICATION_PROCESS_BINARY, c);
            pa_xfree(c);
        }
    }

#ifdef RTLD_NOLOAD
    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_NAME)) {
        void *dl;

        if ((dl = dlopen("libglib-2.0", RTLD_NOLOAD))) {
            const char *(*_g_get_application_name)(void);

            if ((*(void**) &_g_get_application_name = dlsym(dl, "g_get_application_name")))
                pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, _g_get_application_name());

            dlclose(dl);
        }
    }
#endif

    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_NAME)) {
        const char *t;

        if ((t = pa_proplist_gets(p, PA_PROP_APPLICATION_PROCESS_BINARY)))
            pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, t);
    }

    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_LANGUAGE)) {
        const char *l;

        if ((l = setlocale(LC_MESSAGES, NULL)))
            pa_proplist_sets(p, PA_PROP_APPLICATION_LANGUAGE, l);
    }

    if (!pa_proplist_contains(p, PA_PROP_WINDOW_X11_DISPLAY)) {
        const char *t;

        if ((t = getenv("DISPLAY"))) {
            char *c = pa_utf8_filter(t);
            pa_proplist_sets(p, PA_PROP_WINDOW_X11_DISPLAY, c);
            pa_xfree(c);
        }
    }

    if (!pa_proplist_contains(p, PA_PROP_APPLICATION_PROCESS_MACHINE_ID)) {
        char *m;

        if ((m = pa_machine_id())) {
            pa_proplist_sets(p, PA_PROP_APPLICATION_PROCESS_MACHINE_ID, m);
            pa_xfree(m);
        }
    }
}
