#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#include "module.h"
#include "sink.h"
#include "scache.h"
#include "modargs.h"
#include "xmalloc.h"
#include "namereg.h"

struct x11_source {
    struct pa_io_event *io_event;
    struct x11_source *next;
};

struct userdata {
    struct pa_core *core;
    Display *display;
    struct x11_source *x11_sources;
    int xkb_event_base;

    char *sink_name;
    char *scache_item;
};

static const char* const valid_modargs[] = {
    "sink",
    "sample",
    "display",
    NULL
};

static int ring_bell(struct userdata *u, int percent) {
    struct pa_sink *s;
    assert(u);

    if (!(s = pa_namereg_get(u->core, u->sink_name, PA_NAMEREG_SINK, 1))) {
        fprintf(stderr, __FILE__": Invalid sink\n");
        return -1;
    }

    if (pa_scache_play_item(u->core, u->scache_item, s, percent*2) < 0) {
        fprintf(stderr, __FILE__": Failed to play sample\n");
        return -1;
    }

    return 0;
}

static void io_callback(struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    struct userdata *u = userdata;
    assert(u);
    
    while (XPending(u->display)) {
        XEvent e;
        XkbBellNotifyEvent *bne;
        XNextEvent(u->display, &e);

        if (((XkbEvent*) &e)->any.xkb_type != XkbBellNotify)
            continue;

        bne = ((XkbBellNotifyEvent*) &e);
            
        if (ring_bell(u, bne->percent) < 0) {
            fprintf(stderr, __FILE__": Ringing bell failed, reverting to X11 device bell.\n");
            XkbForceDeviceBell(u->display, bne->device, bne->bell_class, bne->bell_id, bne->percent);
        }
    }
}

static void new_io_source(struct userdata *u, int fd) {
    struct x11_source *s;

    s = pa_xmalloc(sizeof(struct x11_source));
    s->io_event = u->core->mainloop->io_new(u->core->mainloop, fd, PA_IO_EVENT_INPUT, io_callback, u);
    assert(s->io_event);
    s->next = u->x11_sources;
    u->x11_sources = s;
}

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u = NULL;
    struct pa_modargs *ma = NULL;
    int major, minor;
    unsigned int auto_ctrls, auto_values;
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        fprintf(stderr, __FILE__": failed to parse module arguments\n");
        goto fail;
    }
    
    m->userdata = u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;
    u->display = NULL;
    u->x11_sources = NULL;
    u->scache_item = pa_xstrdup(pa_modargs_get_value(ma, "sample", "x11-bell"));
    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));

    if (!(u->display = XOpenDisplay(pa_modargs_get_value(ma, "display", NULL)))) {
        fprintf(stderr, __FILE__": XOpenDisplay() failed\n");
        goto fail;
    }

    new_io_source(u, ConnectionNumber(u->display));

    major = XkbMajorVersion;
    minor = XkbMinorVersion;
    
    if (!XkbLibraryVersion(&major, &minor)) {
        fprintf(stderr, __FILE__": XkbLibraryVersion() failed\n");
        goto fail;
    }

    major = XkbMajorVersion;
    minor = XkbMinorVersion;

    if (!XkbQueryExtension(u->display, NULL, &u->xkb_event_base, NULL, &major, &minor)) {
        fprintf(stderr, __FILE__": XkbQueryExtension() failed\n");
        goto fail;
    }

    XkbSelectEvents(u->display, XkbUseCoreKbd, XkbBellNotifyMask, XkbBellNotifyMask);
    auto_ctrls = auto_values = XkbAudibleBellMask;
    XkbSetAutoResetControls(u->display, XkbAudibleBellMask, &auto_ctrls, &auto_values);
    XkbChangeEnabledControls(u->display, XkbUseCoreKbd, XkbAudibleBellMask, 0);

    pa_modargs_free(ma);
    
    return 0;
    
fail:
    if (ma)
        pa_modargs_free(ma);
    if (m->userdata)
        pa_module_done(c, m);
    return -1;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u = m->userdata;
    assert(c && m && u);

    while (u->x11_sources) {
        struct x11_source *s = u->x11_sources;
        u->x11_sources = u->x11_sources->next;
        c->mainloop->io_free(s->io_event);
        pa_xfree(s);
    }

    pa_xfree(u->scache_item);
    
    if (u->display)
        XCloseDisplay(u->display);
    pa_xfree(u);
}
