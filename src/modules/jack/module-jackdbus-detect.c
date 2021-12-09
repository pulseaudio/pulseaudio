/***
  This file is part of PulseAudio.

  Written by David Henningsson <david.henningsson@canonical.com>
  Copyright 2010 Canonical Ltd.

  Some code taken from other parts of PulseAudio, these are
  Copyright 2006-2009 Lennart Poettering

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

#include <pulse/proplist.h>
#include <pulse/xmalloc.h>

#include <pulsecore/log.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/strbuf.h>

PA_MODULE_AUTHOR("David Henningsson");
PA_MODULE_DESCRIPTION("Adds JACK sink/source ports when JACK is started");
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
    "channels=<number of channels> "
    "sink_name=<name for the sink> "
    "sink_properties=<properties for the card> "
    "sink_client_name=<jack client name> "
    "sink_channels=<number of channels> "
    "sink_channel_map=<channel map> "
    "source_name=<name for the source> "
    "source_properties=<properties for the source> "
    "source_client_name=<jack client name> "
    "source_channels=<number of channels> "
    "source_channel_map=<channel map> "
    "connect=<connect ports?>");

#define JACK_SERVICE_NAME "org.jackaudio.service"
#define JACK_INTERFACE_NAME "org.jackaudio.JackControl"
#define JACK_INTERFACE_PATH "/org/jackaudio/Controller"

#define SERVICE_FILTER                          \
        "type='signal',"                        \
        "sender='" DBUS_SERVICE_DBUS "',"       \
        "interface='" DBUS_INTERFACE_DBUS "',"  \
        "member='NameOwnerChanged',"            \
        "arg0='" JACK_SERVICE_NAME "'"

#define RUNNING_FILTER(_a)                      \
        "type='signal',"                        \
        "sender='" JACK_SERVICE_NAME "',"       \
        "interface='" JACK_INTERFACE_NAME "',"  \
        "member='" _a "'"

static const char* const valid_modargs[] = {
    "channels",
    "sink_enabled",
    "sink_name",
    "sink_properties",
    "sink_client_name",
    "sink_channels",
    "sink_channel_map",
    "source_enabled",
    "source_name",
    "source_properties",
    "source_client_name",
    "source_channels",
    "source_channel_map",
    "connect",
    NULL
};

#define JACK_SS_SINK 0
#define JACK_SS_SOURCE 1
#define JACK_SS_COUNT 2

static const char* const modnames[JACK_SS_COUNT] = {
    "module-jack-sink",
    "module-jack-source"
};

static const char* const modtypes[JACK_SS_COUNT] = {
    "sink",
    "source"
};

struct moddata {
    bool enabled;
    char *name;
    pa_proplist *proplist;
    char *client_name;
    uint32_t channels;
    pa_channel_map channel_map;
};

struct userdata {
    pa_module *module;
    pa_core *core;
    pa_dbus_connection *connection;
    bool filter_added, match_added;
    bool is_service_started;
    bool autoconnect_ports;
    struct moddata mod_args[JACK_SS_COUNT];
    /* Using index here protects us from module unloading without us knowing */
    int jack_module_index[JACK_SS_COUNT];
};

static void ensure_ports_stopped(struct userdata* u) {
    unsigned i;
    pa_assert(u);

    for (i = 0; i < JACK_SS_COUNT; i++)
        if (u->jack_module_index[i]) {
            pa_module_unload_request_by_index(u->core, u->jack_module_index[i], true);
            u->jack_module_index[i] = 0;
            pa_log_info("Stopped %s.", modnames[i]);
        }
}

static char* proplist_to_arg(pa_proplist *p) {
    const char *key;
    void *state = NULL;
    pa_strbuf *buf;

    pa_assert(p);

    buf = pa_strbuf_new();

    while ((key = pa_proplist_iterate(p, &state))) {
        const char *v;
        char *escaped;

        if (!pa_strbuf_isempty(buf))
            pa_strbuf_puts(buf, " ");

        if ((v = pa_proplist_gets(p, key))) {
            pa_strbuf_printf(buf, "%s=\"", key);

            escaped = pa_escape(v, "\"'");
            pa_strbuf_puts(buf, escaped);
            pa_xfree(escaped);

            pa_strbuf_puts(buf, "\"");
        } else {
            const void *value;
            size_t nbytes;
            char *c;

            pa_assert_se(pa_proplist_get(p, key, &value, &nbytes) == 0);
            c = pa_xmalloc(nbytes*2+1);
            pa_hexstr((const uint8_t*) value, nbytes, c, nbytes*2+1);

            pa_strbuf_printf(buf, "%s=hex:%s", key, c);
            pa_xfree(c);
        }
    }

    return pa_strbuf_to_string_free(buf);
}

static void ensure_ports_started(struct userdata* u) {
    unsigned i;
    char *escaped;
    pa_assert(u);

    for (i = 0; i < JACK_SS_COUNT; i++)
        if (u->mod_args[i].enabled && !u->jack_module_index[i]) {
            pa_strbuf *args_buf = pa_strbuf_new();
            char *args;
            pa_module *m;
            pa_strbuf_printf(args_buf, "connect=%s", pa_yes_no(u->autoconnect_ports));
            if (u->mod_args[i].name) {
                escaped = pa_escape(u->mod_args[i].name, "'");
                pa_strbuf_printf(args_buf, " %s_name='%s'", modtypes[i], escaped);
                pa_xfree(escaped);
            }
            if (!pa_proplist_isempty(u->mod_args[i].proplist)) {
                escaped = proplist_to_arg(u->mod_args[i].proplist);
                pa_strbuf_printf(args_buf, " %s_properties='%s'", modtypes[i], escaped);
                pa_xfree(escaped);
            }
            if (u->mod_args[i].client_name) {
                escaped = pa_escape(u->mod_args[i].client_name, "'");
                pa_strbuf_printf(args_buf, " client_name='%s'", escaped);
                pa_xfree(escaped);
            }
            if (u->mod_args[i].channels > 0)
                pa_strbuf_printf(args_buf, " channels=%" PRIu32, u->mod_args[i].channels);
            if (u->mod_args[i].channel_map.channels > 0) {
                char cm[PA_CHANNEL_MAP_SNPRINT_MAX];
                pa_channel_map_snprint(cm, sizeof(cm), &u->mod_args[i].channel_map);
                pa_strbuf_printf(args_buf, " channel_map='%s'", cm);
            }
            args = pa_strbuf_to_string_free(args_buf);
            pa_module_load(&m, u->core, modnames[i], args);
            pa_xfree(args);

            if (m) {
                pa_log_info("Successfully started %s.", modnames[i]);
                u->jack_module_index[i] = m->index;
            }
            else
                pa_log_info("Failed to start %s.", modnames[i]);
        }
}

static bool check_service_started(struct userdata* u) {
    DBusError error;
    DBusMessage *m = NULL, *reply = NULL;
    bool new_status = false;
    dbus_bool_t call_result;
    pa_assert(u);

    dbus_error_init(&error);

    /* Just a safety check; it isn't such a big deal if the name disappears just after the call. */
    if (!dbus_bus_name_has_owner(pa_dbus_connection_get(u->connection),
            JACK_SERVICE_NAME, &error)) {
        pa_log_debug("jackdbus isn't running.");
        goto finish;
    }

    if (!(m = dbus_message_new_method_call(JACK_SERVICE_NAME, JACK_INTERFACE_PATH, JACK_INTERFACE_NAME, "IsStarted"))) {
        pa_log("Failed to allocate IsStarted() method call.");
        goto finish;
    }

    if (!(reply = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(u->connection), m, -1, &error))) {
        pa_log("IsStarted() call failed: %s: %s", error.name, error.message);
        goto finish;
    }

    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_BOOLEAN, &call_result, DBUS_TYPE_INVALID)) {
        pa_log("IsStarted() call return failed: %s: %s", error.name, error.message);
        goto finish;
    }

    new_status = call_result;

finish:
    if (m)
        dbus_message_unref(m);
    if (reply)
        dbus_message_unref(reply);

    dbus_error_free(&error);
    if (new_status)
        ensure_ports_started(u);
    else
        ensure_ports_stopped(u);
    u->is_service_started = new_status;
    return new_status;
}

static DBusHandlerResult dbus_filter_handler(DBusConnection *c, DBusMessage *s, void *userdata) {
    struct userdata *u = NULL;
    DBusError error;

    pa_assert(userdata);
    u = ((pa_module*) userdata)->userdata;
    pa_assert(u);

    dbus_error_init(&error);

    if (dbus_message_is_signal(s, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        const char *name, *old, *new;
        if (!dbus_message_get_args(s, &error,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old,
                                   DBUS_TYPE_STRING, &new,
                                   DBUS_TYPE_INVALID))
            goto finish;
        if (!pa_streq(name, JACK_SERVICE_NAME))
            goto finish;

        ensure_ports_stopped(u);
        check_service_started(u);
    }

    else if (dbus_message_is_signal(s, JACK_INTERFACE_NAME, "ServerStarted")) {
        ensure_ports_stopped(u);
        check_service_started(u);
    }

    else if (dbus_message_is_signal(s, JACK_INTERFACE_NAME, "ServerStopped")) {
        ensure_ports_stopped(u);
    }

finish:
    dbus_error_free(&error);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int pa__init(pa_module *m) {
    DBusError error;
    pa_dbus_connection *connection = NULL;
    struct userdata *u = NULL;
    pa_modargs *ma;
    uint32_t channels = 0;
    unsigned i;
    char argname[32];
    const char *name;

    pa_assert(m);

    dbus_error_init(&error);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->autoconnect_ports = true;

    if (pa_modargs_get_value_boolean(ma, "connect", &u->autoconnect_ports) < 0) {
        pa_log("Failed to parse connect= argument.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "channels", &channels) < 0 || (channels > 0 && !pa_channels_valid(channels))) {
        pa_log("Failed to parse channels= argument.");
        goto fail;
    }

    for (i = 0; i < JACK_SS_COUNT; i++) {
        u->mod_args[i].enabled = true;
        pa_snprintf(argname, sizeof(argname), "%s_enabled", modtypes[i]);
        if (pa_modargs_get_value_boolean(ma, argname, &u->mod_args[i].enabled) < 0) {
            pa_log("Failed to parse %s= argument.", argname);
            goto fail;
        }

        pa_snprintf(argname, sizeof(argname), "%s_name", modtypes[i]);
        name = pa_modargs_get_value(ma, argname, NULL);
        u->mod_args[i].name = pa_xstrdup(name);

        u->mod_args[i].proplist = pa_proplist_new();
        pa_snprintf(argname, sizeof(argname), "%s_properties", modtypes[i]);
        if (pa_modargs_get_proplist(ma, argname, u->mod_args[i].proplist, PA_UPDATE_REPLACE) < 0) {
            pa_log("Invalid %s properties", modtypes[i]);
            goto fail;
        }

        pa_snprintf(argname, sizeof(argname), "%s_client_name", modtypes[i]);
        name = pa_modargs_get_value(ma, argname, NULL);
        u->mod_args[i].client_name = pa_xstrdup(name);

        u->mod_args[i].channels = channels;
        pa_snprintf(argname, sizeof(argname), "%s_channels", modtypes[i]);
        if (pa_modargs_get_value_u32(ma, argname, &u->mod_args[i].channels) < 0
                || (u->mod_args[i].channels > 0 && !pa_channels_valid(u->mod_args[i].channels))) {
            pa_log("Failed to parse %s= argument.", argname);
            goto fail;
        }

        pa_channel_map_init(&u->mod_args[i].channel_map);
        pa_snprintf(argname, sizeof(argname), "%s_channel_map", modtypes[i]);
        if (pa_modargs_get_value(ma, argname, NULL)) {
            if (pa_modargs_get_channel_map(ma, argname, &u->mod_args[i].channel_map) < 0
                    || (u->mod_args[i].channels > 0 && u->mod_args[i].channel_map.channels != u->mod_args[i].channels)) {
                pa_log("Failed to parse %s= argument.", argname);
                goto fail;
            }
        }
    }

    if (!(connection = pa_dbus_bus_get(m->core, DBUS_BUS_SESSION, &error)) || dbus_error_is_set(&error)) {

        if (connection)
            pa_dbus_connection_unref(connection);

        pa_log_error("Unable to contact D-Bus session bus: %s: %s", error.name, error.message);
        goto fail;
    }
    u->connection = connection;

    if (!dbus_connection_add_filter(pa_dbus_connection_get(connection), dbus_filter_handler, m, NULL)) {
        pa_log_error("Unable to add D-Bus filter");
        goto fail;
    }
    u->filter_added = 1;

    if (pa_dbus_add_matches(
                pa_dbus_connection_get(connection), &error, SERVICE_FILTER,
                RUNNING_FILTER("ServerStarted"), RUNNING_FILTER("ServerStopped"), NULL) < 0) {
        pa_log_error("Unable to subscribe to signals: %s: %s", error.name, error.message);
        goto fail;
    }
    u->match_added = 1;

    check_service_started(u);

    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    dbus_error_free(&error);
    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;
    unsigned i;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    ensure_ports_stopped(u);

    if (u->match_added) {
        pa_dbus_remove_matches(
                pa_dbus_connection_get(u->connection), SERVICE_FILTER,
                RUNNING_FILTER("ServerStarted"), RUNNING_FILTER("ServerStopped"), NULL);
    }

    if (u->filter_added) {
        dbus_connection_remove_filter(pa_dbus_connection_get(u->connection), dbus_filter_handler, m);
    }

    if (u->connection) {
        pa_dbus_connection_unref(u->connection);
    }

    for (i = 0; i < JACK_SS_COUNT; i++) {
        pa_xfree(u->mod_args[i].name);

        if (u->mod_args[i].proplist)
            pa_proplist_free(u->mod_args[i].proplist);

        pa_xfree(u->mod_args[i].client_name);
    }

    pa_xfree(u);
}
