#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <assert.h>

#define GLIB_MAIN_LOOP

#ifdef GLIB_MAIN_LOOP
#include <glib.h>
#include "glib-mainloop.h"
static GMainLoop* glib_main_loop = NULL;
#else
#include "mainloop.h"
#endif

static struct pa_defer_event *de;

static void iocb(struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags f, void *userdata) {
    unsigned char c;
    read(fd, &c, sizeof(c));
    fprintf(stderr, "IO EVENT: %c\n", c < 32 ? '.' : c);
    a->defer_enable(de, 1);
}

static void dcb(struct pa_mainloop_api*a, struct pa_defer_event *e, void *userdata) {
    fprintf(stderr, "DEFER EVENT\n");
    a->defer_enable(e, 0);
}

static void tcb(struct pa_mainloop_api*a, struct pa_time_event *e, const struct timeval *tv, void *userdata) {
    fprintf(stderr, "TIME EVENT\n");

#ifdef GLIB_MAIN_LOOP
    g_main_loop_quit(glib_main_loop);
#else
    a->quit(a, 0);
#endif
}

int main(int argc, char *argv[]) {
    struct pa_mainloop_api *a;
    struct pa_io_event *ioe;
    struct pa_time_event *te;
    struct timeval tv;

#ifdef GLIB_MAIN_LOOP
    struct pa_glib_mainloop *g;
    glib_main_loop = g_main_loop_new(NULL, FALSE);
    assert(glib_main_loop);

    g = pa_glib_mainloop_new(NULL);
    assert(g);

    a = pa_glib_mainloop_get_api(g);
    assert(a);
#else
    struct pa_mainloop *m;

    m = pa_mainloop_new();
    assert(m);

    a = pa_mainloop_get_api(m);
    assert(a);
#endif

    ioe = a->io_new(a, 0, PA_IO_EVENT_INPUT, iocb, NULL);
    assert(ioe);

    de = a->defer_new(a, dcb, NULL);
    assert(de);

    gettimeofday(&tv, NULL);
    tv.tv_sec += 10;
    te = a->time_new(a, &tv, tcb, NULL);

#ifdef GLIB_MAIN_LOOP
    g_main_loop_run(glib_main_loop);
#else
    pa_mainloop_run(m, NULL);
#endif

    a->time_free(te);
    a->defer_free(de);
    a->io_free(ioe);

#ifdef GLIB_MAIN_LOOP
    pa_glib_mainloop_free(g);
    g_main_loop_unref(glib_main_loop);
#else
    pa_mainloop_free(m);
#endif
    
    return 0;
}
