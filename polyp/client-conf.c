/* $Id$ */

/***
  This file is part of polypaudio.

  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#include "client-conf.h"
#include "xmalloc.h"
#include "log.h"
#include "conf-parser.h"
#include "util.h"

#ifndef DEFAULT_CLIENT_CONFIG_FILE
#define DEFAULT_CLIENT_CONFIG_FILE "/etc/polypaudio/client.conf"
#endif

#ifndef DEFAULT_CLIENT_CONFIG_FILE_USER
#define DEFAULT_CLIENT_CONFIG_FILE_USER ".polypaudio/client.conf"
#endif

#define ENV_CLIENT_CONFIG_FILE "POLYP_CLIENTCONFIG"
#define ENV_DEFAULT_SINK "POLYP_SINK"
#define ENV_DEFAULT_SOURCE "POLYP_SOURCE"
#define ENV_DEFAULT_SERVER "POLYP_SERVER"
#define ENV_DAEMON_BINARY "POLYP_BINARY"


static const struct pa_client_conf default_conf = {
    .daemon_binary = NULL,
    .extra_arguments = NULL,
    .default_sink = NULL,
    .default_source = NULL,
    .default_server = NULL,
    .autospawn = 0
};

struct pa_client_conf *pa_client_conf_new(void) {
    struct pa_client_conf *c = pa_xmemdup(&default_conf, sizeof(default_conf));

    c->daemon_binary = pa_xstrdup(POLYPAUDIO_BINARY);
    c->extra_arguments = pa_xstrdup("--daemonize=yes --log-target=syslog");
    
    return c;
}

void pa_client_conf_free(struct pa_client_conf *c) {
    assert(c);
    pa_xfree(c->daemon_binary);
    pa_xfree(c->extra_arguments);
    pa_xfree(c->default_sink);
    pa_xfree(c->default_source);
    pa_xfree(c->default_server);
    pa_xfree(c);
}
int pa_client_conf_load(struct pa_client_conf *c, const char *filename) {
    char *def = NULL;
    int r;

    const struct pa_config_item table[] = {
        { "daemon-binary",          pa_config_parse_string,  &c->daemon_binary },
        { "extra-arguments",        pa_config_parse_string,  &c->extra_arguments },
        { "default-sink",           pa_config_parse_string,  &c->default_sink },
        { "default-source",         pa_config_parse_string,  &c->default_source },
        { "default-server",         pa_config_parse_string,  &c->default_server },
        { "autospawn",              pa_config_parse_bool,    &c->autospawn },
        { NULL,                     NULL,                    NULL },
    };

    if (!filename)
        filename = getenv(ENV_CLIENT_CONFIG_FILE);

    if (!filename) {
        char *h;
        
        if ((h = getenv("HOME"))) {
            def = pa_sprintf_malloc("%s/%s", h, DEFAULT_CLIENT_CONFIG_FILE_USER);
            
            if (!access(def, F_OK)) 
                filename = def;
            else {
                pa_xfree(def);
                def = NULL;
            }
        }
    }

    if (!filename)
        filename = DEFAULT_CLIENT_CONFIG_FILE;
    
    r = pa_config_parse(filename, table, NULL);
    pa_xfree(def);
    return r;
}

int pa_client_conf_env(struct pa_client_conf *c) {
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
    }
    
    if ((e = getenv(ENV_DAEMON_BINARY))) {
        pa_xfree(c->daemon_binary);
        c->daemon_binary = pa_xstrdup(e);
    }

    return 0;
}
