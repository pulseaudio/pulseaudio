/***
    This file is part of PulseAudio.

    Copyright 2008 Joao Paulo Rechi Vita

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
    USA.

    1. Connects to the BlueZ audio service via one BlueZ specific well known unix socket
        bluez/audio/ipc: bt_audio_service_open()
    2. Configures a connection to the BT device
    3. Gets a BT socket fd passed in via the unix socket
        bluez/audio/ipc: bt_audio_service_get_data_fd()
    4. Hands this over to its RT thread.
        pa_thread_mq
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>

#include "dbus-util.h"
#include "module-bt-device-symdef.h"
#include "bt-ipc.h"

PA_MODULE_AUTHOR("Joao Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Bluetooth audio sink and source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "addr=<address of the device> "
        "uuid=<the profile that this device will work on>");

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    const char *addr;
    const char *uuid;
};

static const char* const valid_modargs[] = {
    "addr",
    "uuid",
    NULL
};

static int audioservice_send(int sk, const bt_audio_msg_header_t *msg) {
    int e;
    pa_log("sending %s", bt_audio_strmsg(msg->msg_type)); /*debug*/
    if (send(sk, msg, BT_AUDIO_IPC_PACKET_SIZE, 0) > 0)
        e = 0;
    else {
        e = -errno;
        pa_log_error("Error sending data to audio service: %s(%d)", strerror(errno), errno);
    }
    return e;
}

static int audioservice_recv(int sk, bt_audio_msg_header_t *inmsg) {
    int err;
    const char *type;

    pa_log/*_debug*/("trying to receive msg from audio service...");
    if (recv(sk, inmsg, BT_AUDIO_IPC_PACKET_SIZE, 0) > 0) {
        type = bt_audio_strmsg(inmsg->msg_type);
        if (type) {
            pa_log/*_debug*/("Received %s", type);
            err = 0;
        }
        else {
            err = -EINVAL;
            pa_log_error("Bogus message type %d received from audio service", inmsg->msg_type);
        }
    }
    else {
        err = -errno;
        pa_log_error("Error receiving data from audio service: %s(%d)", strerror(errno), errno);
    }

    return err;
}

static int audioservice_expect(int sk, bt_audio_msg_header_t *rsp_hdr, int expected_type) {
    int err = audioservice_recv(sk, rsp_hdr);
    if (err == 0) {
        if (rsp_hdr->msg_type != expected_type) {
            err = -EINVAL;
            pa_log_error("Bogus message %s received while %s was expected", bt_audio_strmsg(rsp_hdr->msg_type),
                    bt_audio_strmsg(expected_type));
        }
    }
    return err;
}

int pa__init(pa_module* m) {
    pa_modargs *ma;
    struct userdata *u;
    int sk = -1;

    pa_assert(m);
    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("failed to parse module arguments");
        goto fail;
    }
    if (!(u->addr = pa_modargs_get_value(ma, "addr", NULL))) {
        pa_log_error("failed to parse device address from module arguments");
        goto fail;
    }
    if (!(u->uuid = pa_modargs_get_value(ma, "uuid", NULL))) {
        pa_log_error("failed to parse device uuid from module arguments");
        goto fail;
    }
    pa_log("Loading module-bt-device for %s, UUID=%s", u->addr, u->uuid);

    /* Connects to the BlueZ audio service via one BlueZ specific well known unix socket */
    sk = bt_audio_service_open();
    if (sk <= 0) {
        pa_log_error("couldn't connect to bluetooth audio service");
        goto fail;
    }
    pa_log("socket to audio service: %d", sk); /*debug*/

    return 0;

fail:
    pa__done(m);
    return -1;
}

void pa__done(pa_module *m) {
    pa_log("Unloading module-bt-device");
}

