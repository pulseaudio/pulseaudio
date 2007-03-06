/* $Id$ */

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
#include <assert.h>
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

#include <hal/libhal.h>

#include "dbus-util.h"
#include "module-hal-detect-symdef.h"

PA_MODULE_AUTHOR("Shahms King")
PA_MODULE_DESCRIPTION("Detect available audio hardware and load matching drivers")
PA_MODULE_VERSION(PACKAGE_VERSION)

typedef enum {
#ifdef HAVE_ALSA
    CAP_ALSA,
#endif
#ifdef HAVE_OSS
    CAP_OSS,
#endif
    CAP_MAX
} capability_t;

static const char* const capabilities[CAP_MAX] = {
#ifdef HAVE_ALSA
    [CAP_ALSA] = "alsa",
#endif
#ifdef HAVE_OSS
    [CAP_OSS] = "oss",
#endif
};

struct device {
    uint32_t index;
    char *udi;
};

struct userdata {
    pa_core *core;
    LibHalContext *ctx;
    capability_t capability;
    pa_dbus_connection *conn;
    pa_hashmap *devices;
};

struct timerdata {
    struct userdata *u;
    char *udi;
};

static const char* get_capability_name(capability_t cap) {
    if (cap >= CAP_MAX)
        return NULL;
    return capabilities[cap];
}

static void hal_device_free(struct device* d) {
    pa_xfree(d->udi);
    pa_xfree(d);
}

static void hal_device_free_cb(void *d, PA_GCC_UNUSED void *data) {
    hal_device_free((struct device*) d);
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

static alsa_type_t hal_device_get_alsa_type(LibHalContext *ctx, const char *udi,
                                            DBusError *error)
{
    char *type;
    alsa_type_t t;

    type = libhal_device_get_property_string(ctx, udi, "alsa.type", error);
    if (!type || dbus_error_is_set(error))
        return FALSE;

    if (!strcmp(type, "playback")) {
        t = ALSA_TYPE_SINK;
    } else if (!strcmp(type, "capture")) {
        t = ALSA_TYPE_SOURCE;
    } else {
        t = ALSA_TYPE_OTHER;
    }
    libhal_free_string(type);

    return t;
}

static int hal_device_get_alsa_card(LibHalContext *ctx, const char *udi,
                                    DBusError *error)
{
    return libhal_device_get_property_int(ctx, udi, "alsa.card", error);
}

static int hal_device_get_alsa_device(LibHalContext *ctx, const char *udi,
                                      DBusError *error)
{
    return libhal_device_get_property_int(ctx, udi, "alsa.device", error);
}

static pa_module* hal_device_load_alsa(struct userdata *u, const char *udi,
                                       DBusError  *error)
{
    char args[128];
    alsa_type_t type;
    int device, card;
    const char *module_name;

    type = hal_device_get_alsa_type(u->ctx, udi, error);
    if (dbus_error_is_set(error) || type == ALSA_TYPE_OTHER)
        return NULL;

    device = hal_device_get_alsa_device(u->ctx, udi, error);
    if (dbus_error_is_set(error) || device != 0)
        return NULL;

    card = hal_device_get_alsa_card(u->ctx, udi, error);
    if (dbus_error_is_set(error))
        return NULL;

    if (type == ALSA_TYPE_SINK) {
        module_name = "module-alsa-sink";
        snprintf(args, sizeof(args), "device=hw:%u sink_name=alsa_output.%s", card, strip_udi(udi));
    } else {
        module_name = "module-alsa-source";
        snprintf(args, sizeof(args), "device=hw:%u source_name=alsa_input.%s", card, strip_udi(udi));
    }

    return pa_module_load(u->core, module_name, args);
}

#endif

#ifdef HAVE_OSS
static dbus_bool_t hal_device_is_oss_pcm(LibHalContext *ctx, const char *udi,
                                         DBusError *error)
{
    dbus_bool_t rv = FALSE;
    char* type, *device_file = NULL;
    int device;

    type = libhal_device_get_property_string(ctx, udi, "oss.type", error);
    if (!type || dbus_error_is_set(error))
        return FALSE;

    if (!strcmp(type, "pcm")) {
        char *e;

        device = libhal_device_get_property_int(ctx, udi, "oss.device", error);
        if (dbus_error_is_set(error) || device != 0)
            goto exit;

        device_file = libhal_device_get_property_string(ctx, udi, "oss.device_file",
                                                   error);
        if (!device_file || dbus_error_is_set(error))
            goto exit;

        /* hack to ignore /dev/audio style devices */
        if ((e = strrchr(device_file, '/')))
            rv = !pa_startswith(e + 1, "audio");
    }

exit:
    libhal_free_string(type);
    libhal_free_string(device_file);
    return rv;
}

static pa_module* hal_device_load_oss(struct userdata *u, const char *udi,
                                      DBusError  *error)
{
    char args[256];
    char* device;

    if (!hal_device_is_oss_pcm(u->ctx, udi, error) || dbus_error_is_set(error))
        return NULL;

    device = libhal_device_get_property_string(u->ctx, udi, "oss.device_file",
                                               error);
    if (!device || dbus_error_is_set(error))
        return NULL;

    snprintf(args, sizeof(args), "device=%s sink_name=oss_output.%s source_name=oss_input.%s", device, strip_udi(udi), strip_udi(udi));
    libhal_free_string(device);

    return pa_module_load(u->core, "module-oss", args);
}
#endif

static dbus_bool_t hal_device_add(struct userdata *u, const char *udi,
                                  DBusError *error)
{
    pa_module* m;
    struct device *d;

    switch(u->capability) {
#ifdef HAVE_ALSA
        case CAP_ALSA:
            m = hal_device_load_alsa(u, udi, error);
            break;
#endif
#ifdef HAVE_OSS
        case CAP_OSS:
            m = hal_device_load_oss(u, udi, error);
            break;
#endif
        default:
            assert(FALSE); /* never reached */
            break;
    }

    if (!m || dbus_error_is_set(error))
        return FALSE;

    d = pa_xnew(struct device, 1);
    d->udi = pa_xstrdup(udi);
    d->index = m->index;

    pa_hashmap_put(u->devices, d->udi, d);

    return TRUE;
}

static int hal_device_add_all(struct userdata *u, capability_t capability)
{
    DBusError error;
    int i,n,count;
    dbus_bool_t r;
    char** udis;
    const char* cap = get_capability_name(capability);

    assert(capability < CAP_MAX);

    pa_log_info("Trying capability %u (%s)", capability, cap);
    dbus_error_init(&error);
    udis = libhal_find_device_by_capability(u->ctx, cap, &n, &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error("Error finding devices: %s: %s", error.name,
                     error.message);
        dbus_error_free(&error);
        return -1;
    }
    count = 0;
    u->capability = capability;
    for (i = 0; i < n; ++i) {
        r = hal_device_add(u, udis[i], &error);
        if (dbus_error_is_set(&error)) {
            pa_log_error("Error adding device: %s: %s", error.name,
                         error.message);
            dbus_error_free(&error);
            count = -1;
            break;
        }
        if (r)
            ++count;
    }

    libhal_free_string_array(udis);
    return count;
}

static dbus_bool_t device_has_capability(LibHalContext *ctx, const char *udi,
                                         const char* cap, DBusError *error)
{
    dbus_bool_t has_prop;
    has_prop = libhal_device_property_exists(ctx, udi, "info.capabilities",
                                             error);
    if (!has_prop || dbus_error_is_set(error))
        return FALSE;

    return libhal_device_query_capability(ctx, udi, cap, error);
}

static void device_added_time_cb(pa_mainloop_api *ea, pa_time_event *ev,
                                 const struct timeval *tv, void *userdata)
{
    DBusError error;
    struct timerdata *td = (struct timerdata*) userdata;

    dbus_error_init(&error);
    if (libhal_device_exists(td->u->ctx, td->udi, &error))
        hal_device_add(td->u, td->udi, &error);

    if (dbus_error_is_set(&error)) {
        pa_log_error("Error adding device: %s: %s", error.name,
                     error.message);
        dbus_error_free(&error);
    }

    pa_xfree(td->udi);
    pa_xfree(td);
    ea->time_free(ev);
}

static void device_added_cb(LibHalContext *ctx, const char *udi)
{
    DBusError error;
    struct timeval tv;
    dbus_bool_t has_cap;
    struct timerdata *t;
    struct userdata *u = (struct userdata*) libhal_ctx_get_user_data(ctx);
    const char* cap = get_capability_name(u->capability);

    pa_log_debug("HAL Device added: %s", udi);

    dbus_error_init(&error);
    has_cap = device_has_capability(ctx, udi, cap, &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error("Error getting capability: %s: %s", error.name,
                     error.message);
        dbus_error_free(&error);
        return;
    }

    /* skip it */
    if (!has_cap)
        return;

    /* actually add the device 1/2 second later */
    t = pa_xnew(struct timerdata, 1);
    t->u = u;
    t->udi = pa_xstrdup(udi);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, 500000);
    u->core->mainloop->time_new(u->core->mainloop, &tv,
                                device_added_time_cb, t);
}

static void device_removed_cb(LibHalContext* ctx, const char *udi)
{
    struct device *d;
    struct userdata *u = (struct userdata*) libhal_ctx_get_user_data(ctx);

    pa_log_debug("Device removed: %s", udi);
    if ((d = pa_hashmap_remove(u->devices, udi))) {
        pa_module_unload_by_index(u->core, d->index);
        hal_device_free(d);
    }
}

static void new_capability_cb(LibHalContext *ctx, const char *udi,
                              const char* capability)
{
    struct userdata *u = (struct userdata*) libhal_ctx_get_user_data(ctx);
    const char* capname = get_capability_name(u->capability);

    if (capname && !strcmp(capname, capability)) {
        /* capability we care about, pretend it's a new device */
        device_added_cb(ctx, udi);
    }
}

static void lost_capability_cb(LibHalContext *ctx, const char *udi,
                               const char* capability)
{
    struct userdata *u = (struct userdata*) libhal_ctx_get_user_data(ctx);
    const char* capname = get_capability_name(u->capability);

    if (capname && !strcmp(capname, capability)) {
        /* capability we care about, pretend it was removed */
        device_removed_cb(ctx, udi);
    }
}

#if 0
static void property_modified_cb(LibHalContext *ctx, const char *udi,
                                 const char* key,
                                 dbus_bool_t is_removed,
                                 dbus_bool_t is_added)
{
}
#endif

static void pa_hal_context_free(LibHalContext* hal_ctx)
{
    DBusError error;

    dbus_error_init(&error);
    libhal_ctx_shutdown(hal_ctx, &error);
    libhal_ctx_free(hal_ctx);

    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
    }
}

static void userdata_free(struct userdata *u) {
    pa_hal_context_free(u->ctx);
    /* free the devices with the hashmap */
    pa_hashmap_free(u->devices, hal_device_free_cb, NULL);
    pa_dbus_connection_unref(u->conn);
    pa_xfree(u);
}

static LibHalContext* pa_hal_context_new(pa_core* c, DBusConnection *conn)
{
    DBusError error;
    LibHalContext *hal_ctx = NULL;

    dbus_error_init(&error);
    if (!(hal_ctx = libhal_ctx_new())) {
        pa_log_error("libhal_ctx_new() failed");
        goto fail;
    }

    if (!libhal_ctx_set_dbus_connection(hal_ctx, conn)) {
        pa_log_error("Error establishing DBUS connection: %s: %s",
                     error.name, error.message);
        goto fail;
    }

    if (!libhal_ctx_init(hal_ctx, &error)) {
        pa_log_error("Couldn't connect to hald: %s: %s",
                     error.name, error.message);
        goto fail;
    }

    return hal_ctx;

fail:
    if (hal_ctx)
        pa_hal_context_free(hal_ctx);

    if (dbus_error_is_set(&error))
        dbus_error_free(&error);

    return NULL;
}

int pa__init(pa_core *c, pa_module*m) {
    DBusError error;
    pa_dbus_connection *conn;
    struct userdata *u = NULL;
    LibHalContext *hal_ctx = NULL;

    assert(c);
    assert(m);

    dbus_error_init(&error);
    if (!(conn = pa_dbus_bus_get(c, DBUS_BUS_SYSTEM, &error))) {
        pa_log_error("Unable to contact DBUS system bus: %s: %s",
                     error.name, error.message);
        dbus_error_free(&error);
        return -1;
    }

    if (!(hal_ctx = pa_hal_context_new(c, pa_dbus_connection_get(conn)))) {
        /* pa_hal_context_new() logs appropriate errors */
        return -1;
    }

    u = pa_xnew(struct userdata, 1);
    u->core = c;
    u->ctx = hal_ctx;
    u->conn = conn;
    u->devices = pa_hashmap_new(pa_idxset_string_hash_func,
                                pa_idxset_string_compare_func);
    m->userdata = (void*) u;

#ifdef HAVE_ALSA
    hal_device_add_all(u, CAP_ALSA);
#endif
#ifdef HAVE_OSS
    hal_device_add_all(u, CAP_OSS);
#endif

    libhal_ctx_set_user_data(hal_ctx, (void*) u);
    libhal_ctx_set_device_added(hal_ctx, device_added_cb);
    libhal_ctx_set_device_removed(hal_ctx, device_removed_cb);
    libhal_ctx_set_device_new_capability(hal_ctx, new_capability_cb);
    libhal_ctx_set_device_lost_capability(hal_ctx, lost_capability_cb);
    /*libhal_ctx_set_device_property_modified(hal_ctx, property_modified_cb);*/

    dbus_error_init(&error);
    if (!libhal_device_property_watch_all(hal_ctx, &error)) {
        pa_log_error("error monitoring device list: %s: %s",
                     error.name, error.message);
        dbus_error_free(&error);
        userdata_free(u);
        return -1;
    }

    pa_log_info("loaded %i modules.", n);

    return 0;
}


void pa__done(PA_GCC_UNUSED pa_core *c, pa_module *m) {
    assert (c && m);

    /* free the user data */
    userdata_free(m->userdata);
}
