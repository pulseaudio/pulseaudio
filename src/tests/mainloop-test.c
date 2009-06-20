/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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
#include <unistd.h>
#include <sys/time.h>
#include <assert.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/gccmacro.h>

#include <pulsecore/core-util.h>
#include <pulsecore/core-rtclock.h>

#ifdef GLIB_MAIN_LOOP

#include <glib.h>
#include <pulse/glib-mainloop.h>

static GMainLoop* glib_main_loop = NULL;

#else /* GLIB_MAIN_LOOP */
#include <pulse/mainloop.h>
#endif /* GLIB_MAIN_LOOP */

static pa_defer_event *de;

static void iocb(pa_mainloop_api*a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata) {
    unsigned char c;
    (void) read(fd, &c, sizeof(c));
    fprintf(stderr, "IO EVENT: %c\n", c < 32 ? '.' : c);
    a->defer_enable(de, 1);
}

static void dcb(pa_mainloop_api*a, pa_defer_event *e, void *userdata) {
    fprintf(stderr, "DEFER EVENT\n");
    a->defer_enable(e, 0);
}

static void tcb(pa_mainloop_api*a, pa_time_event *e, const struct timeval *tv, void *userdata) {
    fprintf(stderr, "TIME EVENT\n");

#if defined(GLIB_MAIN_LOOP)
    g_main_loop_quit(glib_main_loop);
#else
    a->quit(a, 0);
#endif
}

int main(int argc, char *argv[]) {
    pa_mainloop_api *a;
    pa_io_event *ioe;
    pa_time_event *te;
    struct timeval tv;

#ifdef GLIB_MAIN_LOOP
    pa_glib_mainloop *g;

    glib_main_loop = g_main_loop_new(NULL, FALSE);
    assert(glib_main_loop);

    g = pa_glib_mainloop_new(NULL);
    assert(g);

    a = pa_glib_mainloop_get_api(g);
    assert(a);
#else /* GLIB_MAIN_LOOP */
    pa_mainloop *m;

    m = pa_mainloop_new();
    assert(m);

    a = pa_mainloop_get_api(m);
    assert(a);
#endif /* GLIB_MAIN_LOOP */

    ioe = a->io_new(a, 0, PA_IO_EVENT_INPUT, iocb, NULL);
    assert(ioe);

    de = a->defer_new(a, dcb, NULL);
    assert(de);

    te = a->time_new(a, pa_timeval_rtstore(&tv, pa_rtclock_now() + 2 * PA_USEC_PER_SEC, TRUE), tcb, NULL);

#if defined(GLIB_MAIN_LOOP)
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
