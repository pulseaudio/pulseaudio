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

struct x11_source {
    void *io_source;
    struct x11_source *next;
};

struct userdata {
    struct pa_core *core;
    Display *display;
    struct x11_source *x11_sources;
    int xkb_event_base;

    int sink_index;
    char *scache_item;
};

static const char* const valid_modargs[] = {
    "sink",
    "sample",
    "display",
    NULL
};

static struct pa_sink* get_output_sink(struct userdata *u) {
    struct pa_sink *s;
    assert(u);

    if (!(s = pa_idxset_get_by_index(u->core->sinks, u->sink_index)))
        s = pa_sink_get_default(u->core);

    u->sink_index = s ? s->index : PA_IDXSET_INVALID;
    return s;
}

static int ring_bell(struct userdata *u, int percent) {
    struct pa_sink *s;
    assert(u);

    if (!(s = get_output_sink(u))) {
        fprintf(stderr, __FILE__": Invalid sink\n");
        return -1;
    }

    if (pa_scache_play_item(u->core, u->scache_item, s, percent*2) < 0) {
        fprintf(stderr, __FILE__": Failed to play sample\n");
        return -1;
    }

    return 0;
}

static void io_callback(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata) {
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

    s = malloc(sizeof(struct x11_source));
    assert(s);
    s->io_source = u->core->mainloop->source_io(u->core->mainloop, fd, PA_MAINLOOP_API_IO_EVENT_INPUT, io_callback, u);
    assert(s->io_source);
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
    
    m->userdata = u = malloc(sizeof(struct userdata));
    assert(u);
    u->core = c;
    u->display = NULL;
    u->x11_sources = NULL;
    u->scache_item = strdup(pa_modargs_get_value(ma, "sample", "x11-bell"));
    assert(u->scache_item);
        
    if (pa_modargs_get_sink_index(ma, c, &u->sink_index) < 0) {
        fprintf(stderr, __FILE__": Invalid sink specified\n");
        goto fail;
    }

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
        c->mainloop->cancel_io(c->mainloop, s->io_source);
        free(s);
    }

    free(u->scache_item);
    
    if (u->display)
        XCloseDisplay(u->display);
    free(u);
}
