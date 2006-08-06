/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/core-util.h>

#include <hal/libhal.h>

#include "dbus-util.h"
#include "module-hal-detect-symdef.h"

PA_MODULE_AUTHOR("Shahms King")
PA_MODULE_DESCRIPTION("Detect available audio hardware and load matching drivers")
PA_MODULE_VERSION(PACKAGE_VERSION)

static const char*const capabilities[] = { "alsa", "oss" };

typedef enum {
    CAP_ALSA,
    CAP_OSS,
    CAP_MAX
} capability_t;

typedef enum {
    ALSA_TYPE_SINK,
    ALSA_TYPE_SOURCE,
    ALSA_TYPE_OTHER,
    ALSA_TYPE_MAX
} alsa_type_t;

struct device {
    char *udi;
    pa_module *module;
};

struct userdata {
    pa_core *core;
    pa_subscription *sub;
    LibHalContext *ctx;
    capability_t capability;
    pa_dbus_connection *conn;
    pa_hashmap *by_udi;
    pa_hashmap *by_module;
};

struct timerdata {
    struct userdata *u;
    char *udi;
};

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

static void hal_device_free(struct device* d) {
    pa_xfree(d->udi);
    pa_xfree(d);
}

static void hal_device_free_cb(void *d, PA_GCC_UNUSED void *data) {
    hal_device_free((struct device*) d);
}

static dbus_bool_t hal_device_add_alsa(struct userdata *u, const char *udi,
                                       DBusError  *error)
{
    char args[64];
    alsa_type_t type;
    int device, card;
    pa_module *m;
    struct device *d;
    const char *module_name;

    type = hal_device_get_alsa_type(u->ctx, udi, error);
    if (dbus_error_is_set(error) || type == ALSA_TYPE_OTHER) {
        return FALSE;
    }

    device = hal_device_get_alsa_device(u->ctx, udi, error);
    if (dbus_error_is_set(error) || device != 0)
        return FALSE;

    card = hal_device_get_alsa_card(u->ctx, udi, error);
    if (dbus_error_is_set(error))
        return FALSE;

    module_name = (type == ALSA_TYPE_SINK) ? "module-alsa-sink"
                                           : "module-alsa-source";
    snprintf(args, sizeof(args), "device=hw:%u", card);
    if (!(m = pa_module_load(u->core, module_name, args)))
        return FALSE;

    d = pa_xmalloc(sizeof(struct device));
    d->udi = pa_xstrdup(udi);
    d->module = m;

    pa_hashmap_put(u->by_module, m, d);
    pa_hashmap_put(u->by_udi, udi, d);

    return TRUE;
}

static int hal_device_add_all(struct userdata *u, capability_t capability)
{
    DBusError error;
    int i,n,count;
    dbus_bool_t r;
    char** udis;
    const char* cap = capabilities[capability];

    assert(capability < CAP_MAX);

    pa_log_info(__FILE__": Trying capability %u (%s)", capability, cap);
    dbus_error_init(&error);
    udis = libhal_find_device_by_capability(u->ctx, cap, &n, &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error(__FILE__": Error finding devices: %s: %s", error.name,
                     error.message);
        dbus_error_free(&error);
        return -1;
    }
    count = 0;
    for (i = 0; i < n; ++i) {
        switch(capability) {
            case CAP_ALSA:
                r = hal_device_add_alsa(u, udis[i], &error);
                break;
            case CAP_OSS:
                /* r = hal_device_add_oss(u, udis[i], &error)
                 * break; */
            case CAP_MAX:
            default:
                assert(FALSE);
                break;
        }

        if (dbus_error_is_set(&error)) {
            pa_log_error(__FILE__": Error adding device: %s: %s", error.name,
                         error.message);
            dbus_error_free(&error);
            count = -1;
            break;
        }
        if (r)
            ++count;
    }

    libhal_free_string_array(udis);
    u->capability = capability;
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
    if (!libhal_device_exists(td->u->ctx, td->udi, &error))
        goto exit;

    switch(td->u->capability) {
        case CAP_ALSA:
            hal_device_add_alsa(td->u, td->udi, &error);
            break;
        case CAP_OSS:
            /* hal_device_add_oss(td->u, td->udi, &error);
             * break; */
        case CAP_MAX:
        default:
            /* not reached */
            assert(FALSE);
            break;
    }

exit:
    if (dbus_error_is_set(&error)) {
        pa_log_error(__FILE__": Error adding device: %s: %s", error.name,
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
    const char* cap = capabilities[u->capability];

    pa_log_debug(__FILE__": HAL Device added: %s", udi);

    dbus_error_init(&error);
    has_cap = device_has_capability(ctx, udi, cap, &error);
    if (dbus_error_is_set(&error)) {
        pa_log_error(__FILE__": Error getting capability: %s: %s", error.name,
                     error.message);
        dbus_error_free(&error);
        return;
    }

    /* skip it */
    if (!has_cap)
        return;

    /* actually add the device one second later */
    t = pa_xmalloc(sizeof(struct timerdata));
    t->u = u;
    t->udi = pa_xstrdup(udi);

    gettimeofday(&tv, NULL);
    tv.tv_sec += 1;
    u->core->mainloop->time_new(u->core->mainloop, &tv,
                                device_added_time_cb, t);
}

static void device_removed_cb(LibHalContext* ctx, const char *udi)
{
    struct device *d;
    struct userdata *u = (struct userdata*) libhal_ctx_get_user_data(ctx);

    pa_log_debug(__FILE__": Device removed: %s", udi);
    if ((d = pa_hashmap_remove(u->by_udi, udi))) {
        d = pa_hashmap_remove(u->by_module, d->module);
        pa_log_debug(__FILE__": Unloading: %s <%s>", d->module->name, d->module->argument);
        pa_module_unload_request(d->module);
        hal_device_free(d);
    }
}

#if 0
static void new_capability_cb(LibHalContext *ctx, const char *udi,
                                      const char* capability)
{
}

static void lost_capability_cb(LibHalContext *ctx, const char *udi,
                               const char* capability)
{
}

static void property_modified_cb(LibHalContext *ctx, const char *udi,
                                 const char* key,
                                 dbus_bool_t is_removed,
                                 dbus_bool_t is_added)
{
}
#endif

static void subscribe_notify_cb(pa_core *c, pa_subscription_event_type_t type,
                                uint32_t idx, void *userdata)
{
    pa_module *m;
    struct device *d;
    struct userdata *u = (struct userdata*) userdata;

    /* only listen for module remove events */
    if (type != (PA_SUBSCRIPTION_EVENT_MODULE|PA_SUBSCRIPTION_EVENT_REMOVE))
        return;

    if (!(m = pa_idxset_get_by_index(c->modules, idx)))
        return;

    /* we found the module, see if it's one we care about */
    if ((d = pa_hashmap_remove(u->by_module, m))) {
        pa_log_debug(__FILE__": Removing module #%u %s: %s",
                     m->index, m->name, d->udi);
        d = pa_hashmap_remove(u->by_udi, d->udi);
        hal_device_free(d);
    }
}


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
    pa_subscription_free(u->sub);
    /* free the hashmap */
    pa_hashmap_free(u->by_module, NULL, NULL);
    /* free the devices with the hashmap */
    pa_hashmap_free(u->by_udi, hal_device_free_cb, NULL);
    pa_dbus_connection_unref(u->conn);
    pa_xfree(u);
}

static LibHalContext* pa_hal_context_new(pa_core* c, DBusConnection *conn)
{
    DBusError error;
    LibHalContext *hal_ctx = NULL;

    dbus_error_init(&error);
    if (!(hal_ctx = libhal_ctx_new())) {
        pa_log_error(__FILE__": libhal_ctx_new() failed");
        goto fail;
    }

    if (!libhal_ctx_set_dbus_connection(hal_ctx, conn)) {
        pa_log_error(__FILE__": Error establishing DBUS connection: %s: %s",
                     error.name, error.message);
        goto fail;
    }

    if (!libhal_ctx_init(hal_ctx, &error)) {
        pa_log_error(__FILE__": Couldn't connect to hald: %s: %s",
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
    int n;
    DBusError error;
    pa_dbus_connection *conn;
    struct userdata *u = NULL;
    LibHalContext *hal_ctx = NULL;

    assert(c);
    assert(m);

    dbus_error_init(&error);
    if (!(conn = pa_dbus_bus_get(c, DBUS_BUS_SYSTEM, &error))) {
        pa_log_error(__FILE__": Unable to contact DBUS system bus: %s: %s",
                     error.name, error.message);
        dbus_error_free(&error);
        return -1;
    }

    if (!(hal_ctx = pa_hal_context_new(c, pa_dbus_connection_get(conn)))) {
        /* pa_hal_context_new() logs appropriate errors */
        return -1;
    }

    u = pa_xmalloc(sizeof(struct userdata));
    u->core = c;
    u->ctx = hal_ctx;
    u->conn = conn;
    u->by_module = pa_hashmap_new(NULL, NULL);
    u->by_udi = pa_hashmap_new(pa_idxset_string_hash_func,
                               pa_idxset_string_compare_func);
    u->sub = pa_subscription_new(c, PA_SUBSCRIPTION_MASK_MODULE,
                                 subscribe_notify_cb, (void*) u);
    m->userdata = (void*) u;

#if HAVE_ALSA
    if ((n = hal_device_add_all(u, CAP_ALSA)) <= 0)
#endif
#if HAVE_OSS
    if ((n = hal_device_add_all(u, CAP_OSS)) <= 0)
#endif
    {
        pa_log_warn(__FILE__": failed to detect any sound hardware.");
        userdata_free(u);
        return -1;
    }

    libhal_ctx_set_user_data(hal_ctx, (void*) u);
    libhal_ctx_set_device_added(hal_ctx, device_added_cb);
    libhal_ctx_set_device_removed(hal_ctx, device_removed_cb);

    dbus_error_init(&error);
    if (!libhal_device_property_watch_all(hal_ctx, &error)) {
        pa_log_error(__FILE__": error monitoring device list: %s: %s",
                     error.name, error.message);
        dbus_error_free(&error);
        userdata_free(u);
        return -1;
    }

    /*
    libhal_ctx_set_device_new_capability(hal_ctx, new_capability_cb);
    libhal_ctx_set_device_lost_capability(hal_ctx, lost_capability_cb);
    libhal_ctx_set_device_property_modified(hal_ctx, property_modified_cb);
    */

    pa_log_info(__FILE__": loaded %i modules.", n);

    return 0;
}


void pa__done(PA_GCC_UNUSED pa_core *c, pa_module *m) {
    assert (c && m);

    /* free the user data */
    userdata_free(m->userdata);
}
