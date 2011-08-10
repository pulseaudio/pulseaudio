/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/conf-parser.h>
#include <pulsecore/core-util.h>
#include <pulsecore/authkey.h>

#include "client-conf.h"

#define DEFAULT_CLIENT_CONFIG_FILE PA_DEFAULT_CONFIG_DIR PA_PATH_SEP "client.conf"
#define DEFAULT_CLIENT_CONFIG_FILE_USER "client.conf"

#define ENV_CLIENT_CONFIG_FILE "PULSE_CLIENTCONFIG"
#define ENV_DEFAULT_SINK "PULSE_SINK"
#define ENV_DEFAULT_SOURCE "PULSE_SOURCE"
#define ENV_DEFAULT_SERVER "PULSE_SERVER"
#define ENV_DAEMON_BINARY "PULSE_BINARY"
#define ENV_COOKIE_FILE "PULSE_COOKIE"

static const pa_client_conf default_conf = {
    .daemon_binary = NULL,
    .extra_arguments = NULL,
    .default_sink = NULL,
    .default_source = NULL,
    .default_server = NULL,
    .default_dbus_server = NULL,
    .autospawn = TRUE,
    .disable_shm = FALSE,
    .cookie_file = NULL,
    .cookie_valid = FALSE,
    .shm_size = 0,
    .auto_connect_localhost = FALSE,
    .auto_connect_display = FALSE
};

pa_client_conf *pa_client_conf_new(void) {
    pa_client_conf *c = pa_xmemdup(&default_conf, sizeof(default_conf));

    c->daemon_binary = pa_xstrdup(PA_BINARY);
    c->extra_arguments = pa_xstrdup("--log-target=syslog");
    c->cookie_file = pa_xstrdup(PA_NATIVE_COOKIE_FILE);

    return c;
}

void pa_client_conf_free(pa_client_conf *c) {
    pa_assert(c);
    pa_xfree(c->daemon_binary);
    pa_xfree(c->extra_arguments);
    pa_xfree(c->default_sink);
    pa_xfree(c->default_source);
    pa_xfree(c->default_server);
    pa_xfree(c->default_dbus_server);
    pa_xfree(c->cookie_file);
    pa_xfree(c);
}

int pa_client_conf_load(pa_client_conf *c, const char *filename) {
    FILE *f = NULL;
    char *fn = NULL;
    int r = -1;

    /* Prepare the configuration parse table */
    pa_config_item table[] = {
        { "daemon-binary",          pa_config_parse_string,   &c->daemon_binary, NULL },
        { "extra-arguments",        pa_config_parse_string,   &c->extra_arguments, NULL },
        { "default-sink",           pa_config_parse_string,   &c->default_sink, NULL },
        { "default-source",         pa_config_parse_string,   &c->default_source, NULL },
        { "default-server",         pa_config_parse_string,   &c->default_server, NULL },
        { "default-dbus-server",    pa_config_parse_string,   &c->default_dbus_server, NULL },
        { "autospawn",              pa_config_parse_bool,     &c->autospawn, NULL },
        { "cookie-file",            pa_config_parse_string,   &c->cookie_file, NULL },
        { "disable-shm",            pa_config_parse_bool,     &c->disable_shm, NULL },
        { "enable-shm",             pa_config_parse_not_bool, &c->disable_shm, NULL },
        { "shm-size-bytes",         pa_config_parse_size,     &c->shm_size, NULL },
        { "auto-connect-localhost", pa_config_parse_bool,     &c->auto_connect_localhost, NULL },
        { "auto-connect-display",   pa_config_parse_bool,     &c->auto_connect_display, NULL },
        { NULL,                     NULL,                     NULL, NULL },
    };

    if (filename) {

        if (!(f = pa_fopen_cloexec(filename, "r"))) {
            pa_log(_("Failed to open configuration file '%s': %s"), fn, pa_cstrerror(errno));
            goto finish;
        }

        fn = pa_xstrdup(fn);

    } else {

        if (!(f = pa_open_config_file(DEFAULT_CLIENT_CONFIG_FILE, DEFAULT_CLIENT_CONFIG_FILE_USER, ENV_CLIENT_CONFIG_FILE, &fn)))
            if (errno != ENOENT)
                goto finish;
    }

    r = f ? pa_config_parse(fn, f, table, NULL) : 0;

    if (!r)
        r = pa_client_conf_load_cookie(c);

finish:
    pa_xfree(fn);

    if (f)
        fclose(f);

    return r;
}

int pa_client_conf_env(pa_client_conf *c) {
    char *e;

    if ((e = getenv(ENV_DEFAULT_SINK))) {
        pa_xfree(c->default_sink);
        c->default_sink = pa_xstrdup(e);
    }

    if ((e = getenv(ENV_DEFAULT_SOURCE))) {
        pa_xfree(c->default_source);
        c->default_source = pa_xstrdup(e);
    }

    if ((e = getenv(ENV_DEFAULT_SERVER))) {
        pa_xfree(c->default_server);
        c->default_server = pa_xstrdup(e);

        /* We disable autospawning automatically if a specific server was set */
        c->autospawn = FALSE;
    }

    if ((e = getenv(ENV_DAEMON_BINARY))) {
        pa_xfree(c->daemon_binary);
        c->daemon_binary = pa_xstrdup(e);
    }

    if ((e = getenv(ENV_COOKIE_FILE))) {
        pa_xfree(c->cookie_file);
        c->cookie_file = pa_xstrdup(e);

        return pa_client_conf_load_cookie(c);
    }

    return 0;
}

int pa_client_conf_load_cookie(pa_client_conf* c) {
    pa_assert(c);

    if (!c->cookie_file)
        return -1;

    c->cookie_valid = FALSE;

    if (pa_authkey_load_auto(c->cookie_file, c->cookie, sizeof(c->cookie)) < 0)
        return -1;

    c->cookie_valid = TRUE;
    return 0;
}
