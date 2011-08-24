/***
    This file is part of PulseAudio.

    Copyright 2006 Lennart Poettering
    Copyright 2006 Shams E. King

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
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>

#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/core-util.h>
#include <pulsecore/namereg.h>
#include <pulsecore/modargs.h>
#include <pulsecore/dbus-shared.h>

#include <hal/libhal.h>

#include "module-hal-detect-symdef.h"

PA_MODULE_AUTHOR("Shahms King");
PA_MODULE_DESCRIPTION("Detect available audio hardware and load matching drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
#if defined(HAVE_ALSA) && defined(HAVE_OSS_OUTPUT)
PA_MODULE_USAGE("api=<alsa or oss> "
                "tsched=<enable system timer based scheduling mode?> "
                "subdevices=<init all subdevices>");
#elif defined(HAVE_ALSA)
PA_MODULE_USAGE("api=<alsa> "
                "tsched=<enable system timer based scheduling mode?>");
#elif defined(HAVE_OSS_OUTPUT)
PA_MODULE_USAGE("api=<oss> "
                "subdevices=<init all subdevices>");
#endif
PA_MODULE_DEPRECATED("Please use module-udev-detect instead of module-hal-detect!");

struct device {
    char *udi, *originating_udi;
    char *card_name, *sink_name, *source_name;
    uint32_t module;
    pa_bool_t acl_race_fix;
};

struct userdata {
    pa_core *core;
    LibHalContext *context;
    pa_dbus_connection *connection;
    pa_hashmap *devices; /* Every entry is indexed twice in this table: by the udi we found the device with and by the originating device's udi */
    const char *capability;
#ifdef HAVE_ALSA
    pa_bool_t use_tsched;
#endif
#ifdef HAVE_OSS_OUTPUT
    pa_bool_t init_subdevs;
#endif
    pa_bool_t filter_added:1;
};

#define CAPABILITY_ALSA "alsa"
#define CAPABILITY_OSS "oss"

static const char* const valid_modargs[] = {
    "api",
#ifdef HAVE_ALSA
    "tsched",
#endif
#ifdef HAVE_OSS_OUTPUT
    "subdevices",
#endif
    NULL
};

static void device_free(struct device* d) {
    pa_assert(d);

    pa_xfree(d->udi);
    pa_xfree(d->originating_udi);
    pa_xfree(d->sink_name);
    pa_xfree(d->source_name);
    pa_xfree(d->card_name);
    pa_xfree(d);
}

static const char *strip_udi(const char *udi) {
    const char *slash;

    pa_assert(udi);

    if ((slash = strrchr(udi, '/')))
        return slash+1;

    return udi;
}

#ifdef HAVE_ALSA

enum alsa_type {
    ALSA_TYPE_PLAYBACK,
    ALSA_TYPE_CAPTURE,
    ALSA_TYPE_CONTROL,
    ALSA_TYPE_OTHER
};

static enum alsa_type hal_alsa_device_get_type(LibHalContext *context, const char *udi) {
    char *type;
    enum alsa_type t = ALSA_TYPE_OTHER;
    DBusError error;

    dbus_error_init(&error);

    pa_assert(context);
    pa_assert(udi);

    if (!(type = libhal_device_get_property_string(context, udi, "alsa.type", &error)))
        goto finish;

    if (pa_streq(type, "playback"))
        t = ALSA_TYPE_PLAYBACK;
    else if (pa_streq(type, "capture"))
        t = ALSA_TYPE_CAPTURE;
    else if (pa_streq(type, "control"))
        t = ALSA_TYPE_CONTROL;

    libhal_free_string(type);

finish:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing HAL ALSA data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    return t;
}

static pa_bool_t hal_alsa_device_is_modem(LibHalContext *context, const char *udi) {
    char *class;
    pa_bool_t r = FALSE;
    DBusError error;

    dbus_error_init(&error);

    pa_assert(context);
    pa_assert(udi);

    if (!(class = libhal_device_get_property_string(context, udi, "alsa.pcm_class", &error)))
        goto finish;

    r = pa_streq(class, "modem");
    libhal_free_string(class);

finish:
    if (dbus_error_is_set(&error)) {
        if (!dbus_error_has_name(&error, "org.freedesktop.Hal.NoSuchProperty"))
            pa_log_error("D-Bus error while parsing HAL ALSA data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    return r;
}

static int hal_device_load_alsa(struct userdata *u, const char *udi, struct device *d) {
    enum alsa_type type;
    int card;
    DBusError error;
    pa_module *m;
    char *args, *originating_udi = NULL, *card_name = NULL;

    dbus_error_init(&error);

    pa_assert(u);
    pa_assert(udi);
    pa_assert(d);

    /* We only care for PCM devices */
    type = hal_alsa_device_get_type(u->context, udi);

    /* For each ALSA card that appears the control device will be the
     * last one to be created, this is considered part of the ALSA
     * userspace API. We rely on this and load our modules only when
     * the control device is available assuming that *all* device
     * nodes have been properly created and assigned the right ACLs at
     * that time. Also see:
     *
     * http://mailman.alsa-project.org/pipermail/alsa-devel/2009-April/015958.html
     *
     * and the associated thread.*/

    if (type != ALSA_TYPE_CONTROL)
        goto fail;

    /* We don't care for modems -- this is most likely not set for
     * control devices, so kind of pointless here. */
    if (hal_alsa_device_is_modem(u->context, udi))
        goto fail;

    /* We store only one entry per card, hence we look for the originating device */
    originating_udi = libhal_device_get_property_string(u->context, udi, "alsa.originating_device", &error);
    if (dbus_error_is_set(&error) || !originating_udi)
        goto fail;

    /* Make sure we only load one module per card */
    if (pa_hashmap_get(u->devices, originating_udi))
        goto fail;

    /* We need the identifier */
    card = libhal_device_get_property_int(u->context, udi, "alsa.card", &error);
    if (dbus_error_is_set(&error))
        goto fail;

    card_name = pa_sprintf_malloc("alsa_card.%s", strip_udi(originating_udi));
    args = pa_sprintf_malloc("device_id=%u name=\"%s\" card_name=\"%s\" tsched=%i card_properties=\"module-hal-detect.discovered=1\"", card, strip_udi(originating_udi), card_name, (int) u->use_tsched);

    pa_log_debug("Loading module-alsa-card with arguments '%s'", args);
    m = pa_module_load(u->core, "module-alsa-card", args);
    pa_xfree(args);

    if (!m)
        goto fail;

    d->originating_udi = originating_udi;
    d->module = m->index;
    d->card_name = card_name;

    return 0;

fail:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing HAL ALSA data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    pa_xfree(originating_udi);
    pa_xfree(card_name);

    return -1;
}

#endif

#ifdef HAVE_OSS_OUTPUT

static pa_bool_t hal_oss_device_is_pcm(LibHalContext *context, const char *udi, pa_bool_t init_subdevices) {
    char *class = NULL, *dev = NULL, *e;
    int device;
    pa_bool_t r = FALSE;
    DBusError error;

    dbus_error_init(&error);

    pa_assert(context);
    pa_assert(udi);

    /* We only care for PCM devices */
    class = libhal_device_get_property_string(context, udi, "oss.type", &error);
    if (dbus_error_is_set(&error) || !class)
        goto finish;

    if (!pa_streq(class, "pcm"))
        goto finish;

    /* We don't like /dev/audio */
    dev = libhal_device_get_property_string(context, udi, "oss.device_file", &error);
    if (dbus_error_is_set(&error) || !dev)
        goto finish;

    if ((e = strrchr(dev, '/')))
        if (pa_startswith(e + 1, "audio"))
            goto finish;

    /* We only care for the main device */
    device = libhal_device_get_property_int(context, udi, "oss.device", &error);
    if (dbus_error_is_set(&error) || (device != 0 && init_subdevices == FALSE))
        goto finish;

    r = TRUE;

finish:

    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing HAL OSS data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    libhal_free_string(class);
    libhal_free_string(dev);

    return r;
}

static int hal_device_load_oss(struct userdata *u, const char *udi, struct device *d) {
    DBusError error;
    pa_module *m;
    char *args, *originating_udi = NULL, *device, *sink_name = NULL, *source_name = NULL;

    dbus_error_init(&error);

    pa_assert(u);
    pa_assert(udi);
    pa_assert(d);

    /* We only care for OSS PCM devices */
    if (!hal_oss_device_is_pcm(u->context, udi, u->init_subdevs))
        goto fail;

    /* We store only one entry per card, hence we look for the originating device */
    originating_udi = libhal_device_get_property_string(u->context, udi, "oss.originating_device", &error);
    if (dbus_error_is_set(&error) || !originating_udi)
        goto fail;

    /* Make sure we only load one module per card */
    if (pa_hashmap_get(u->devices, originating_udi))
        goto fail;

    /* We need the device file */
    device = libhal_device_get_property_string(u->context, udi, "oss.device_file", &error);
    if (!device || dbus_error_is_set(&error))
        goto fail;

    sink_name = pa_sprintf_malloc("oss_output.%s", strip_udi(udi));
    source_name = pa_sprintf_malloc("oss_input.%s", strip_udi(udi));
    args = pa_sprintf_malloc("device=%s sink_name=%s source_name=%s", device, sink_name, source_name);

    libhal_free_string(device);

    pa_log_debug("Loading module-oss with arguments '%s'", args);
    m = pa_module_load(u->core, "module-oss", args);
    pa_xfree(args);

    if (!m)
        goto fail;

    d->originating_udi = originating_udi;
    d->module = m->index;
    d->sink_name = sink_name;
    d->source_name = source_name;

    return 0;

fail:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing OSS HAL data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    pa_xfree(originating_udi);
    pa_xfree(source_name);
    pa_xfree(sink_name);

    return -1;
}
#endif

static struct device* hal_device_add(struct userdata *u, const char *udi) {
    struct device *d;
    int r;

    pa_assert(u);
    pa_assert(u->capability);

    d = pa_xnew(struct device, 1);
    d->acl_race_fix = FALSE;
    d->udi = pa_xstrdup(udi);
    d->originating_udi = NULL;
    d->module = PA_INVALID_INDEX;
    d->sink_name = d->source_name = d->card_name = NULL;
    r = -1;

#ifdef HAVE_ALSA
    if (pa_streq(u->capability, CAPABILITY_ALSA))
        r = hal_device_load_alsa(u, udi,  d);
#endif
#ifdef HAVE_OSS_OUTPUT
    if (pa_streq(u->capability, CAPABILITY_OSS))
        r = hal_device_load_oss(u, udi, d);
#endif

    if (r < 0) {
        device_free(d);
        return NULL;
    }

    pa_hashmap_put(u->devices, d->udi, d);
    pa_hashmap_put(u->devices, d->originating_udi, d);

    return d;
}

static int hal_device_add_all(struct userdata *u) {
    int n, count = 0;
    char** udis;
    DBusError error;

    dbus_error_init(&error);

    pa_assert(u);

    udis = libhal_find_device_by_capability(u->context, u->capability, &n, &error);
    if (dbus_error_is_set(&error) || !udis)
        goto fail;

    if (n > 0) {
        int i;

        for (i = 0; i < n; i++) {
            if (hal_device_add(u, udis[i])) {
                count++;
                pa_log_debug("Loaded device %s", udis[i]);
            } else
                pa_log_debug("Not loaded device %s", udis[i]);
        }
    }

    libhal_free_string_array(udis);

    return count;

fail:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing HAL data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    return -1;
}

static void device_added_cb(LibHalContext *context, const char *udi) {
    DBusError error;
    struct userdata *u;
    pa_bool_t good = FALSE;

    dbus_error_init(&error);

    pa_assert(context);
    pa_assert(udi);

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    good = libhal_device_query_capability(context, udi, u->capability, &error);
    if (dbus_error_is_set(&error) || !good)
        goto finish;

    if (!hal_device_add(u, udi))
        pa_log_debug("Not loaded device %s", udi);
    else
        pa_log_debug("Loaded device %s", udi);

finish:
    if (dbus_error_is_set(&error)) {
        if (!dbus_error_has_name(&error, "org.freedesktop.Hal.NoSuchProperty"))
            pa_log_error("D-Bus error while parsing HAL data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }
}

static void device_removed_cb(LibHalContext* context, const char *udi) {
    struct device *d;
    struct userdata *u;

    pa_assert(context);
    pa_assert(udi);

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (!(d = pa_hashmap_get(u->devices, udi)))
        return;

    pa_hashmap_remove(u->devices, d->originating_udi);
    pa_hashmap_remove(u->devices, d->udi);

    pa_log_debug("Removing HAL device: %s", d->originating_udi);

    pa_module_unload_request_by_index(u->core, d->module, TRUE);
    device_free(d);
}

static void new_capability_cb(LibHalContext *context, const char *udi, const char* capability) {
    struct userdata *u;

    pa_assert(context);
    pa_assert(udi);
    pa_assert(capability);

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (pa_streq(u->capability, capability))
        /* capability we care about, pretend it's a new device */
        device_added_cb(context, udi);
}

static void lost_capability_cb(LibHalContext *context, const char *udi, const char* capability) {
    struct userdata *u;

    pa_assert(context);
    pa_assert(udi);
    pa_assert(capability);

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (pa_streq(u->capability, capability))
        /* capability we care about, pretend it was removed */
        device_removed_cb(context, udi);
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *message, void *userdata) {
    struct userdata*u;
    DBusError error;

    pa_assert(bus);
    pa_assert(message);
    pa_assert_se(u = userdata);

    dbus_error_init(&error);

    pa_log_debug("dbus: interface=%s, path=%s, member=%s\n",
                 dbus_message_get_interface(message),
                 dbus_message_get_path(message),
                 dbus_message_get_member(message));

    if (dbus_message_is_signal(message, "org.freedesktop.Hal.Device.AccessControl", "ACLAdded") ||
        dbus_message_is_signal(message, "org.freedesktop.Hal.Device.AccessControl", "ACLRemoved")) {
        uint32_t uid;
        pa_bool_t suspend = strcmp(dbus_message_get_member(message), "ACLRemoved") == 0;

        if (!dbus_message_get_args(message, &error, DBUS_TYPE_UINT32, &uid, DBUS_TYPE_INVALID) || dbus_error_is_set(&error)) {
            pa_log_error("Failed to parse ACL message: %s: %s", error.name, error.message);
            goto finish;
        }

        /* Check if this is about us? */
        if (uid == getuid() || uid == geteuid()) {
            struct device *d;
            const char *udi;

            udi = dbus_message_get_path(message);

            if ((d = pa_hashmap_get(u->devices, udi))) {
                pa_bool_t send_acl_race_fix_message = FALSE;
                d->acl_race_fix = FALSE;

                if (d->sink_name) {
                    pa_sink *sink;

                    if ((sink = pa_namereg_get(u->core, d->sink_name, PA_NAMEREG_SINK))) {
                        pa_bool_t success = pa_sink_suspend(sink, suspend, PA_SUSPEND_SESSION) >= 0;

                        if (!success && !suspend)
                            d->acl_race_fix = TRUE; /* resume failed, let's try again */
                        else if (suspend)
                            send_acl_race_fix_message = TRUE; /* suspend finished, let's tell everyone to try again */
                    }
                }

                if (d->source_name) {
                    pa_source *source;

                    if ((source = pa_namereg_get(u->core, d->source_name, PA_NAMEREG_SOURCE))) {
                        pa_bool_t success = pa_source_suspend(source, suspend, PA_SUSPEND_SESSION) >= 0;

                        if (!success && !suspend)
                            d->acl_race_fix = TRUE; /* resume failed, let's try again */
                        else if (suspend)
                            send_acl_race_fix_message = TRUE; /* suspend finished, let's tell everyone to try again */
                    }
                }

                if (d->card_name) {
                    pa_card *card;

                    if ((card = pa_namereg_get(u->core, d->card_name, PA_NAMEREG_CARD))) {
                        pa_bool_t success = pa_card_suspend(card, suspend, PA_SUSPEND_SESSION) >= 0;

                        if (!success && !suspend)
                            d->acl_race_fix = TRUE; /* resume failed, let's try again */
                        else if (suspend)
                            send_acl_race_fix_message = TRUE; /* suspend finished, let's tell everyone to try again */
                    }
                }

                if (send_acl_race_fix_message) {
                    DBusMessage *msg;
                    msg = dbus_message_new_signal(udi, "org.pulseaudio.Server", "DirtyGiveUpMessage");
                    dbus_connection_send(pa_dbus_connection_get(u->connection), msg, NULL);
                    dbus_message_unref(msg);
                }

            } else if (!suspend)
                device_added_cb(u->context, udi);

        }

    } else if (dbus_message_is_signal(message, "org.pulseaudio.Server", "DirtyGiveUpMessage")) {
        /* We use this message to avoid a dirty race condition when we
           get an ACLAdded message before the previously owning PA
           sever has closed the device. We can remove this as
           soon as HAL learns frevoke() */

        struct device *d;
        const char *udi;

        udi = dbus_message_get_path(message);

        if ((d = pa_hashmap_get(u->devices, udi))) {

            if (d->acl_race_fix) {
                d->acl_race_fix = FALSE;
                pa_log_debug("Got dirty give up message for '%s', trying resume ...", udi);

                if (d->sink_name) {
                    pa_sink *sink;

                    if ((sink = pa_namereg_get(u->core, d->sink_name, PA_NAMEREG_SINK)))
                        pa_sink_suspend(sink, FALSE, PA_SUSPEND_SESSION);
                }

                if (d->source_name) {
                    pa_source *source;

                    if ((source = pa_namereg_get(u->core, d->source_name, PA_NAMEREG_SOURCE)))
                        pa_source_suspend(source, FALSE, PA_SUSPEND_SESSION);
                }

                if (d->card_name) {
                    pa_card *card;

                    if ((card = pa_namereg_get(u->core, d->source_name, PA_NAMEREG_CARD)))
                        pa_card_suspend(card, FALSE, PA_SUSPEND_SESSION);
                }
            }

        } else
            /* Yes, we don't check the UDI for validity, but hopefully HAL will */
            device_added_cb(u->context, udi);

    }

finish:
    dbus_error_free(&error);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void hal_context_free(LibHalContext* hal_context) {
    DBusError error;

    dbus_error_init(&error);

    libhal_ctx_shutdown(hal_context, &error);
    libhal_ctx_free(hal_context);

    dbus_error_free(&error);
}

static LibHalContext* hal_context_new(DBusConnection *connection) {
    DBusError error;
    LibHalContext *hal_context = NULL;

    dbus_error_init(&error);

    pa_assert(connection);

    if (!(hal_context = libhal_ctx_new())) {
        pa_log_error("libhal_ctx_new() failed");
        goto fail;
    }

    if (!libhal_ctx_set_dbus_connection(hal_context, connection)) {
        pa_log_error("Error establishing DBUS connection: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!libhal_ctx_init(hal_context, &error)) {
        pa_log_error("Couldn't connect to hald: %s: %s", error.name, error.message);
        goto fail;
    }

    return hal_context;

fail:
    if (hal_context)
        hal_context_free(hal_context);

    dbus_error_free(&error);

    return NULL;
}

int pa__init(pa_module*m) {
    DBusError error;
    struct userdata *u = NULL;
    int n = 0;
    pa_modargs *ma;
    const char *api;

    pa_assert(m);

    dbus_error_init(&error);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->devices = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

#ifdef HAVE_ALSA
    u->use_tsched = TRUE;

    if (pa_modargs_get_value_boolean(ma, "tsched", &u->use_tsched) < 0) {
        pa_log("Failed to parse tsched argument.");
        goto fail;
    }

    api = pa_modargs_get_value(ma, "api", "alsa");

    if (pa_streq(api, "alsa"))
        u->capability = CAPABILITY_ALSA;
#else
    api = pa_modargs_get_value(ma, "api", "oss");
#endif

#ifdef HAVE_OSS_OUTPUT
    if (pa_streq(api, "oss"))
        u->capability = CAPABILITY_OSS;
#endif

    if (!u->capability) {
        pa_log_error("Invalid API specification.");
        goto fail;
    }

#ifdef HAVE_OSS_OUTPUT
    if (pa_modargs_get_value_boolean(ma, "subdevices", &u->init_subdevs) < 0) {
        pa_log("Failed to parse subdevices= argument.");
        goto fail;
    }
#endif

    if (!(u->connection = pa_dbus_bus_get(m->core, DBUS_BUS_SYSTEM, &error)) || dbus_error_is_set(&error)) {
        pa_log_error("Unable to contact DBUS system bus: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!(u->context = hal_context_new(pa_dbus_connection_get(u->connection)))) {
        /* pa_hal_context_new() logs appropriate errors */
        goto fail;
    }

    n = hal_device_add_all(u);

    libhal_ctx_set_user_data(u->context, u);
    libhal_ctx_set_device_added(u->context, device_added_cb);
    libhal_ctx_set_device_removed(u->context, device_removed_cb);
    libhal_ctx_set_device_new_capability(u->context, new_capability_cb);
    libhal_ctx_set_device_lost_capability(u->context, lost_capability_cb);

    if (!libhal_device_property_watch_all(u->context, &error)) {
        pa_log_error("Error monitoring device list: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!dbus_connection_add_filter(pa_dbus_connection_get(u->connection), filter_cb, u, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }
    u->filter_added = TRUE;

    if (pa_dbus_add_matches(
                pa_dbus_connection_get(u->connection), &error,
                "type='signal',sender='org.freedesktop.Hal',interface='org.freedesktop.Hal.Device.AccessControl',member='ACLAdded'",
                "type='signal',sender='org.freedesktop.Hal',interface='org.freedesktop.Hal.Device.AccessControl',member='ACLRemoved'",
                "type='signal',interface='org.pulseaudio.Server',member='DirtyGiveUpMessage'", NULL) < 0) {
        pa_log_error("Unable to subscribe to HAL ACL signals: %s: %s", error.name, error.message);
        goto fail;
    }

    pa_log_info("Loaded %i modules.", n);

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

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->context)
        hal_context_free(u->context);

    if (u->devices) {
        struct device *d;

        while ((d = pa_hashmap_first(u->devices))) {
            pa_hashmap_remove(u->devices, d->udi);
            pa_hashmap_remove(u->devices, d->originating_udi);
            device_free(d);
        }

        pa_hashmap_free(u->devices, NULL, NULL);
    }

    if (u->connection) {
        pa_dbus_remove_matches(
                pa_dbus_connection_get(u->connection),
                "type='signal',sender='org.freedesktop.Hal',interface='org.freedesktop.Hal.Device.AccessControl',member='ACLAdded'",
                "type='signal',sender='org.freedesktop.Hal',interface='org.freedesktop.Hal.Device.AccessControl',member='ACLRemoved'",
                "type='signal',interface='org.pulseaudio.Server',member='DirtyGiveUpMessage'", NULL);

        if (u->filter_added)
            dbus_connection_remove_filter(pa_dbus_connection_get(u->connection), filter_cb, u);
        pa_dbus_connection_unref(u->connection);
    }

    pa_xfree(u);
}
