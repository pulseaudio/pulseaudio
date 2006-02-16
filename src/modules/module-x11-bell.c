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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#include <polypcore/iochannel.h>
#include <polypcore/sink.h>
#include <polypcore/core-scache.h>
#include <polypcore/modargs.h>
#include <polypcore/xmalloc.h>
#include <polypcore/namereg.h>
#include <polypcore/log.h>
#include <polypcore/x11wrap.h>

#include "module-x11-bell-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("X11 Bell interceptor")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("sink=<sink to connect to> sample=<sample name> display=<X11 display>")

struct userdata {
    pa_core *core;
    int xkb_event_base;
    char *sink_name;
    char *scache_item;
    Display *display;

    pa_x11_wrapper *x11_wrapper;
    pa_x11_client *x11_client;
};

static const char* const valid_modargs[] = {
    "sink",
    "sample",
    "display",
    NULL
};

static int ring_bell(struct userdata *u, int percent) {
    pa_sink *s;
    pa_cvolume cv;
    assert(u);

    if (!(s = pa_namereg_get(u->core, u->sink_name, PA_NAMEREG_SINK, 1))) {
        pa_log(__FILE__": Invalid sink: %s\n", u->sink_name);
        return -1;
    }

    pa_scache_play_item(u->core, u->scache_item, s, pa_cvolume_set(&cv, PA_CHANNELS_MAX, percent*PA_VOLUME_NORM/100));
    return 0;
}

static int x11_event_callback(pa_x11_wrapper *w, XEvent *e, void *userdata) {
    XkbBellNotifyEvent *bne;
    struct userdata *u = userdata;
    assert(w && e && u && u->x11_wrapper == w);

    if (((XkbEvent*) e)->any.xkb_type != XkbBellNotify)
        return 0;

    bne = (XkbBellNotifyEvent*) e;

    if (ring_bell(u, bne->percent) < 0) {
        pa_log_info(__FILE__": Ringing bell failed, reverting to X11 device bell.\n");
        XkbForceDeviceBell(pa_x11_wrapper_get_display(w), bne->device, bne->bell_class, bne->bell_id, bne->percent);
    }

    return 1;
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    int major, minor;
    unsigned int auto_ctrls, auto_values;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments\n");
        goto fail;
    }
    
    m->userdata = u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;
    u->scache_item = pa_xstrdup(pa_modargs_get_value(ma, "sample", "x11-bell"));
    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));
    u->x11_client = NULL;

    if (!(u->x11_wrapper = pa_x11_wrapper_get(c, pa_modargs_get_value(ma, "display", NULL)))) 
        goto fail;

    u->display = pa_x11_wrapper_get_display(u->x11_wrapper);
    
    major = XkbMajorVersion;
    minor = XkbMinorVersion;
    
    if (!XkbLibraryVersion(&major, &minor)) {
        pa_log(__FILE__": XkbLibraryVersion() failed\n");
        goto fail;
    }

    major = XkbMajorVersion;
    minor = XkbMinorVersion;


    if (!XkbQueryExtension(u->display, NULL, &u->xkb_event_base, NULL, &major, &minor)) {
        pa_log(__FILE__": XkbQueryExtension() failed\n");
        goto fail;
    }

    XkbSelectEvents(u->display, XkbUseCoreKbd, XkbBellNotifyMask, XkbBellNotifyMask);
    auto_ctrls = auto_values = XkbAudibleBellMask;
    XkbSetAutoResetControls(u->display, XkbAudibleBellMask, &auto_ctrls, &auto_values);
    XkbChangeEnabledControls(u->display, XkbUseCoreKbd, XkbAudibleBellMask, 0);

    u->x11_client = pa_x11_client_new(u->x11_wrapper, x11_event_callback, u);
    
    pa_modargs_free(ma);
    
    return 0;
    
fail:
    if (ma)
        pa_modargs_free(ma);
    if (m->userdata)
        pa__done(c, m);
    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u = m->userdata;
    assert(c && m && u);

    pa_xfree(u->scache_item);
    pa_xfree(u->sink_name);

    if (u->x11_client)
        pa_x11_client_free(u->x11_client);

    if (u->x11_wrapper)
        pa_x11_wrapper_unref(u->x11_wrapper);

    pa_xfree(u);
}
