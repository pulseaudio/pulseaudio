#include <assert.h>

#include "glib-mainloop.h"
#include "idxset.h"
#include "xmalloc.h"

struct pa_io_event {
    GSource source;
    int dead;
    struct pa_glib_mainloop *mainloop;
    int fd;
    GPollFD pollfd;
    void (*callback) (struct pa_mainloop_api*m, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata);
    void *userdata;
    void (*destroy_callback) (struct pa_mainloop_api *m, struct pa_io_event*e, void *userdata);
    struct pa_io_event *next, *prev;
};

struct pa_time_event {
    struct pa_glib_mainloop *mainloop;
    int dead;
    GSource *source;
    struct timeval timeval;
    void (*callback) (struct pa_mainloop_api*m, struct pa_time_event *e, const struct timeval *tv, void *userdata);
    void *userdata;
    void (*destroy_callback) (struct pa_mainloop_api *m, struct pa_time_event*e, void *userdata);
    struct pa_time_event *next, *prev;
};

struct pa_defer_event {
    struct pa_glib_mainloop *mainloop;
    int dead;
    GSource *source;
    void (*callback) (struct pa_mainloop_api*m, struct pa_defer_event *e, void *userdata);
    void *userdata;
    void (*destroy_callback) (struct pa_mainloop_api *m, struct pa_defer_event*e, void *userdata);
    struct pa_defer_event *next, *prev;
};

struct pa_glib_mainloop {
    GMainLoop *glib_mainloop;
    struct pa_mainloop_api api;
    GSource *cleanup_source;
    struct pa_io_event *io_events, *dead_io_events;
    struct pa_time_event *time_events, *dead_time_events;
    struct pa_defer_event *defer_events, *dead_defer_events;
};

static void schedule_free_dead_events(struct pa_glib_mainloop *g);

static gboolean glib_source_prepare(GSource *source, gint *timeout) {
    return FALSE;
}

static gboolean glib_source_check(GSource *source) {
    struct pa_io_event *e = (struct pa_io_event*) source;
    assert(e);
    return !!e->pollfd.revents;
}

static gboolean glib_source_dispatch(GSource *source, GSourceFunc callback, gpointer user_data) {
    struct pa_io_event *e = (struct pa_io_event*) source;
    assert(e);

    if (e->pollfd.revents) {
        int f =
            (e->pollfd.revents ? G_IO_IN : PA_IO_EVENT_INPUT) |
            (e->pollfd.revents ? G_IO_OUT : PA_IO_EVENT_OUTPUT) |
            (e->pollfd.revents ? G_IO_HUP : PA_IO_EVENT_HANGUP) |
            (e->pollfd.revents ? G_IO_ERR : PA_IO_EVENT_ERROR);
        e->pollfd.revents = 0;

        assert(e->callback);
        e->callback(&e->mainloop->api, e, e->fd, f, e->userdata);
    }

    return TRUE;
}

static void glib_io_enable(struct pa_io_event*e, enum pa_io_event_flags f);

static struct pa_io_event* glib_io_new(struct pa_mainloop_api*m, int fd, enum pa_io_event_flags f, void (*callback) (struct pa_mainloop_api*m, struct pa_io_event*e, int fd, enum pa_io_event_flags f, void *userdata), void *userdata) {
    struct pa_io_event *e;
    struct pa_glib_mainloop *g;

    GSourceFuncs io_source_funcs = {
        prepare: glib_source_prepare,
        check: glib_source_check,
        dispatch: glib_source_dispatch,
        finalize: NULL,
        closure_callback: NULL,	   
        closure_marshal : NULL,
    };

    assert(m && m->userdata && fd >= 0 && callback);
    g = m->userdata;

    e = (struct pa_io_event*) g_source_new(&io_source_funcs, sizeof(struct pa_io_event));
    assert(e);
    e->mainloop = m->userdata;
    e->dead = 0;
    e->fd = fd;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;

    e->pollfd.fd = fd;
    e->pollfd.events = e->pollfd.revents = 0;

    g_source_attach(&e->source, g_main_loop_get_context(g->glib_mainloop));

    glib_io_enable(e, f);

    e->next = g->io_events;
    if (e->next) e->next->prev = e;
    g->io_events = e;
    e->prev = NULL;
    
    return e;
}

static void glib_io_enable(struct pa_io_event*e, enum pa_io_event_flags f) {
    int o;
    assert(e && !e->dead);

    o = e->pollfd.events;
    e->pollfd.events = (f & PA_IO_EVENT_INPUT ? G_IO_IN : 0) | (f & PA_IO_EVENT_OUTPUT ? G_IO_OUT : 0) | G_IO_HUP | G_IO_ERR;

    if (!o && e->pollfd.events)
        g_source_add_poll(&e->source, &e->pollfd);
    else if (o && !e->pollfd.events)
        g_source_remove_poll(&e->source, &e->pollfd);
}

static void glib_io_free(struct pa_io_event*e) {
    assert(e && !e->dead);

    g_source_destroy(&e->source);
    
    if (e->prev)
        e->prev->next = e->next;
    else
        e->mainloop->io_events = e->next;

    if (e->next)
        e->next->prev = e->prev;

    if ((e->next = e->mainloop->dead_io_events))
        e->next->prev = e;

    e->mainloop->dead_io_events = e;
    e->prev = NULL;

    e->dead = 1;
    schedule_free_dead_events(e->mainloop);
}

static void glib_io_set_destroy(struct pa_io_event*e, void (*callback)(struct pa_mainloop_api*m, struct pa_io_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* Time sources */

static void glib_time_restart(struct pa_time_event*e, const struct timeval *tv);

static struct pa_time_event* glib_time_new(struct pa_mainloop_api*m, const struct timeval *tv, void (*callback) (struct pa_mainloop_api*m, struct pa_time_event*e, const struct timeval *tv, void *userdata), void *userdata) {
    struct pa_glib_mainloop *g;
    struct pa_time_event *e;
    
    assert(m && m->userdata && tv && callback);
    g = m->userdata;

    e = pa_xmalloc(sizeof(struct pa_time_event));
    e->mainloop = g;
    e->dead = 0;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;
    e->source = NULL;

    glib_time_restart(e, tv);

    e->next = g->time_events;
    if (e->next) e->next->prev = e;
    g->time_events = e;
    e->prev = NULL;
    
    return e;
}

static guint msec_diff(const struct timeval *a, const struct timeval *b) {
    guint r;
    assert(a && b);
    
    if (a->tv_sec < b->tv_sec)
        return 0;

    if (a->tv_sec == b->tv_sec && a->tv_sec <= b->tv_sec)
        return 0;

    r = (a->tv_sec-b->tv_sec)*1000;

    if (a->tv_usec >= b->tv_usec)
        r += (a->tv_usec - b->tv_usec) / 1000;
    else
        r -= (b->tv_usec - a->tv_usec) / 1000;
    
    return r;
}

static gboolean time_cb(gpointer data) {
    struct pa_time_event* e = data;
    assert(e && e->mainloop && e->source);

    g_source_unref(e->source);
    e->source = NULL;

    e->callback(&e->mainloop->api, e, &e->timeval, e->userdata);
    return FALSE;
}

static void glib_time_restart(struct pa_time_event*e, const struct timeval *tv) {
    struct timeval now;
    assert(e && e->mainloop);

    gettimeofday(&now, NULL);
    if (e->source) {
        g_source_destroy(e->source);
        g_source_unref(e->source);
    }

    if (tv) {
        e->timeval = *tv;
        e->source = g_timeout_source_new(msec_diff(tv, &now));
        assert(e->source);
        g_source_set_callback(e->source, time_cb, e, NULL);
        g_source_attach(e->source, g_main_loop_get_context(e->mainloop->glib_mainloop));
    } else
        e->source = NULL;
 }

static void glib_time_free(struct pa_time_event *e) {
    assert(e && e->mainloop);

    if (e->source) {
        g_source_destroy(e->source);
        g_source_unref(e->source);
        e->source = NULL;
    }

    if (e->prev)
        e->prev->next = e->next;
    else
        e->mainloop->time_events = e->next;

    if (e->next)
        e->next->prev = e->prev;

    if ((e->next = e->mainloop->dead_time_events))
        e->next->prev = e;

    e->mainloop->dead_time_events = e;
    e->prev = NULL;

    e->dead = 1;
    schedule_free_dead_events(e->mainloop);
}

static void glib_time_set_destroy(struct pa_time_event *e, void (*callback)(struct pa_mainloop_api*m, struct pa_time_event*e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* Deferred sources */

static void glib_defer_enable(struct pa_defer_event *e, int b);

static struct pa_defer_event* glib_defer_new(struct pa_mainloop_api*m, void (*callback) (struct pa_mainloop_api*m, struct pa_defer_event *e, void *userdata), void *userdata) {
    struct pa_defer_event *e;
    struct pa_glib_mainloop *g;

    assert(m && m->userdata && callback);
    g = m->userdata;
    
    e = pa_xmalloc(sizeof(struct pa_defer_event));
    e->mainloop = g;
    e->dead = 0;
    e->callback = callback;
    e->userdata = userdata;
    e->destroy_callback = NULL;
    e->source = NULL;

    glib_defer_enable(e, 1);

    e->next = g->defer_events;
    if (e->next) e->next->prev = e;
    g->defer_events = e;
    e->prev = NULL;
    return e;
}

static gboolean idle_cb(gpointer data) {
    struct pa_defer_event* e = data;
    assert(e && e->mainloop && e->source);

    e->callback(&e->mainloop->api, e, e->userdata);
    return TRUE;
}

static void glib_defer_enable(struct pa_defer_event *e, int b) {
    assert(e && e->mainloop);

    if (e->source && !b) {
        g_source_destroy(e->source);
        g_source_unref(e->source);
        e->source = NULL;
    } else if (!e->source && b) {
        e->source = g_idle_source_new();
        assert(e->source);
        g_source_set_callback(e->source, idle_cb, e, NULL);
        g_source_attach(e->source, g_main_loop_get_context(e->mainloop->glib_mainloop));
        g_source_set_priority(e->source, G_PRIORITY_HIGH_IDLE);
    }
}

static void glib_defer_free(struct pa_defer_event *e) {
    assert(e && e->mainloop);
    
    if (e->source) {
        g_source_destroy(e->source);
        g_source_unref(e->source);
        e->source = NULL;
    }

    if (e->prev)
        e->prev->next = e->next;
    else
        e->mainloop->defer_events = e->next;

    if (e->next)
        e->next->prev = e->prev;

    if ((e->next = e->mainloop->dead_defer_events))
        e->next->prev = e;

    e->mainloop->dead_defer_events = e;
    e->prev = NULL;

    e->dead = 1;
    schedule_free_dead_events(e->mainloop);
}

static void glib_defer_set_destroy(struct pa_defer_event *e, void (*callback)(struct pa_mainloop_api *m, struct pa_defer_event *e, void *userdata)) {
    assert(e);
    e->destroy_callback = callback;
}

/* quit() */

static void glib_quit(struct pa_mainloop_api*a, int retval) {
    struct pa_glib_mainloop *g;
    assert(a && a->userdata);
    g = a->userdata;
    
    g_main_loop_quit(g->glib_mainloop);
}

static const struct pa_mainloop_api vtable = {
    userdata: NULL,

    io_new: glib_io_new,
    io_enable: glib_io_enable,
    io_free: glib_io_free,
    io_set_destroy: glib_io_set_destroy,

    time_new : glib_time_new,
    time_restart : glib_time_restart,
    time_free : glib_time_free,
    time_set_destroy : glib_time_set_destroy,
    
    defer_new : glib_defer_new,
    defer_enable : glib_defer_enable,
    defer_free : glib_defer_free,
    defer_set_destroy : glib_defer_set_destroy,
    
    quit : glib_quit,
};

struct pa_glib_mainloop *pa_glib_mainloop_new(GMainLoop *ml) {
    struct pa_glib_mainloop *g;
    assert(ml);
    
    g = pa_xmalloc(sizeof(struct pa_glib_mainloop));
    g->glib_mainloop = g_main_loop_ref(ml);
    g->api = vtable;
    g->api.userdata = g;

    g->io_events = g->dead_io_events = NULL;
    g->time_events = g->dead_time_events = NULL;
    g->defer_events = g->dead_defer_events = NULL;

    g->cleanup_source = NULL;
    return g;
}

static void free_io_events(struct pa_io_event *e) {
    while (e) {
        struct pa_io_event *r = e;
        e = r->next;

        if (r->pollfd.events)
            g_source_remove_poll(&r->source, &r->pollfd);

        if (!r->dead)
            g_source_destroy(&r->source);
        
        if (r->destroy_callback)
            r->destroy_callback(&r->mainloop->api, r, r->userdata);

        g_source_unref(&r->source);
    }
}

static void free_time_events(struct pa_time_event *e) {
    while (e) {
        struct pa_time_event *r = e;
        e = r->next;

        if (r->source) {
            g_source_destroy(r->source);
            g_source_unref(r->source);
        }
        
        if (r->destroy_callback)
            r->destroy_callback(&r->mainloop->api, r, r->userdata);

        pa_xfree(r);
    }
}

static void free_defer_events(struct pa_defer_event *e) {
    while (e) {
        struct pa_defer_event *r = e;
        e = r->next;

        if (r->source) {
            g_source_destroy(r->source);
            g_source_unref(r->source);
        }
        
        if (r->destroy_callback)
            r->destroy_callback(&r->mainloop->api, r, r->userdata);

        pa_xfree(r);
    }
}

void pa_glib_mainloop_free(struct pa_glib_mainloop* g) {
    assert(g);

    free_io_events(g->io_events);
    free_io_events(g->dead_io_events);
    free_defer_events(g->defer_events);
    free_defer_events(g->dead_defer_events);
    free_time_events(g->time_events);
    free_time_events(g->dead_time_events);

    g_main_loop_unref(g->glib_mainloop);
    pa_xfree(g);
}

struct pa_mainloop_api* pa_glib_mainloop_get_api(struct pa_glib_mainloop *g) {
    assert(g);
    return &g->api;
}

static gboolean free_dead_events(gpointer p) {
    struct pa_glib_mainloop *g = p;
    assert(g);

    free_io_events(g->dead_io_events);
    free_defer_events(g->dead_defer_events);
    free_time_events(g->dead_time_events);

    g_source_destroy(g->cleanup_source);
    g_source_unref(g->cleanup_source);
    g->cleanup_source = NULL;

    return FALSE;
}

static void schedule_free_dead_events(struct pa_glib_mainloop *g) {
    assert(g && g->glib_mainloop);

    if (g->cleanup_source)
        return;
    
    g->cleanup_source = g_idle_source_new();
    assert(g->cleanup_source);
    g_source_set_callback(g->cleanup_source, free_dead_events, g, NULL);
    g_source_attach(g->cleanup_source, g_main_loop_get_context(g->glib_mainloop));
}
