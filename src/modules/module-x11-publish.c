/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/x11wrap.h>
#include <pulsecore/core-util.h>
#include <pulsecore/native-common.h>
#include <pulsecore/authkey-prop.h>
#include <pulsecore/authkey.h>
#include <pulsecore/x11prop.h>
#include <pulsecore/strlist.h>
#include <pulsecore/props.h>

#include "module-x11-publish-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("X11 Credential Publisher")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("display=<X11 display>")

static const char* const valid_modargs[] = {
    "display",
    "sink",
    "source",
    "cookie",
    NULL
};

struct userdata {
    pa_core *core;
    pa_x11_wrapper *x11_wrapper;
    char *id;
    uint8_t auth_cookie[PA_NATIVE_COOKIE_LENGTH];
    int auth_cookie_in_property;
};

static int load_key(struct userdata *u, const char*fn) {
    assert(u);

    u->auth_cookie_in_property = 0;
    
    if (!fn && pa_authkey_prop_get(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME, u->auth_cookie, sizeof(u->auth_cookie)) >= 0) {
        pa_log_debug(__FILE__": using already loaded auth cookie.");
        pa_authkey_prop_ref(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME);
        u->auth_cookie_in_property = 1;
        return 0;
    }
    
    if (!fn)
        fn = PA_NATIVE_COOKIE_FILE;

    if (pa_authkey_load_auto(fn, u->auth_cookie, sizeof(u->auth_cookie)) < 0)
        return -1;

    pa_log_debug(__FILE__": loading cookie from disk.");
    
    if (pa_authkey_prop_put(u->core, PA_NATIVE_COOKIE_PROPERTY_NAME, u->auth_cookie, sizeof(u->auth_cookie)) >= 0)
        u->auth_cookie_in_property = 1;

    return 0;
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    char hn[256], un[128];
    char hx[PA_NATIVE_COOKIE_LENGTH*2+1];
    const char *t;
    char *s;
    pa_strlist *l;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;
    u->id = NULL;
    u->auth_cookie_in_property = 0;

    if (load_key(u, pa_modargs_get_value(ma, "cookie", NULL)) < 0)
        goto fail;

    if (!(u->x11_wrapper = pa_x11_wrapper_get(c, pa_modargs_get_value(ma, "display", NULL)))) 
        goto fail;

    if (!(l = pa_property_get(c, PA_NATIVE_SERVER_PROPERTY_NAME)))
        goto fail;

    s = pa_strlist_tostring(l);
    pa_x11_set_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_SERVER", s);
    pa_xfree(s);
    
    if (!pa_get_fqdn(hn, sizeof(hn)) || !pa_get_user_name(un, sizeof(un)))
        goto fail;
    
    u->id = pa_sprintf_malloc("%s@%s/%u", un, hn, (unsigned) getpid());
    pa_x11_set_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_ID", u->id);

    if ((t = pa_modargs_get_value(ma, "source", NULL)))
        pa_x11_set_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_SOURCE", t);

    if ((t = pa_modargs_get_value(ma, "sink", NULL)))
        pa_x11_set_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_SINK", t);

    pa_x11_set_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_COOKIE", pa_hexstr(u->auth_cookie, sizeof(u->auth_cookie), hx, sizeof(hx)));
    
    pa_modargs_free(ma);
    return 0;
    
fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(c, m);
    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata*u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    if (u->x11_wrapper) {
        char t[256];

        /* Yes, here is a race condition */
        if (!pa_x11_get_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_ID", t, sizeof(t)) || strcmp(t, u->id))
            pa_log_warn(__FILE__": PulseAudio information vanished from X11!");
        else {
            pa_x11_del_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_ID");
            pa_x11_del_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_SERVER");
            pa_x11_del_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_SINK");
            pa_x11_del_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_SOURCE");
            pa_x11_del_prop(pa_x11_wrapper_get_display(u->x11_wrapper), "POLYP_COOKIE");
            XSync(pa_x11_wrapper_get_display(u->x11_wrapper), False);
        }
    }
    
    if (u->x11_wrapper)
        pa_x11_wrapper_unref(u->x11_wrapper);

    if (u->auth_cookie_in_property)
        pa_authkey_prop_unref(c, PA_NATIVE_COOKIE_PROPERTY_NAME);

    pa_xfree(u->id);
    pa_xfree(u);
}

