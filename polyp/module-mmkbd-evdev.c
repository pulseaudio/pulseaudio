/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <linux/input.h>

#include "module.h"
#include "log.h"
#include "module-mmkbd-evdev-symdef.h"
#include "namereg.h"
#include "sink.h"
#include "xmalloc.h"
#include "modargs.h"
#include "util.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Multimedia keyboard support via Linux evdev")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("device=<evdev device> sink=<sink name>")

#define DEFAULT_DEVICE "/dev/input/event0"

/*
 * This isn't defined in older kernel headers and there is no way of
 * detecting it.
 */
struct _input_id {
    __u16 bustype;
    __u16 vendor;
    __u16 product;
    __u16 version;
};

static const char* const valid_modargs[] = {
    "device",
    "sink",
    NULL,
};

struct userdata {
    int fd;
    struct pa_io_event *io;
    char *sink_name;
    struct pa_module *module;
    float mute_toggle_save;
};

static void io_callback(struct pa_mainloop_api *io, struct pa_io_event *e, int fd, enum pa_io_event_flags events, void*userdata) {
    struct userdata *u = userdata;
    assert(io);
    assert(u);

    if (events & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) {
        pa_log(__FILE__": lost connection to evdev device.\n");
        goto fail;
    }
        
    if (events & PA_IO_EVENT_INPUT) {
        struct input_event e;

        if (pa_loop_read(u->fd, &e, sizeof(e)) <= 0) {
            pa_log(__FILE__": failed to read from event device: %s\n", strerror(errno));
            goto fail;
        }

        if (e.type == EV_KEY && (e.value == 1 || e.value == 2)) {
            enum { INVALID, UP, DOWN, MUTE_TOGGLE } volchange = INVALID;

            pa_log_debug(__FILE__": key code=%u, value=%u\n", e.code, e.value);

            switch (e.code) {
                case KEY_VOLUMEDOWN:  volchange = DOWN; break;
                case KEY_VOLUMEUP:    volchange = UP; break;
                case KEY_MUTE:        volchange = MUTE_TOGGLE; break;
            }

            if (volchange != INVALID) {
                struct pa_sink *s;
                
                if (!(s = pa_namereg_get(u->module->core, u->sink_name, PA_NAMEREG_SINK, 1)))
                    pa_log(__FILE__": failed to get sink '%s'\n", u->sink_name);
                else {
                    double v = pa_volume_to_user(s->volume);
                    
                    switch (volchange) {
                        case UP:       v += .05; break;
                        case DOWN:     v -= .05; break;
                        case MUTE_TOGGLE: {

                            if (v > 0) {
                                u->mute_toggle_save = v;
                                v = 0;
                            } else
                                v = u->mute_toggle_save;
                        }
                        default:
                            ;
                    }
                    
                    pa_sink_set_volume(s, pa_volume_from_user(v));
                }
            }
        }
    }

    return;
    
fail:
    u->module->core->mainloop->io_free(u->io);
    u->io = NULL;

    pa_module_unload_request(u->module);
}

#define test_bit(bit, array) (array[bit/8] & (1<<(bit%8)))
    
int pa__init(struct pa_core *c, struct pa_module*m) {
    struct pa_modargs *ma = NULL;
    struct userdata *u;
    int version;
    struct _input_id input_id;
    char name[256];
    uint8_t evtype_bitmask[EV_MAX/8 + 1];
    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": Failed to parse module arguments\n");
        goto fail;
    }

    m->userdata = u = pa_xmalloc(sizeof(struct userdata));
    u->module = m;
    u->io = NULL;
    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));
    u->fd = -1;
    u->mute_toggle_save = 0;

    if ((u->fd = open(pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), O_RDONLY)) < 0) {
        pa_log(__FILE__": failed to open evdev device: %s\n", strerror(errno));
        goto fail;
    }

    if (ioctl(u->fd, EVIOCGVERSION, &version) < 0) {
        pa_log(__FILE__": EVIOCGVERSION failed: %s\n", strerror(errno));
        goto fail;
    }

    pa_log_info(__FILE__": evdev driver version %i.%i.%i\n", version >> 16, (version >> 8) & 0xff, version & 0xff);

    if(ioctl(u->fd, EVIOCGID, &input_id)) {
        pa_log(__FILE__": EVIOCGID failed: %s\n", strerror(errno));
        goto fail;
    }

    pa_log_info(__FILE__": evdev vendor 0x%04hx product 0x%04hx version 0x%04hx bustype %u\n",
                input_id.vendor, input_id.product, input_id.version, input_id.bustype);

    if(ioctl(u->fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        pa_log(__FILE__": EVIOCGNAME failed: %s\n", strerror(errno));
        goto fail;
    }

    pa_log_info(__FILE__": evdev device name: %s\n", name);

    memset(evtype_bitmask, 0, sizeof(evtype_bitmask));
    if (ioctl(u->fd, EVIOCGBIT(0, EV_MAX), evtype_bitmask) < 0) {
        pa_log(__FILE__": EVIOCGBIT failed: %s\n", strerror(errno));
        goto fail;
    }

    if (!test_bit(EV_KEY, evtype_bitmask)) {
        pa_log(__FILE__": device has no keys.\n");
        goto fail;
    }

    u->io = c->mainloop->io_new(c->mainloop, u->fd, PA_IO_EVENT_INPUT|PA_IO_EVENT_HANGUP, io_callback, u);

    pa_modargs_free(ma);
    
    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(c, m);
    return -1;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c);
    assert(m);

    if (!(u = m->userdata))
        return;

    if (u->io)
        m->core->mainloop->io_free(u->io);

    if (u->fd >= 0)
        close(u->fd);

    pa_xfree(u->sink_name);
    pa_xfree(u);
}
