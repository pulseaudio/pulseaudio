/***
  This file is part of PulseAudio.

  Copyright 2005-2006 Lennart Poettering

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
#include <string.h>
#include <stdlib.h>

#include <lirc/lirc_client.h>

#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/macro.h>

#include "module-lirc-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("LIRC volume control");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE("config=<config file> sink=<sink name> appname=<lirc application name>");

static const char* const valid_modargs[] = {
    "config",
    "sink",
    "appname",
    NULL,
};

struct userdata {
    int lirc_fd;
    pa_io_event *io;
    struct lirc_config *config;
    char *sink_name;
    pa_module *module;
    float mute_toggle_save;
};

#define DELTA (PA_VOLUME_NORM/20)

static void io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t events, void*userdata) {
    struct userdata *u = userdata;
    char *name = NULL, *code = NULL;

    pa_assert(io);
    pa_assert(u);

    if (events & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) {
        pa_log("Lost connection to LIRC daemon.");
        goto fail;
    }

    if (events & PA_IO_EVENT_INPUT) {
        char *c;

        if (lirc_nextcode(&code) != 0 || !code) {
            pa_log("lirc_nextcode() failed.");
            goto fail;
        }

        c = pa_xstrdup(code);
        c[strcspn(c, "\n\r")] = 0;
        pa_log_debug("Raw IR code '%s'", c);
        pa_xfree(c);

        while (lirc_code2char(u->config, code, &name) == 0 && name) {
            enum {
                INVALID,
                UP,
                DOWN,
                MUTE,
                RESET,
                MUTE_TOGGLE
            } volchange = INVALID;

            pa_log_info("Translated IR code '%s'", name);

            if (strcasecmp(name, "volume-up") == 0)
                volchange = UP;
            else if (strcasecmp(name, "volume-down") == 0)
                volchange = DOWN;
            else if (strcasecmp(name, "mute") == 0)
                volchange = MUTE;
            else if (strcasecmp(name, "mute-toggle") == 0)
                volchange = MUTE_TOGGLE;
            else if (strcasecmp(name, "reset") == 0)
                volchange = RESET;

            if (volchange == INVALID)
                pa_log_warn("Received unknown IR code '%s'", name);
            else {
                pa_sink *s;

                if (!(s = pa_namereg_get(u->module->core, u->sink_name, PA_NAMEREG_SINK)))
                    pa_log("Failed to get sink '%s'", u->sink_name);
                else {
                    pa_cvolume cv = *pa_sink_get_volume(s, FALSE);

                    switch (volchange) {
                        case UP:
                            pa_cvolume_inc(&cv, DELTA);
                            pa_sink_set_volume(s, &cv, TRUE, TRUE);
                            break;

                        case DOWN:
                            pa_cvolume_dec(&cv, DELTA);
                            pa_sink_set_volume(s, &cv, TRUE, TRUE);
                            break;

                        case MUTE:
                            pa_sink_set_mute(s, TRUE, TRUE);
                            break;

                        case RESET:
                            pa_sink_set_mute(s, FALSE, TRUE);
                            break;

                        case MUTE_TOGGLE:
                            pa_sink_set_mute(s, !pa_sink_get_mute(s, FALSE), TRUE);
                            break;

                        case INVALID:
                            pa_assert_not_reached();
                    }
                }
            }
        }
    }

    pa_xfree(code);

    return;

fail:
    u->module->core->mainloop->io_free(u->io);
    u->io = NULL;

    pa_module_unload_request(u->module, TRUE);

    pa_xfree(code);
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->module = m;
    u->io = NULL;
    u->config = NULL;
    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));
    u->lirc_fd = -1;
    u->mute_toggle_save = 0;

    if ((u->lirc_fd = lirc_init((char*) pa_modargs_get_value(ma, "appname", "pulseaudio"), 1)) < 0) {
        pa_log("lirc_init() failed.");
        goto fail;
    }

    if (lirc_readconfig((char*) pa_modargs_get_value(ma, "config", NULL), &u->config, NULL) < 0) {
        pa_log("lirc_readconfig() failed.");
        goto fail;
    }

    u->io = m->core->mainloop->io_new(m->core->mainloop, u->lirc_fd, PA_IO_EVENT_INPUT|PA_IO_EVENT_HANGUP, io_callback, u);

    pa_modargs_free(ma);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->io)
        m->core->mainloop->io_free(u->io);

    if (u->config)
        lirc_freeconfig(u->config);

    if (u->lirc_fd >= 0)
        lirc_deinit();

    pa_xfree(u->sink_name);
    pa_xfree(u);
}
