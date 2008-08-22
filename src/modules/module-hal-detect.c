/***
    This file is part of PulseAudio.

    Copyright 2006 Lennart Poettering
    Copyright 2006 Shams E. King

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
#include <sys/stat.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/core-util.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/modargs.h>

#include <hal/libhal.h>

#include "dbus-util.h"
#include "module-hal-detect-symdef.h"

PA_MODULE_AUTHOR("Shahms King");
PA_MODULE_DESCRIPTION("Detect available audio hardware and load matching drivers");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
#if defined(HAVE_ALSA) && defined(HAVE_OSS)
PA_MODULE_USAGE("api=<alsa or oss> "
                "tsched=<enable system timer based scheduling mode?>");
#elif defined(HAVE_ALSA)
PA_MODULE_USAGE("api=<alsa> "
                "tsched=<enable system timer based scheduling mode?>");
#elif defined(HAVE_OSS)
PA_MODULE_USAGE("api=<oss>");
#endif

struct device {
    uint32_t index;
    char *udi;
    char *sink_name, *source_name;
    pa_bool_t acl_race_fix;
};

struct userdata {
    pa_core *core;
    LibHalContext *context;
    pa_dbus_connection *connection;
    pa_hashmap *devices;
    const char *capability;
#ifdef HAVE_ALSA
    pa_bool_t use_tsched;
#endif
};

struct timerdata {
    struct userdata *u;
    char *udi;
};

#define CAPABILITY_ALSA "alsa"
#define CAPABILITY_OSS "oss"

static const char* const valid_modargs[] = {
    "api",
#ifdef HAVE_ALSA
    "tsched",
#endif
    NULL
};

static void hal_device_free(struct device* d) {
    pa_assert(d);

    pa_xfree(d->udi);
    pa_xfree(d->sink_name);
    pa_xfree(d->source_name);
    pa_xfree(d);
}

static void hal_device_free_cb(void *d, void *data) {
    hal_device_free(d);
}

static const char *strip_udi(const char *udi) {
    const char *slash;

    if ((slash = strrchr(udi, '/')))
        return slash+1;

    return udi;
}

#ifdef HAVE_ALSA

typedef enum {
    ALSA_TYPE_SINK,
    ALSA_TYPE_SOURCE,
    ALSA_TYPE_OTHER,
    ALSA_TYPE_MAX
} alsa_type_t;

static alsa_type_t hal_alsa_device_get_type(LibHalContext *context, const char *udi, DBusError *error) {
    char *type;
    alsa_type_t t;

    if (!(type = libhal_device_get_property_string(context, udi, "alsa.type", error)))
        return ALSA_TYPE_OTHER;

    if (!strcmp(type, "playback"))
        t = ALSA_TYPE_SINK;
    else if (!strcmp(type, "capture"))
        t = ALSA_TYPE_SOURCE;
    else
        t = ALSA_TYPE_OTHER;

    libhal_free_string(type);

    return t;
}

static int hal_alsa_device_is_modem(LibHalContext *context, const char *udi, DBusError *error) {
    char *class;
    int r;

    if (!(class = libhal_device_get_property_string(context, udi, "alsa.pcm_class", error)))
        return 0;

    r = strcmp(class, "modem") == 0;
    pa_xfree(class);

    return r;
}

static pa_module* hal_device_load_alsa(struct userdata *u, const char *udi, char **sink_name, char **source_name) {
    char *args;
    alsa_type_t type;
    int device, card;
    const char *module_name;
    DBusError error;
    pa_module *m;

    dbus_error_init(&error);

    pa_assert(u);
    pa_assert(sink_name);
    pa_assert(source_name);

    *sink_name = *source_name = NULL;

    type = hal_alsa_device_get_type(u->context, udi, &error);
    if (dbus_error_is_set(&error) || type == ALSA_TYPE_OTHER)
        goto fail;

    device = libhal_device_get_property_int(u->context, udi, "alsa.device", &error);
    if (dbus_error_is_set(&error) || device != 0)
        goto fail;

    card = libhal_device_get_property_int(u->context, udi, "alsa.card", &error);
    if (dbus_error_is_set(&error))
        goto fail;

    if (hal_alsa_device_is_modem(u->context, udi, &error))
        goto fail;

    if (type == ALSA_TYPE_SINK) {
        *sink_name = pa_sprintf_malloc("alsa_output.%s", strip_udi(udi));

        module_name = "module-alsa-sink";
        args = pa_sprintf_malloc("device_id=%u sink_name=%s tsched=%i", card, *sink_name, (int) u->use_tsched);
    } else {
        *source_name = pa_sprintf_malloc("alsa_input.%s", strip_udi(udi));

        module_name = "module-alsa-source";
        args = pa_sprintf_malloc("device_id=%u source_name=%s tsched=%i", card, *source_name, (int) u->use_tsched);
    }

    pa_log_debug("Loading %s with arguments '%s'", module_name, args);

    m = pa_module_load(u->core, module_name, args);

    pa_xfree(args);

    if (!m) {
        pa_xfree(*sink_name);
        pa_xfree(*source_name);
        *sink_name = *source_name = NULL;
    }

    return m;

fail:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing ALSA data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    return NULL;
}

#endif

#ifdef HAVE_OSS

static int hal_oss_device_is_pcm(LibHalContext *context, const char *udi, DBusError *error) {
    char *class = NULL, *dev = NULL, *e;
    int device;
    int r = 0;

    class = libhal_device_get_property_string(context, udi, "oss.type", error);
    if (dbus_error_is_set(error) || !class)
        goto finish;

    if (strcmp(class, "pcm"))
        goto finish;

    dev = libhal_device_get_property_string(context, udi, "oss.device_file", error);
    if (dbus_error_is_set(error) || !dev)
        goto finish;

    if ((e = strrchr(dev, '/')))
        if (pa_startswith(e + 1, "audio"))
            goto finish;

    device = libhal_device_get_property_int(context, udi, "oss.device", error);
    if (dbus_error_is_set(error) || device != 0)
        goto finish;

    r = 1;

finish:

    libhal_free_string(class);
    libhal_free_string(dev);

    return r;
}

static pa_module* hal_device_load_oss(struct userdata *u, const char *udi, char **sink_name, char **source_name) {
    char* args;
    char* device;
    DBusError error;
    pa_module *m;

    dbus_error_init(&error);

    pa_assert(u);
    pa_assert(sink_name);
    pa_assert(source_name);

    *sink_name = *source_name = NULL;

    if (!hal_oss_device_is_pcm(u->context, udi, &error) || dbus_error_is_set(&error))
        goto fail;

    device = libhal_device_get_property_string(u->context, udi, "oss.device_file", &error);
    if (!device || dbus_error_is_set(&error))
        goto fail;

    *sink_name = pa_sprintf_malloc("oss_output.%s", strip_udi(udi));
    *source_name = pa_sprintf_malloc("oss_input.%s", strip_udi(udi));

    args = pa_sprintf_malloc("device=%s sink_name=%s source_name=%s", device, *sink_name, *source_name);
    libhal_free_string(device);

    pa_log_debug("Loading module-oss with arguments '%s'", args);
    m = pa_module_load(u->core, "module-oss", args);
    pa_xfree(args);

    if (!m) {
        pa_xfree(*sink_name);
        pa_xfree(*source_name);
        *sink_name = *source_name = NULL;
    }

    return m;

fail:
    if (dbus_error_is_set(&error)) {
        pa_log_error("D-Bus error while parsing OSS data: %s: %s", error.name, error.message);
        dbus_error_free(&error);
    }

    return NULL;
}
#endif

static struct device* hal_device_add(struct userdata *u, const char *udi) {
    pa_module* m = NULL;
    struct device *d;
    char *sink_name = NULL, *source_name = NULL;

    pa_assert(u);
    pa_assert(u->capability);
    pa_assert(!pa_hashmap_get(u->devices, udi));

#ifdef HAVE_ALSA
    if (strcmp(u->capability, CAPABILITY_ALSA) == 0)
        m = hal_device_load_alsa(u, udi, &sink_name, &source_name);
#endif
#ifdef HAVE_OSS
    if (strcmp(u->capability, CAPABILITY_OSS) == 0)
        m = hal_device_load_oss(u, udi, &sink_name, &source_name);
#endif

    if (!m)
        return NULL;

    d = pa_xnew(struct device, 1);
    d->acl_race_fix = FALSE;
    d->udi = pa_xstrdup(udi);
    d->index = m->index;
    d->sink_name = sink_name;
    d->source_name = source_name;
    pa_hashmap_put(u->devices, d->udi, d);

    return d;
}

static int hal_device_add_all(struct userdata *u, const char *capability) {
    DBusError error;
    int i, n, count = 0;
    char** udis;

    pa_assert(u);

    dbus_error_init(&error);

    if (u->capability && strcmp(u->capability, capability) != 0)
        return 0;

    pa_log_info("Trying capability %s", capability);

    udis = libhal_find_device_by_capability(u->context, capability, &n, &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error("Error finding devices: %s: %s", error.name, error.message);
        dbus_error_free(&error);
        return -1;
    }

    if (n > 0) {
        u->capability = capability;

        for (i = 0; i < n; i++) {
            struct device *d;

            if (!(d = hal_device_add(u, udis[i])))
                pa_log_debug("Not loaded device %s", udis[i]);
            else {
                if (d->sink_name)
                    pa_scache_play_item_by_name(u->core, "pulse-coldplug", d->sink_name, FALSE, PA_VOLUME_NORM, NULL, NULL);
                count++;
            }
        }
    }

    libhal_free_string_array(udis);
    return count;
}

static dbus_bool_t device_has_capability(LibHalContext *context, const char *udi, const char* cap, DBusError *error){
    dbus_bool_t has_prop;

    has_prop = libhal_device_property_exists(context, udi, "info.capabilities", error);
    if (!has_prop || dbus_error_is_set(error))
        return FALSE;

    return libhal_device_query_capability(context, udi, cap, error);
}

static void device_added_time_cb(pa_mainloop_api *ea, pa_time_event *ev, const struct timeval *tv, void *userdata) {
    DBusError error;
    struct timerdata *td = userdata;

    dbus_error_init(&error);

    if (!pa_hashmap_get(td->u->devices, td->udi)) {
        dbus_bool_t b;
        struct device *d;

        b = libhal_device_exists(td->u->context, td->udi, &error);

        if (dbus_error_is_set(&error)) {
            pa_log_error("Error adding device: %s: %s", error.name, error.message);
            dbus_error_free(&error);
        } else if (b) {
            if (!(d = hal_device_add(td->u, td->udi)))
                pa_log_debug("Not loaded device %s", td->udi);
            else {
                if (d->sink_name)
                    pa_scache_play_item_by_name(td->u->core, "pulse-hotplug", d->sink_name, FALSE, PA_VOLUME_NORM, NULL, NULL);
            }
        }
    }

    pa_xfree(td->udi);
    pa_xfree(td);
    ea->time_free(ev);
}

static void device_added_cb(LibHalContext *context, const char *udi) {
    DBusError error;
    struct timeval tv;
    struct timerdata *t;
    struct userdata *u;
    pa_bool_t good = FALSE;

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (pa_hashmap_get(u->devices, udi))
        return;

    pa_log_debug("HAL Device added: %s", udi);

    dbus_error_init(&error);

    if (u->capability) {

        good = device_has_capability(context, udi, u->capability, &error);

        if (dbus_error_is_set(&error)) {
            pa_log_error("Error getting capability: %s: %s", error.name, error.message);
            dbus_error_free(&error);
            return;
        }

    } else {

#ifdef HAVE_ALSA
        good = device_has_capability(context, udi, CAPABILITY_ALSA, &error);

        if (dbus_error_is_set(&error)) {
            pa_log_error("Error getting capability: %s: %s", error.name, error.message);
            dbus_error_free(&error);
            return;
        }

        if (good)
            u->capability = CAPABILITY_ALSA;
#endif
#if defined(HAVE_OSS) && defined(HAVE_ALSA)
        if (!good) {
#endif
#ifdef HAS_OSS
            good = device_has_capability(context, udi, CAPABILITY_OSS, &error);

            if (dbus_error_is_set(&error)) {
                pa_log_error("Error getting capability: %s: %s", error.name, error.message);
                dbus_error_free(&error);
                return;
            }

            if (good)
                u->capability = CAPABILITY_OSS;

#endif
#if defined(HAVE_OSS) && defined(HAVE_ALSA)
        }
#endif
    }

    if (!good)
        return;

    /* actually add the device 1/2 second later */
    t = pa_xnew(struct timerdata, 1);
    t->u = u;
    t->udi = pa_xstrdup(udi);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, 500000);
    u->core->mainloop->time_new(u->core->mainloop, &tv, device_added_time_cb, t);
}

static void device_removed_cb(LibHalContext* context, const char *udi) {
    struct device *d;
    struct userdata *u;

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    pa_log_debug("Device removed: %s", udi);

    if ((d = pa_hashmap_remove(u->devices, udi))) {
        pa_module_unload_by_index(u->core, d->index, TRUE);
        hal_device_free(d);
    }
}

static void new_capability_cb(LibHalContext *context, const char *udi, const char* capability) {
    struct userdata *u;

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (!u->capability || strcmp(u->capability, capability) == 0)
        /* capability we care about, pretend it's a new device */
        device_added_cb(context, udi);
}

static void lost_capability_cb(LibHalContext *context, const char *udi, const char* capability) {
    struct userdata *u;

    pa_assert_se(u = libhal_ctx_get_user_data(context));

    if (u->capability && strcmp(u->capability, capability) == 0)
        /* capability we care about, pretend it was removed */
        device_removed_cb(context, udi);
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *message, void *userdata) {
    struct userdata*u = userdata;
    DBusError error;

    pa_assert(bus);
    pa_assert(message);
    pa_assert(u);

    dbus_error_init(&error);

    pa_log_debug("dbus: interface=%s, path=%s, member=%s\n",
                 dbus_message_get_interface(message),
                 dbus_message_get_path(message),
                 dbus_message_get_member(message));

    if (dbus_message_is_signal(message, "org.freedesktop.Hal.Device.AccessControl", "ACLAdded") ||
        dbus_message_is_signal(message, "org.freedesktop.Hal.Device.AccessControl", "ACLRemoved")) {
        uint32_t uid;
        int suspend = strcmp(dbus_message_get_member(message), "ACLRemoved") == 0;

        if (!dbus_message_get_args(message, &error, DBUS_TYPE_UINT32, &uid, DBUS_TYPE_INVALID) || dbus_error_is_set(&error)) {
            pa_log_error("Failed to parse ACL message: %s: %s", error.name, error.message);
            goto finish;
        }

        if (uid == getuid() || uid == geteuid()) {
            struct device *d;
            const char *udi;

            udi = dbus_message_get_path(message);

            if ((d = pa_hashmap_get(u->devices, udi))) {
                pa_bool_t send_acl_race_fix_message = FALSE;

                d->acl_race_fix = FALSE;

                if (d->sink_name) {
                    pa_sink *sink;

                    if ((sink = pa_namereg_get(u->core, d->sink_name, PA_NAMEREG_SINK, 0))) {
                        int prev_suspended = pa_sink_get_state(sink) == PA_SINK_SUSPENDED;

                        if (prev_suspended && !suspend) {
                            /* resume */
                            if (pa_sink_suspend(sink, 0) >= 0)
                                pa_scache_play_item_by_name(u->core, "pulse-access", d->sink_name, FALSE, PA_VOLUME_NORM, NULL, NULL);
                            else
                                d->acl_race_fix = TRUE;

                        } else if (!prev_suspended && suspend) {
                            /* suspend */
                            if (pa_sink_suspend(sink, 1) >= 0)
                                send_acl_race_fix_message = TRUE;
                        }
                    }
                }

                if (d->source_name) {
                    pa_source *source;

                    if ((source = pa_namereg_get(u->core, d->source_name, PA_NAMEREG_SOURCE, 0))) {
                        int prev_suspended = pa_source_get_state(source) == PA_SOURCE_SUSPENDED;

                        if (prev_suspended && !suspend) {
                            /* resume */
                            if (pa_source_suspend(source, 0) < 0)
                                d->acl_race_fix = TRUE;

                        } else if (!prev_suspended && suspend) {
                            /* suspend */
                            if (pa_source_suspend(source, 0) >= 0)
                                send_acl_race_fix_message = TRUE;
                        }
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

        return DBUS_HANDLER_RESULT_HANDLED;

    } else if (dbus_message_is_signal(message, "org.pulseaudio.Server", "DirtyGiveUpMessage")) {
        /* We use this message to avoid a dirty race condition when we
           get an ACLAdded message before the previously owning PA
           sever has closed the device. We can remove this as
           soon as HAL learns frevoke() */

        const char *udi;
        struct device *d;

        udi = dbus_message_get_path(message);

        if ((d = pa_hashmap_get(u->devices, udi)) && d->acl_race_fix) {
            pa_log_debug("Got dirty give up message for '%s', trying resume ...", udi);

            d->acl_race_fix = FALSE;

            if (d->sink_name) {
                pa_sink *sink;

                if ((sink = pa_namereg_get(u->core, d->sink_name, PA_NAMEREG_SINK, 0))) {

                    int prev_suspended = pa_sink_get_state(sink) == PA_SINK_SUSPENDED;

                    if (prev_suspended) {
                        /* resume */
                        if (pa_sink_suspend(sink, 0) >= 0)
                            pa_scache_play_item_by_name(u->core, "pulse-access", d->sink_name, FALSE, PA_VOLUME_NORM, NULL, NULL);
                    }
                }
            }

            if (d->source_name) {
                pa_source *source;

                if ((source = pa_namereg_get(u->core, d->source_name, PA_NAMEREG_SOURCE, 0))) {

                    int prev_suspended = pa_source_get_state(source) == PA_SOURCE_SUSPENDED;

                    if (prev_suspended)
                        pa_source_suspend(source, 0);
                }
            }

        } else
            /* Yes, we don't check the UDI for validity, but hopefully HAL will */
            device_added_cb(u->context, udi);

        return DBUS_HANDLER_RESULT_HANDLED;
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

static LibHalContext* hal_context_new(pa_core* c, DBusConnection *conn) {
    DBusError error;
    LibHalContext *hal_context = NULL;

    dbus_error_init(&error);

    if (!(hal_context = libhal_ctx_new())) {
        pa_log_error("libhal_ctx_new() failed");
        goto fail;
    }

    if (!libhal_ctx_set_dbus_connection(hal_context, conn)) {
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
    pa_dbus_connection *conn;
    struct userdata *u = NULL;
    LibHalContext *hal_context = NULL;
    int n = 0;
    pa_modargs *ma;
    const char *api;
    pa_bool_t use_tsched = TRUE;

    pa_assert(m);

    dbus_error_init(&error);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "tsched", &use_tsched) < 0) {
        pa_log("Failed to parse tsched argument.");
        goto fail;
    }

    if ((api = pa_modargs_get_value(ma, "api", NULL))) {
        pa_bool_t good = FALSE;

#ifdef HAVE_ALSA
        if (strcmp(api, CAPABILITY_ALSA) == 0) {
            good = TRUE;
            api = CAPABILITY_ALSA;
        }
#endif
#ifdef HAVE_OSS
        if (strcmp(api, CAPABILITY_OSS) == 0) {
            good = TRUE;
            api = CAPABILITY_OSS;
        }
#endif

        if (!good) {
            pa_log_error("Invalid API specification.");
            goto fail;
        }
    }

    if (!(conn = pa_dbus_bus_get(m->core, DBUS_BUS_SYSTEM, &error)) || dbus_error_is_set(&error)) {
        if (conn)
            pa_dbus_connection_unref(conn);
        pa_log_error("Unable to contact DBUS system bus: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!(hal_context = hal_context_new(m->core, pa_dbus_connection_get(conn)))) {
        /* pa_hal_context_new() logs appropriate errors */
        pa_dbus_connection_unref(conn);
        goto fail;
    }

    u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->context = hal_context;
    u->connection = conn;
    u->devices = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    u->capability = api;
#ifdef HAVE_ALSA
    u->use_tsched = use_tsched;
#endif
    m->userdata = u;

#ifdef HAVE_ALSA
    n = hal_device_add_all(u, CAPABILITY_ALSA);
#endif
#if defined(HAVE_ALSA) && defined(HAVE_OSS)
    if (n <= 0)
#endif
#ifdef HAVE_OSS
        n += hal_device_add_all(u, CAPABILITY_OSS);
#endif

    libhal_ctx_set_user_data(hal_context, u);
    libhal_ctx_set_device_added(hal_context, device_added_cb);
    libhal_ctx_set_device_removed(hal_context, device_removed_cb);
    libhal_ctx_set_device_new_capability(hal_context, new_capability_cb);
    libhal_ctx_set_device_lost_capability(hal_context, lost_capability_cb);

    if (!libhal_device_property_watch_all(hal_context, &error)) {
        pa_log_error("Error monitoring device list: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!dbus_connection_add_filter(pa_dbus_connection_get(conn), filter_cb, u, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(conn), "type='signal',sender='org.freedesktop.Hal', interface='org.freedesktop.Hal.Device.AccessControl'", &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error("Unable to subscribe to HAL ACL signals: %s: %s", error.name, error.message);
        goto fail;
    }

    dbus_bus_add_match(pa_dbus_connection_get(conn), "type='signal',interface='org.pulseaudio.Server'", &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error("Unable to subscribe to PulseAudio signals: %s: %s", error.name, error.message);
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

    if (u->devices)
        pa_hashmap_free(u->devices, hal_device_free_cb, NULL);

    if (u->connection) {
        DBusError error;
        dbus_error_init(&error);

        dbus_bus_remove_match(pa_dbus_connection_get(u->connection), "type='signal',sender='org.freedesktop.Hal', interface='org.freedesktop.Hal.Device.AccessControl'", &error);
        dbus_error_free(&error);

        dbus_bus_remove_match(pa_dbus_connection_get(u->connection), "type='signal',interface='org.pulseaudio.Server'", &error);
        dbus_error_free(&error);

        dbus_connection_remove_filter(pa_dbus_connection_get(u->connection), filter_cb, u);

        pa_dbus_connection_unref(u->connection);
    }

    pa_xfree(u);
}
