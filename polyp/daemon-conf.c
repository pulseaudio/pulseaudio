/* $Id$ */

/***
  This file is part of polypaudio.

  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "daemon-conf.h"
#include "util.h"
#include "xmalloc.h"
#include "strbuf.h"
#include "conf-parser.h"
#include "resampler.h"

#ifndef DEFAULT_CONFIG_DIR
# ifndef OS_IS_WIN32
#  define DEFAULT_CONFIG_DIR "/etc/polypaudio"
# else
#  define DEFAULT_CONFIG_DIR "%POLYP_ROOT%"
# endif
#endif

#ifndef OS_IS_WIN32
# define PATH_SEP "/"
#else
# define PATH_SEP "\\"
#endif

#define DEFAULT_SCRIPT_FILE DEFAULT_CONFIG_DIR PATH_SEP "default.pa"
#define DEFAULT_SCRIPT_FILE_USER ".polypaudio" PATH_SEP "default.pa"
#define DEFAULT_CONFIG_FILE DEFAULT_CONFIG_DIR PATH_SEP "daemon.conf"
#define DEFAULT_CONFIG_FILE_USER ".polypaudio" PATH_SEP "daemon.conf"

#define ENV_SCRIPT_FILE "POLYP_SCRIPT"
#define ENV_CONFIG_FILE "POLYP_CONFIG"
#define ENV_DL_SEARCH_PATH "POLYP_DLPATH"

static const pa_daemon_conf default_conf = {
    .cmd = PA_CMD_DAEMON,
    .daemonize = 0,
    .fail = 1,
    .high_priority = 0,
    .disallow_module_loading = 0,
    .exit_idle_time = -1,
    .module_idle_time = 20,
    .scache_idle_time = 20,
    .auto_log_target = 1,
    .script_commands = NULL,
    .dl_search_path = NULL,
    .default_script_file = NULL,
    .log_target = PA_LOG_SYSLOG,
    .log_level = PA_LOG_NOTICE,
    .resample_method = PA_RESAMPLER_SRC_SINC_FASTEST,
    .config_file = NULL,
    .use_pid_file = 1
};

pa_daemon_conf* pa_daemon_conf_new(void) {
    FILE *f;
    pa_daemon_conf *c = pa_xmemdup(&default_conf, sizeof(default_conf));

    if ((f = pa_open_config_file(DEFAULT_SCRIPT_FILE, DEFAULT_SCRIPT_FILE_USER, ENV_SCRIPT_FILE, &c->default_script_file)))
        fclose(f);

#ifdef DLSEARCHPATH
    c->dl_search_path = pa_xstrdup(DLSEARCHPATH);
#endif
    return c;
}

void pa_daemon_conf_free(pa_daemon_conf *c) {
    assert(c);
    pa_xfree(c->script_commands);
    pa_xfree(c->dl_search_path);
    pa_xfree(c->default_script_file);
    pa_xfree(c->config_file);
    pa_xfree(c);
}

int pa_daemon_conf_set_log_target(pa_daemon_conf *c, const char *string) {
    assert(c && string);

    if (!strcmp(string, "auto"))
        c->auto_log_target = 1;
    else if (!strcmp(string, "syslog")) {
        c->auto_log_target = 0;
        c->log_target = PA_LOG_SYSLOG;
    } else if (!strcmp(string, "stderr")) {
        c->auto_log_target = 0;
        c->log_target = PA_LOG_STDERR;
    } else
        return -1;

    return 0;
}

int pa_daemon_conf_set_log_level(pa_daemon_conf *c, const char *string) {
    uint32_t u;
    assert(c && string);

    if (pa_atou(string, &u) >= 0) {
        if (u >= PA_LOG_LEVEL_MAX)
            return -1;

        c->log_level = (pa_log_level_t) u;
    } else if (pa_startswith(string, "debug"))
        c->log_level = PA_LOG_DEBUG;
    else if (pa_startswith(string, "info"))
        c->log_level = PA_LOG_INFO;
    else if (pa_startswith(string, "notice"))
        c->log_level = PA_LOG_NOTICE;
    else if (pa_startswith(string, "warn"))
        c->log_level = PA_LOG_WARN;
    else if (pa_startswith(string, "err"))
        c->log_level = PA_LOG_ERROR;
    else
        return -1;

    return 0;
}

int pa_daemon_conf_set_resample_method(pa_daemon_conf *c, const char *string) {
    int m;
    assert(c && string);

    if ((m = pa_parse_resample_method(string)) < 0)
        return -1;

    c->resample_method = m;
    return 0;
}

static int parse_log_target(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, PA_GCC_UNUSED void *userdata) {
    pa_daemon_conf *c = data;
    assert(filename && lvalue && rvalue && data);

    if (pa_daemon_conf_set_log_target(c, rvalue) < 0) {
        pa_log(__FILE__": [%s:%u] Invalid log target '%s'.\n", filename, line, rvalue);
        return -1;
    }

    return 0;
}

static int parse_log_level(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, PA_GCC_UNUSED void *userdata) {
    pa_daemon_conf *c = data;
    assert(filename && lvalue && rvalue && data);

    if (pa_daemon_conf_set_log_level(c, rvalue) < 0) {
        pa_log(__FILE__": [%s:%u] Invalid log level '%s'.\n", filename, line, rvalue);
        return -1;
    }

    return 0;
}

static int parse_resample_method(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, PA_GCC_UNUSED void *userdata) {
    pa_daemon_conf *c = data;
    assert(filename && lvalue && rvalue && data);

    if (pa_daemon_conf_set_resample_method(c, rvalue) < 0) {
        pa_log(__FILE__": [%s:%u] Inavalid resample method '%s'.\n", filename, line, rvalue);
        return -1;
    }

    return 0;
}

int pa_daemon_conf_load(pa_daemon_conf *c, const char *filename) {
    int r = -1;
    FILE *f = NULL;
    
    pa_config_item table[] = {
        { "daemonize",               pa_config_parse_bool,    NULL },
        { "fail",                    pa_config_parse_bool,    NULL },
        { "high-priority",           pa_config_parse_bool,    NULL },
        { "disallow-module-loading", pa_config_parse_bool,    NULL },
        { "exit-idle-time",          pa_config_parse_int,     NULL },
        { "module-idle-time",        pa_config_parse_int,     NULL },
        { "scache-idle-time",        pa_config_parse_int,     NULL },
        { "dl-search-path",          pa_config_parse_string,  NULL },
        { "default-script-file",     pa_config_parse_string,  NULL },
        { "log-target",              parse_log_target,        NULL },
        { "log-level",               parse_log_level,         NULL },
        { "verbose",                 parse_log_level,         NULL },
        { "resample-method",         parse_resample_method,   NULL },
        { "use-pid-file",            pa_config_parse_bool,    NULL },
        { NULL,                      NULL,                    NULL },
    };
    
    table[0].data = &c->daemonize;
    table[1].data = &c->fail;
    table[2].data = &c->high_priority;
    table[3].data = &c->disallow_module_loading;
    table[4].data = &c->exit_idle_time;
    table[5].data = &c->module_idle_time;
    table[6].data = &c->scache_idle_time;
    table[7].data = &c->dl_search_path;
    table[8].data = &c->default_script_file;
    table[9].data = c;
    table[10].data = c;
    table[11].data = c;
    table[12].data = c;
    table[13].data = &c->use_pid_file;
    
    pa_xfree(c->config_file);
    c->config_file = NULL;

    f = filename ?
        fopen(c->config_file = pa_xstrdup(filename), "r") :
        pa_open_config_file(DEFAULT_CONFIG_FILE, DEFAULT_CONFIG_FILE_USER, ENV_CONFIG_FILE, &c->config_file);

    if (!f && errno != ENOENT) {
        pa_log(__FILE__": WARNING: failed to open configuration file '%s': %s\n", filename, strerror(errno));
        goto finish;
    }

    r = f ? pa_config_parse(c->config_file, f, table, NULL) : 0;
    
finish:
    if (f)
        fclose(f);
    
    return r;
}

int pa_daemon_conf_env(pa_daemon_conf *c) {
    char *e;

    if ((e = getenv(ENV_DL_SEARCH_PATH))) {
        pa_xfree(c->dl_search_path);
        c->dl_search_path = pa_xstrdup(e);
    }
    if ((e = getenv(ENV_SCRIPT_FILE))) {
        pa_xfree(c->default_script_file);
        c->default_script_file = pa_xstrdup(e);
    }

    return 0;
}

static const char* const log_level_to_string[] = {
    [PA_LOG_DEBUG] = "debug",
    [PA_LOG_INFO] = "info",
    [PA_LOG_NOTICE] = "notice",
    [PA_LOG_WARN] = "warning",
    [PA_LOG_ERROR] = "error"
};

char *pa_daemon_conf_dump(pa_daemon_conf *c) {
    pa_strbuf *s = pa_strbuf_new();

    if (c->config_file)
        pa_strbuf_printf(s, "### Read from configuration file: %s ###\n", c->config_file);

    assert(c->log_level <= PA_LOG_LEVEL_MAX);
    
    pa_strbuf_printf(s, "daemonize = %i\n", !!c->daemonize);
    pa_strbuf_printf(s, "fail = %i\n", !!c->fail);
    pa_strbuf_printf(s, "high-priority = %i\n", !!c->high_priority);
    pa_strbuf_printf(s, "disallow-module-loading = %i\n", !!c->disallow_module_loading);
    pa_strbuf_printf(s, "exit-idle-time = %i\n", c->exit_idle_time);
    pa_strbuf_printf(s, "module-idle-time = %i\n", c->module_idle_time);
    pa_strbuf_printf(s, "scache-idle-time = %i\n", c->scache_idle_time);
    pa_strbuf_printf(s, "dl-search-path = %s\n", c->dl_search_path ? c->dl_search_path : "");
    pa_strbuf_printf(s, "default-script-file = %s\n", c->default_script_file);
    pa_strbuf_printf(s, "log-target = %s\n", c->auto_log_target ? "auto" : (c->log_target == PA_LOG_SYSLOG ? "syslog" : "stderr"));
    pa_strbuf_printf(s, "log-level = %s\n", log_level_to_string[c->log_level]);
    pa_strbuf_printf(s, "resample-method = %s\n", pa_resample_method_to_string(c->resample_method));
    pa_strbuf_printf(s, "use-pid-file = %i\n", c->use_pid_file);
    
    return pa_strbuf_tostring_free(s);
}
