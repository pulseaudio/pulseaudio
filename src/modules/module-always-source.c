/***
    This file is part of PulseAudio.

    Copyright 2008 Colin Guthrie
    Copyright 2017 Sebastian Dröge <sebastian@centricular.com>

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

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>

PA_MODULE_AUTHOR("Sebastian Dröge");
PA_MODULE_DESCRIPTION(_("Always keeps at least one source loaded even if it's a null one"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "source_name=<name of source>");

#define DEFAULT_SOURCE_NAME "auto_null"

static const char* const valid_modargs[] = {
    "source_name",
    NULL,
};

struct userdata {
    uint32_t null_module;
    bool ignore;
    char *source_name;
};

static void load_null_source_if_needed(pa_core *c, pa_source *source, struct userdata* u) {
    pa_source *target;
    uint32_t idx;
    char *t;
    pa_module *m;

    pa_assert(c);
    pa_assert(u);

    if (u->null_module != PA_INVALID_INDEX)
        return; /* We've already got a null-source loaded */

    /* Loop through all sources and check to see if we have *any*
     * sources. Ignore the source passed in (if it's not null), and
     * don't count filter or monitor sources. */
    PA_IDXSET_FOREACH(target, c->sources, idx)
        if (!source || ((target != source) && !pa_source_is_filter(target) && target->monitor_of == NULL))
            break;

    if (target)
        return;

    pa_log_debug("Autoloading null-source as no other sources detected.");

    u->ignore = true;

    t = pa_sprintf_malloc("source_name=%s", u->source_name);
    pa_module_load(&m, c, "module-null-source", t);
    u->null_module = m ? m->index : PA_INVALID_INDEX;
    pa_xfree(t);

    u->ignore = false;

    if (!m)
        pa_log_warn("Unable to load module-null-source");
}

static pa_hook_result_t put_hook_callback(pa_core *c, pa_source *source, void* userdata) {
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(source);
    pa_assert(u);

    /* This is us detecting ourselves on load... just ignore this. */
    if (u->ignore)
        return PA_HOOK_OK;

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    /* Auto-loaded null-source not active, so ignoring newly detected source. */
    if (u->null_module == PA_INVALID_INDEX)
        return PA_HOOK_OK;

    /* This is us detecting ourselves on load in a different way... just ignore this too. */
    if (source->module && source->module->index == u->null_module)
        return PA_HOOK_OK;

    /* We don't count filter or monitor sources since they need a real source */
    if (pa_source_is_filter(source) || source->monitor_of != NULL)
        return PA_HOOK_OK;

    pa_log_info("A new source has been discovered. Unloading null-source.");

    pa_module_unload_request_by_index(c, u->null_module, true);
    u->null_module = PA_INVALID_INDEX;

    return PA_HOOK_OK;
}

static pa_hook_result_t unlink_hook_callback(pa_core *c, pa_source *source, void* userdata) {
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(source);
    pa_assert(u);

    /* First check to see if it's our own null-source that's been removed... */
    if (u->null_module != PA_INVALID_INDEX && source->module && source->module->index == u->null_module) {
        pa_log_debug("Autoloaded null-source removed");
        u->null_module = PA_INVALID_INDEX;
        return PA_HOOK_OK;
    }

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    load_null_source_if_needed(c, source, u);

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        return -1;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->source_name = pa_xstrdup(pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME));
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_LATE, (pa_hook_cb_t) put_hook_callback, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], PA_HOOK_EARLY, (pa_hook_cb_t) unlink_hook_callback, u);
    u->null_module = PA_INVALID_INDEX;
    u->ignore = false;

    pa_modargs_free(ma);

    load_null_source_if_needed(m->core, NULL, u);

    return 0;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->null_module != PA_INVALID_INDEX && m->core->state != PA_CORE_SHUTDOWN)
        pa_module_unload_request_by_index(m->core, u->null_module, true);

    pa_xfree(u->source_name);
    pa_xfree(u);
}
