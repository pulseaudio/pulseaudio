/***
  This file is part of PulseAudio.

  Copyright 2020 Greg V

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

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pulsecore/core-util.h>
#include <pulsecore/module.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/ioline.h>
#include <pulsecore/log.h>

PA_MODULE_AUTHOR("Greg V");
PA_MODULE_DESCRIPTION("Detect hotplugged audio hardware and load matching drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE("");

struct userdata {
    pa_core *core;
    pa_hashmap *devices;
    pa_iochannel *io;
    pa_ioline *line;
};

static void line_callback(pa_ioline *line, const char *s, void *userdata) {
    struct userdata *u = userdata;
    pa_module *m = NULL;
    unsigned devnum;
    uint32_t modidx;
    char args[64];

    pa_assert(line);
    pa_assert(u);

    if (sscanf(s, "+pcm%u", &devnum) == 1) {
        pa_snprintf(args, sizeof(args), "device=/dev/dsp%u", devnum);
        pa_module_load(&m, u->core, "module-oss", args);

        if (m) {
            pa_hashmap_put(u->devices, (void *)(uintptr_t)devnum, (void *)(uintptr_t)m->index);
            pa_log_info("Card %u module loaded (%u).", devnum, m->index);
        } else {
            pa_log_info("Card %u failed to load module.", devnum);
        }
    } else if (sscanf(s, "-pcm%u", &devnum) == 1) {
        if (!(modidx = (uint32_t)pa_hashmap_remove(u->devices, (void *)(uintptr_t)devnum)))
            return;

        pa_log_info("Card %u (module %u) removed.", devnum, modidx);

        if (modidx != PA_INVALID_INDEX)
            pa_module_unload_request_by_index(u->core, modidx, true);
    }
}

static void device_free(void *a) {
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd;

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->devices = pa_hashmap_new_full(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func, NULL, (pa_free_cb_t) device_free);

    if ((fd = socket(AF_UNIX, SOCK_SEQPACKET, 0)) < 0) {
        pa_log("Failed to open socket for devd.");
        return -1;
    }

    strncpy(addr.sun_path, "/var/run/devd.seqpacket.pipe", sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        pa_log("Failed to connect to devd.");
        close(fd);
        return -1;
    }

    pa_assert_se(u->io = pa_iochannel_new(m->core->mainloop, fd, -1));
    pa_assert_se(u->line = pa_ioline_new(u->io));
    pa_ioline_set_callback(u->line, line_callback, m->userdata);

    return 0;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->devices)
        pa_hashmap_free(u->devices);

    if (u->line)
        pa_ioline_close(u->line);

    if (u->io)
        pa_iochannel_free(u->io);

    pa_xfree(u);
}
