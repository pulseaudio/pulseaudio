/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <sys/types.h>
#include <sys/wait.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/start-child.h>

#include "../stdin-util.h"

PA_MODULE_AUTHOR("Sylvain Baubeau");
PA_MODULE_DESCRIPTION("GSettings Adapter");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

int pa__init(pa_module*m) {
    struct userdata *u;
    int r;

    u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->module_infos = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t) module_info_free);
    u->pid = (pid_t) -1;
    u->fd = -1;
    u->fd_type = 0;
    u->io_event = NULL;
    u->buf_fill = 0;

    if ((u->fd = pa_start_child_for_read(
#if defined(__linux__) && defined(HAVE_RUNNING_FROM_BUILD_TREE)
#ifdef MESON_BUILD
                              pa_run_from_build_tree() ? PA_BUILDDIR PA_PATH_SEP "src" PA_PATH_SEP "modules" PA_PATH_SEP "gsettings" PA_PATH_SEP "gsettings-helper" :
#else
                              pa_run_from_build_tree() ? PA_BUILDDIR "/gsettings-helper" :
#endif
#endif
                 PA_GSETTINGS_HELPER, NULL, &u->pid)) < 0)
        goto fail;

    u->io_event = m->core->mainloop->io_new(
            m->core->mainloop,
            u->fd,
            PA_IO_EVENT_INPUT,
            io_event_cb,
            u);

    do {
        if ((r = handle_event(u)) < 0)
            goto fail;

        /* Read until the client signalled us that it is ready with
         * initialization */
    } while (r != 1);

    return 0;

fail:
    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->pid != (pid_t) -1) {
        kill(u->pid, SIGTERM);

        for (;;) {
            if (waitpid(u->pid, NULL, 0) >= 0)
                break;

            if (errno != EINTR) {
                pa_log("waitpid() failed: %s", pa_cstrerror(errno));
                break;
            }
        }
    }

    if (u->io_event)
        m->core->mainloop->io_free(u->io_event);

    if (u->fd >= 0)
        pa_close(u->fd);

    if (u->module_infos)
        pa_hashmap_free(u->module_infos);

    pa_xfree(u);
}
