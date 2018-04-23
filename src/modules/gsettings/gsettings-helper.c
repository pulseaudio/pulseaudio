/***
  This file is part of PulseAudio.

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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <gio/gio.h>
#include <glib.h>

#include <pulsecore/core-util.h>

#define PA_GSETTINGS_MODULE_GROUP_SCHEMA "org.freedesktop.pulseaudio.module-group"
#define PA_GSETTINGS_MODULE_GROUPS_SCHEMA "org.freedesktop.pulseaudio.module-groups"
#define PA_GSETTINGS_MODULE_GROUPS_PATH "/org/freedesktop/pulseaudio/module-groups/"

static void handle_module_group(gchar *name) {
    GSettings *settings;
    gchar p[1024];
    gboolean enabled;
    int i;

    pa_snprintf(p, sizeof(p), PA_GSETTINGS_MODULE_GROUPS_PATH"%s/", name);

    if (!(settings = g_settings_new_with_path(PA_GSETTINGS_MODULE_GROUP_SCHEMA,
                                              p)))
        return;

    enabled = g_settings_get_boolean(settings, "enabled");

    printf("%c%s%c", enabled ? '+' : '-', name, 0);

    if (enabled) {
        for (i = 0; i < 10; i++) {
            gchar *n, *a;

            pa_snprintf(p, sizeof(p), "name%d", i);
            n = g_settings_get_string(settings, p);

            pa_snprintf(p, sizeof(p), "args%i", i);
            a = g_settings_get_string(settings, p);

            printf("%s%c%s%c", n, 0, a, 0);

            g_free(n);
            g_free(a);
        }

        printf("%c", 0);
    }

    fflush(stdout);

    g_object_unref(G_OBJECT(settings));
}

static void module_group_callback(GSettings *settings, gchar *key, gpointer user_data) {
    handle_module_group(user_data);
}

int main(int argc, char *argv[]) {
    GMainLoop *g;
    GSettings *settings;
    GPtrArray *groups;
    gchar **group_names, **name;

#if !GLIB_CHECK_VERSION(2,36,0)
    g_type_init();
#endif

    /* gsettings-data-convert copies data from GConf to GSettings. The
     * conversion is defined in the pulseaudio.convert file. The conversion is
     * done only once, so running the command every time gsettings-helper
     * starts is safe. */
    g_spawn_command_line_sync("gsettings-data-convert", NULL, NULL, NULL, NULL);

    if (!(settings = g_settings_new(PA_GSETTINGS_MODULE_GROUPS_SCHEMA)))
        goto fail;

    groups = g_ptr_array_new_full(0, g_object_unref);
    group_names = g_settings_list_children(settings);

    for (name = group_names; *name; name++) {
        GSettings *child = g_settings_get_child(settings, *name);

        /* The child may have been removed between the
         * g_settings_list_children() and g_settings_get_child() calls. */
        if (!child)
            continue;

        g_ptr_array_add(groups, child);
        g_signal_connect(child, "changed", (GCallback) module_group_callback, *name);
        handle_module_group(*name);
    }

    /* Signal the parent that we are now initialized */
    printf("!");
    fflush(stdout);

    g = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g);
    g_main_loop_unref(g);

    g_ptr_array_unref(groups);

    /* group_names can't be freed earlier, because the values are being used as
     * the user_data for module_group_callback(). */
    g_strfreev(group_names);

    g_object_unref(G_OBJECT(settings));

    return 0;

fail:
    return 1;
}
