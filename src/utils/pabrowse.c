/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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
#include <assert.h>
#include <signal.h>

#include <pulse/browser.h>
#include <pulse/pulseaudio.h>
#include <pulse/rtclock.h>

#include <pulsecore/core-util.h>

static void exit_signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata) {
    fprintf(stderr, "Got signal, exiting\n");
    m->quit(m, 0);
}

static void dump_server(const pa_browse_info *i) {
    char t[16];

    if (i->cookie)
        snprintf(t, sizeof(t), "0x%08x", *i->cookie);

    printf("server: %s\n"
           "server-version: %s\n"
           "user-name: %s\n"
           "fqdn: %s\n"
           "cookie: %s\n",
           i->server,
           i->server_version ? i->server_version : "n/a",
           i->user_name ? i->user_name : "n/a",
           i->fqdn ? i->fqdn : "n/a",
           i->cookie ? t : "n/a");
}

static void dump_device(const pa_browse_info *i) {
    char ss[PA_SAMPLE_SPEC_SNPRINT_MAX];

    if (i->sample_spec)
        pa_sample_spec_snprint(ss, sizeof(ss), i->sample_spec);

    printf("device: %s\n"
           "description: %s\n"
           "sample spec: %s\n",
           i->device,
           i->description ? i->description : "n/a",
           i->sample_spec ? ss : "n/a");

}

static void browser_callback(pa_browser *b, pa_browse_opcode_t c, const pa_browse_info *i, void *userdata) {
    assert(b && i);

    switch (c) {

        case PA_BROWSE_NEW_SERVER:
            printf("\n=> new server <%s>\n", i->name);
            dump_server(i);
            break;

        case PA_BROWSE_NEW_SINK:
            printf("\n=> new sink <%s>\n", i->name);
            dump_server(i);
            dump_device(i);
            break;

        case PA_BROWSE_NEW_SOURCE:
            printf("\n=> new source <%s>\n", i->name);
            dump_server(i);
            dump_device(i);
            break;

        case PA_BROWSE_REMOVE_SERVER:
            printf("\n=> removed server <%s>\n", i->name);
            break;

        case PA_BROWSE_REMOVE_SINK:
            printf("\n=> removed sink <%s>\n", i->name);
            break;

        case PA_BROWSE_REMOVE_SOURCE:
            printf("\n=> removed source <%s>\n", i->name);
            break;

        default:
            ;
    }
}

static void error_callback(pa_browser *b, const char *s, void *userdata) {
    pa_mainloop_api*m = userdata;

    fprintf(stderr, "Failure: %s\n", s);
    m->quit(m, 1);
}

int main(int argc, char *argv[]) {
    pa_mainloop *mainloop = NULL;
    pa_browser *browser = NULL;
    int ret = 1, r;
    const char *s;

    if (!(mainloop = pa_mainloop_new()))
        goto finish;

    r = pa_signal_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
    pa_signal_new(SIGTERM, exit_signal_callback, NULL);
    pa_disable_sigpipe();

    if (!(browser = pa_browser_new_full(pa_mainloop_get_api(mainloop), PA_BROWSE_FOR_SERVERS|PA_BROWSE_FOR_SINKS|PA_BROWSE_FOR_SOURCES, &s))) {
        fprintf(stderr, "pa_browse_new_full(): %s\n", s);
        goto finish;
    }

    pa_browser_set_callback(browser, browser_callback, NULL);
    pa_browser_set_error_callback(browser, error_callback, pa_mainloop_get_api(mainloop));

    ret = 0;
    pa_mainloop_run(mainloop, &ret);

finish:

    if (browser)
        pa_browser_unref(browser);

    if (mainloop) {
        pa_signal_done();
        pa_mainloop_free(mainloop);
    }

    return ret;
}
