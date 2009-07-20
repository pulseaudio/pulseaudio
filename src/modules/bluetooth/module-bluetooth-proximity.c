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
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <pulse/xmalloc.h>
#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/start-child.h>
#include <pulsecore/dbus-shared.h>

#include "module-bluetooth-proximity-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Bluetooth Proximity Volume Control");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE(
        "sink=<sink name> "
        "hci=<hci device> "
);

#define DEFAULT_HCI "hci0"

static const char* const valid_modargs[] = {
    "sink",
    "rssi",
    "hci",
    NULL,
};

struct bonding {
    struct userdata *userdata;
    char address[18];

    pid_t pid;
    int fd;

    pa_io_event *io_event;

    enum {
        UNKNOWN,
        FOUND,
        NOT_FOUND
    } state;
};

struct userdata {
    pa_module *module;
    pa_dbus_connection *dbus_connection;

    char *sink_name;
    char *hci, *hci_path;

    pa_hashmap *bondings;

    unsigned n_found;
    unsigned n_unknown;

    pa_bool_t muted;
};

static void update_volume(struct userdata *u) {
    pa_assert(u);

    if (u->muted && u->n_found > 0) {
        pa_sink *s;

        u->muted = FALSE;

        if (!(s = pa_namereg_get(u->module->core, u->sink_name, PA_NAMEREG_SINK))) {
            pa_log_warn("Sink device '%s' not available for unmuting.", pa_strnull(u->sink_name));
            return;
        }

        pa_log_info("Found %u BT devices, unmuting.", u->n_found);
        pa_sink_set_mute(s, FALSE, FALSE);

    } else if (!u->muted && (u->n_found+u->n_unknown) <= 0) {
        pa_sink *s;

        u->muted = TRUE;

        if (!(s = pa_namereg_get(u->module->core, u->sink_name, PA_NAMEREG_SINK))) {
            pa_log_warn("Sink device '%s' not available for muting.", pa_strnull(u->sink_name));
            return;
        }

        pa_log_info("No BT devices found, muting.");
        pa_sink_set_mute(s, TRUE, FALSE);

    } else
        pa_log_info("%u devices now active, %u with unknown state.", u->n_found, u->n_unknown);
}

static void bonding_free(struct bonding *b) {
    pa_assert(b);

    if (b->state == FOUND)
        pa_assert_se(b->userdata->n_found-- >= 1);

    if (b->state == UNKNOWN)
        pa_assert_se(b->userdata->n_unknown-- >= 1);

    if (b->pid != (pid_t) -1) {
        kill(b->pid, SIGTERM);
        waitpid(b->pid, NULL, 0);
    }

    if (b->fd >= 0)
        pa_close(b->fd);

    if (b->io_event)
        b->userdata->module->core->mainloop->io_free(b->io_event);

    pa_xfree(b);
}

static void io_event_cb(
        pa_mainloop_api*a,
        pa_io_event* e,
        int fd,
        pa_io_event_flags_t events,
        void *userdata) {

    struct bonding *b = userdata;
    char x;
    ssize_t r;

    pa_assert(b);

    if ((r = read(fd, &x, 1)) <= 0) {
        pa_log_warn("Child watching '%s' died abnormally: %s", b->address, r == 0 ? "EOF" : pa_cstrerror(errno));

        pa_assert_se(pa_hashmap_remove(b->userdata->bondings, b->address) == b);
        bonding_free(b);
        return;
    }

    pa_assert_se(r == 1);

    if (b->state == UNKNOWN)
        pa_assert_se(b->userdata->n_unknown-- >= 1);

    if (x == '+') {
        pa_assert(b->state == UNKNOWN || b->state == NOT_FOUND);

        b->state = FOUND;
        b->userdata->n_found++;

        pa_log_info("Device '%s' is alive.", b->address);

    } else {
        pa_assert(x == '-');
        pa_assert(b->state == UNKNOWN || b->state == FOUND);

        if (b->state == FOUND)
            b->userdata->n_found--;

        b->state = NOT_FOUND;

        pa_log_info("Device '%s' is dead.", b->address);
    }

    update_volume(b->userdata);
}

static struct bonding* bonding_new(struct userdata *u, const char *a) {
    struct bonding *b = NULL;
    DBusMessage *m = NULL, *r = NULL;
    DBusError e;
    const char *class;

    pa_assert(u);
    pa_assert(a);

    pa_return_val_if_fail(strlen(a) == 17, NULL);
    pa_return_val_if_fail(!pa_hashmap_get(u->bondings, a), NULL);

    dbus_error_init(&e);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", u->hci_path, "org.bluez.Adapter", "GetRemoteMajorClass"));
    pa_assert_se(dbus_message_append_args(m, DBUS_TYPE_STRING, &a, DBUS_TYPE_INVALID));
    r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->dbus_connection), m, -1, &e);

    if (!r) {
        pa_log("org.bluez.Adapter.GetRemoteMajorClass(%s) failed: %s", a, e.message);
        goto fail;
    }

    if (!(dbus_message_get_args(r, &e, DBUS_TYPE_STRING, &class, DBUS_TYPE_INVALID))) {
        pa_log("Malformed org.bluez.Adapter.GetRemoteMajorClass signal: %s", e.message);
        goto fail;
    }

    if (strcmp(class, "phone")) {
        pa_log_info("Found device '%s' of class '%s', ignoring.", a, class);
        goto fail;
    }

    b = pa_xnew(struct bonding, 1);
    b->userdata = u;
    pa_strlcpy(b->address, a, sizeof(b->address));
    b->pid = (pid_t) -1;
    b->fd = -1;
    b->io_event = NULL;
    b->state = UNKNOWN;
    u->n_unknown ++;

    pa_log_info("Watching device '%s' of class '%s'.", b->address, class);

    if ((b->fd = pa_start_child_for_read(PA_BT_PROXIMITY_HELPER, a, &b->pid)) < 0) {
        pa_log("Failed to start helper tool.");
        goto fail;
    }

    b->io_event = u->module->core->mainloop->io_new(
            u->module->core->mainloop,
            b->fd,
            PA_IO_EVENT_INPUT,
            io_event_cb,
            b);

    dbus_message_unref(m);
    dbus_message_unref(r);

    pa_hashmap_put(u->bondings, b->address, b);

    return b;

fail:
    if (m)
        dbus_message_unref(m);
    if (r)
        dbus_message_unref(r);

    if (b)
        bonding_free(b);

    dbus_error_free(&e);
    return NULL;
}

static void bonding_remove(struct userdata *u, const char *a) {
    struct bonding *b;
    pa_assert(u);

    pa_return_if_fail((b = pa_hashmap_remove(u->bondings, a)));

    pa_log_info("No longer watching device '%s'", b->address);
    bonding_free(b);
}

static DBusHandlerResult filter_func(DBusConnection *connection, DBusMessage *m, void *userdata) {
    struct userdata *u = userdata;
    DBusError e;

    dbus_error_init(&e);

    if (dbus_message_is_signal(m, "org.bluez.Adapter", "BondingCreated")) {
        const char *a;

        if (!(dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &a, DBUS_TYPE_INVALID))) {
            pa_log("Malformed org.bluez.Adapter.BondingCreated signal: %s", e.message);
            goto finish;
        }

        bonding_new(u, a);

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Adapter", "BondingRemoved")) {

        const char *a;

        if (!(dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &a, DBUS_TYPE_INVALID))) {
            pa_log("Malformed org.bluez.Adapter.BondingRemoved signal: %s", e.message);
            goto finish;
        }

        bonding_remove(u, a);

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

finish:

    dbus_error_free(&e);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int add_matches(struct userdata *u, pa_bool_t add) {
    char *filter1, *filter2;
    DBusError e;
    int r = -1;

    pa_assert(u);
    dbus_error_init(&e);

    filter1 = pa_sprintf_malloc("type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='BondingCreated',path='%s'", u->hci_path);
    filter2 = pa_sprintf_malloc("type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='BondingRemoved',path='%s'", u->hci_path);

    if (add) {
        dbus_bus_add_match(pa_dbus_connection_get(u->dbus_connection), filter1, &e);

        if (dbus_error_is_set(&e)) {
            pa_log("dbus_bus_add_match(%s) failed: %s", filter1, e.message);
            goto finish;
        }
    } else
        dbus_bus_remove_match(pa_dbus_connection_get(u->dbus_connection), filter1, &e);


    if (add) {
        dbus_bus_add_match(pa_dbus_connection_get(u->dbus_connection), filter2, &e);

        if (dbus_error_is_set(&e)) {
            pa_log("dbus_bus_add_match(%s) failed: %s", filter2, e.message);
            dbus_bus_remove_match(pa_dbus_connection_get(u->dbus_connection), filter2, &e);
            goto finish;
        }
    } else
        dbus_bus_remove_match(pa_dbus_connection_get(u->dbus_connection), filter2, &e);

    if (add)
        pa_assert_se(dbus_connection_add_filter(pa_dbus_connection_get(u->dbus_connection), filter_func, u, NULL));
    else
        dbus_connection_remove_filter(pa_dbus_connection_get(u->dbus_connection), filter_func, u);

    r = 0;

finish:
    pa_xfree(filter1);
    pa_xfree(filter2);
    dbus_error_free(&e);

    return r;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    DBusError e;
    DBusMessage *msg = NULL, *r = NULL;
    DBusMessageIter iter, sub;

    pa_assert(m);
    dbus_error_init(&e);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->module = m;
    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));
    u->hci = pa_xstrdup(pa_modargs_get_value(ma, "hci", DEFAULT_HCI));
    u->hci_path = pa_sprintf_malloc("/org/bluez/%s", u->hci);
    u->n_found = u->n_unknown = 0;
    u->muted = FALSE;

    u->bondings = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    if (!(u->dbus_connection = pa_dbus_bus_get(m->core, DBUS_BUS_SYSTEM, &e))) {
        pa_log("Failed to get D-Bus connection: %s", e.message);
        goto fail;
    }

    if (add_matches(u, TRUE) < 0)
        goto fail;

    pa_assert_se(msg = dbus_message_new_method_call("org.bluez", u->hci_path, "org.bluez.Adapter", "ListBondings"));

    if (!(r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->dbus_connection), msg, -1, &e))) {
        pa_log("org.bluez.Adapter.ListBondings failed: %s", e.message);
        goto fail;
    }

    dbus_message_iter_init(r, &iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        pa_log("Malformed reply to org.bluez.Adapter.ListBondings.");
        goto fail;
    }

    dbus_message_iter_recurse(&iter, &sub);

    while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
        const char *a = NULL;

        dbus_message_iter_get_basic(&sub, &a);
        bonding_new(u, a);

        dbus_message_iter_next(&sub);
    }

    dbus_message_unref(r);
    dbus_message_unref(msg);

    pa_modargs_free(ma);

    if (pa_hashmap_size(u->bondings) == 0)
        pa_log_warn("Warning: no phone device bonded.");

    update_volume(u);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    dbus_error_free(&e);

    if (msg)
        dbus_message_unref(msg);

    if (r)
        dbus_message_unref(r);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->bondings) {
        struct bonding *b;

        while ((b = pa_hashmap_steal_first(u->bondings)))
            bonding_free(b);

        pa_hashmap_free(u->bondings, NULL, NULL);
    }

    if (u->dbus_connection) {
        add_matches(u, FALSE);
        pa_dbus_connection_unref(u->dbus_connection);
    }

    pa_xfree(u->sink_name);
    pa_xfree(u->hci_path);
    pa_xfree(u->hci);
    pa_xfree(u);
}
