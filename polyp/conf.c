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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

#include "conf.h"
#include "util.h"
#include "xmalloc.h"
#include "strbuf.h"

static const struct pa_conf default_conf = {
    .help = 0,
    .daemonize = 0,
    .dump_conf = 0,
    .dump_modules = 0,
    .fail = 1,
    .verbose = 0,
    .high_priority = 0,
    .stay_root = 0,
    .version = 0,
    .disallow_module_loading = 0,
    .exit_idle_time = -1,
    .module_idle_time = 20,
    .auto_log_target = 1,
    .script_commands = NULL,
    .dl_search_path = NULL,
    .default_script_file = NULL,
    .log_target = PA_LOG_SYSLOG,
};

#define ENV_SCRIPT_FILE "POLYP_SCRIPT"
#define ENV_CONFIG_FILE "POLYP_CONFIG"

#ifndef DEFAULT_SCRIPT_FILE
#define DEFAULT_SCRIPT_FILE "/etc/polypaudio/default.pa"
#endif

#ifndef DEFAULT_CONFIG_FILE
#define DEFAULT_CONFIG_FILE "/etc/polypaudio/config"
#endif

#define DEFAULT_SCRIPT_FILE_LOCAL ".polypaudio.pa"
#define DEFAULT_CONFIG_FILE_LOCAL ".polypaudio.conf"

char* default_file(const char *envvar, const char *global, const char *local) {
    char *p, *h;

    assert(envvar && global && local);

    if ((p = getenv(envvar)))
        return pa_xstrdup(p);

    if ((h = getenv("HOME"))) {
        struct stat st;
        p = pa_sprintf_malloc("%s/%s", h, local);
        if (stat(p, &st) >= 0)
            return p;
        
        pa_xfree(p);
    }

    return pa_xstrdup(global);
}


struct pa_conf* pa_conf_new(void) {
    struct pa_conf *c = pa_xmemdup(&default_conf, sizeof(default_conf));
    c->default_script_file = default_file(ENV_SCRIPT_FILE, DEFAULT_SCRIPT_FILE, DEFAULT_SCRIPT_FILE_LOCAL);
    return c;
}

void pa_conf_free(struct pa_conf *c) {
    assert(c);
    pa_xfree(c->script_commands);
    pa_xfree(c->dl_search_path);
    pa_xfree(c->default_script_file);
    pa_xfree(c);
}

#define WHITESPACE " \t\n"
#define COMMENTS "#;\n"

#define PARSE_BOOLEAN(t, v) \
    do { \
        if (!strcmp(lvalue, t)) { \
            int b; \
            if ((b = pa_parse_boolean(rvalue)) < 0) \
                goto fail; \
            c->v = b; \
            return 0; \
        } \
    } while (0)

#define PARSE_STRING(t, v) \
    do { \
        if (!strcmp(lvalue, t)) { \
            pa_xfree(c->v); \
            c->v = *rvalue ? pa_xstrdup(rvalue) : NULL; \
            return 0; \
        } \
    } while (0)

#define PARSE_INTEGER(t, v) \
   do { \
       if (!strcmp(lvalue, t)) { \
           char *x = NULL; \
           int i = strtol(rvalue, &x, 0); \
           if (!x || *x) \
                 goto fail; \
           c->v = i; \
           return 0; \
       } \
   } while(0)

static int next_assignment(struct pa_conf *c, char *lvalue, char *rvalue, unsigned n) {
    PARSE_BOOLEAN("daemonize", daemonize);
    PARSE_BOOLEAN("fail", fail);
    PARSE_BOOLEAN("verbose", verbose);
    PARSE_BOOLEAN("high-priority", high_priority);
    PARSE_BOOLEAN("stay-root", stay_root);
    PARSE_BOOLEAN("disallow-module-loading", disallow_module_loading);

    PARSE_INTEGER("exit-idle-time", exit_idle_time);
    PARSE_INTEGER("module-idle-time", module_idle_time);
    
    PARSE_STRING("dl-search-path", dl_search_path);
    PARSE_STRING("default-script-file", default_script_file);

    if (!strcmp(lvalue, "log-target")) {
        if (!strcmp(rvalue, "auto"))
            c->auto_log_target = 1;
        else if (!strcmp(rvalue, "syslog")) {
            c->auto_log_target = 0;
            c->log_target = PA_LOG_SYSLOG;
        } else if (!strcmp(rvalue, "stderr")) {
            c->auto_log_target = 0;
            c->log_target = PA_LOG_STDERR;
        } else
            goto fail;

        return 0;
    }
    
fail:
    pa_log(__FILE__": line %u: parse error.\n", n);
    return -1;
}

#undef PARSE_STRING
#undef PARSE_BOOLEAN

static int in_string(char c, const char *s) {
    for (; *s; s++)
        if (*s == c)
            return 1;

    return 0;
}

static char *strip(char *s) {
    char *b = s+strspn(s, WHITESPACE);
    char *e, *l = NULL;

    for (e = b; *e; e++)
        if (!in_string(*e, WHITESPACE))
            l = e;

    if (l)
        *(l+1) = 0;

    return b;
}

static int parse_line(struct pa_conf *conf, char *l, unsigned n) {
    char *e, *c, *b = l+strspn(l, WHITESPACE);

    if ((c = strpbrk(b, COMMENTS)))
        *c = 0;
    
    if (!*b)
        return 0;

    if (!(e = strchr(b, '='))) {
        pa_log(__FILE__": line %u: missing '='.\n", n);
        return -1;
    }

    *e = 0;
    e++;

    return next_assignment(conf, strip(b), strip(e), n);
}


int pa_conf_load(struct pa_conf *c, const char *filename) {
    FILE *f;
    int r = 0;
    unsigned n = 0;
    char *def = NULL;
    assert(c);
    
    if (!filename)
        filename = def = default_file(ENV_CONFIG_FILE, DEFAULT_CONFIG_FILE, DEFAULT_CONFIG_FILE_LOCAL);

    if (!(f = fopen(filename, "r"))) {
        if (errno != ENOENT)
            pa_log(__FILE__": WARNING: failed to open configuration file '%s': %s\n", filename, strerror(errno));

        goto finish;
    }

    while (!feof(f)) {
        char l[256];
        if (!fgets(l, sizeof(l), f)) {
            if (!feof(f))
                pa_log(__FILE__": WARNING: failed to read configuration file '%s': %s\n", filename, strerror(errno));

            break;
        }

        if (parse_line(c, l, ++n) < 0)
            r = -1;
    }
    
finish:

    if (f)
        fclose(f);
    
    pa_xfree(def);
    
    return r;
}

char *pa_conf_dump(struct pa_conf *c) {
    struct pa_strbuf *s = pa_strbuf_new();
    char *d;

    d = default_file(ENV_CONFIG_FILE, DEFAULT_CONFIG_FILE, DEFAULT_CONFIG_FILE_LOCAL);
    pa_strbuf_printf(s, "### Default configuration file: %s ###\n\n", d);
    
    pa_strbuf_printf(s, "verbose = %i\n", !!c->verbose);
    pa_strbuf_printf(s, "daemonize = %i\n", !!c->daemonize);
    pa_strbuf_printf(s, "fail = %i\n", !!c->fail);
    pa_strbuf_printf(s, "high-priority = %i\n", !!c->high_priority);
    pa_strbuf_printf(s, "stay-root = %i\n", !!c->stay_root);
    pa_strbuf_printf(s, "disallow-module-loading = %i\n", !!c->disallow_module_loading);
    pa_strbuf_printf(s, "exit-idle-time = %i\n", c->exit_idle_time);
    pa_strbuf_printf(s, "module-idle-time = %i\n", c->module_idle_time);
    pa_strbuf_printf(s, "dl-search-path = %s\n", c->dl_search_path ? c->dl_search_path : "");
    pa_strbuf_printf(s, "default-script-file = %s\n", c->default_script_file);
    pa_strbuf_printf(s, "log-target = %s\n", c->auto_log_target ? "auto" : (c->log_target == PA_LOG_SYSLOG ? "syslog" : "stderr"));

    pa_strbuf_printf(s, "\n### EOF ###\n");
    
    pa_xfree(d);
    
    return pa_strbuf_tostring_free(s);
}
