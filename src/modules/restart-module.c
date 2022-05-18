/***
    This file is part of PulseAudio.

    Copyright 2022 Craig Howard

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "restart-module.h"

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
#include <pulse/mainloop.h>

#include <pulsecore/core.h>
#include <pulsecore/thread-mq.h>

struct pa_restart_data {
    init_cb do_init;
    done_cb do_done;

    pa_usec_t restart_usec;
    pa_module *module;
    pa_time_event *time_event;
    pa_defer_event *defer_event;
};

static void do_reinit(pa_mainloop_api *mainloop, pa_restart_data *rd);

static void call_init(pa_mainloop_api *mainloop, pa_time_event *e, const struct timeval *tv, void *userdata) {
    pa_restart_data *rd = userdata;
    int ret;

    if (rd->time_event) {
        mainloop->time_free(rd->time_event);
        rd->time_event = NULL;
    }

    /* now that restart_usec has elapsed, we call do_init to restart the module */
    ret = rd->do_init(rd->module);

    /* if the init failed, we got here because the caller wanted to restart, so
     * setup another restart */
    if (ret < 0)
        do_reinit(mainloop, rd);
}

static void defer_callback(pa_mainloop_api *mainloop, pa_defer_event *e, void *userdata) {
    pa_restart_data *rd = userdata;

    pa_assert(rd->defer_event == e);

    mainloop->defer_enable(rd->defer_event, 0);
    mainloop->defer_free(rd->defer_event);
    rd->defer_event = NULL;

    do_reinit(mainloop, rd);
}

static void do_reinit(pa_mainloop_api *mainloop, pa_restart_data *rd) {
    struct timeval tv;

    pa_assert_ctl_context();

    /* call do_done on the module, which will effectively tear it down; all
     * that remains is the pa_module */
    rd->do_done(rd->module);

    /* after restart_usec, call do_init to restart the module */
    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, rd->restart_usec);
    rd->time_event = mainloop->time_new(mainloop, &tv, call_init, rd);
}

pa_restart_data *pa_restart_module_reinit(pa_module *m, init_cb do_init, done_cb do_done, pa_usec_t restart_usec) {
    pa_restart_data *rd;

    pa_assert_ctl_context();
    pa_assert(do_init);
    pa_assert(do_done);
    pa_assert(restart_usec);

    pa_log_info("Starting reinit for %s", m->name);

    rd = pa_xnew0(pa_restart_data, 1);
    rd->do_init = do_init;
    rd->do_done = do_done;
    rd->restart_usec = restart_usec;
    rd->module = m;

    /* defer actually doing a reinit, so that we can safely exit whatever call
     * chain we're in before we effectively reinit the module */
    rd->defer_event = m->core->mainloop->defer_new(m->core->mainloop, defer_callback, rd);
    m->core->mainloop->defer_enable(rd->defer_event, 1);

    return rd;
}

void pa_restart_free(pa_restart_data *rd) {
    pa_assert_ctl_context();
    pa_assert(rd);

    if (rd->defer_event) {
        rd->module->core->mainloop->defer_enable(rd->defer_event, 0);
        rd->module->core->mainloop->defer_free(rd->defer_event);
    }

    if (rd->time_event) {
        pa_log_info("Cancel reinit for %s", rd->module->name);
        rd->module->core->mainloop->time_free(rd->time_event);
    }

    pa_xfree(rd);
}
