/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/i18n.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/conf-parser.h>
#include <pulsecore/resampler.h>
#include <pulsecore/macro.h>

#include "daemon-conf.h"

#define DEFAULT_SCRIPT_FILE PA_DEFAULT_CONFIG_DIR PA_PATH_SEP "default.pa"
#define DEFAULT_SCRIPT_FILE_USER PA_PATH_SEP "default.pa"
#define DEFAULT_SYSTEM_SCRIPT_FILE PA_DEFAULT_CONFIG_DIR PA_PATH_SEP "system.pa"

#define DEFAULT_CONFIG_FILE PA_DEFAULT_CONFIG_DIR PA_PATH_SEP "daemon.conf"
#define DEFAULT_CONFIG_FILE_USER PA_PATH_SEP "daemon.conf"

#define ENV_SCRIPT_FILE "PULSE_SCRIPT"
#define ENV_CONFIG_FILE "PULSE_CONFIG"
#define ENV_DL_SEARCH_PATH "PULSE_DLPATH"

static const pa_daemon_conf default_conf = {
    .cmd = PA_CMD_DAEMON,
    .daemonize = FALSE,
    .fail = TRUE,
    .high_priority = TRUE,
    .nice_level = -11,
    .realtime_scheduling = FALSE,
    .realtime_priority = 5,  /* Half of JACK's default rtprio */
    .disallow_module_loading = FALSE,
    .disallow_exit = FALSE,
    .exit_idle_time = 20,
    .module_idle_time = 20,
    .scache_idle_time = 20,
    .auto_log_target = 1,
    .script_commands = NULL,
    .dl_search_path = NULL,
    .load_default_script_file = TRUE,
    .default_script_file = NULL,
    .log_target = PA_LOG_SYSLOG,
    .log_level = PA_LOG_NOTICE,
    .resample_method = PA_RESAMPLER_AUTO,
    .disable_remixing = FALSE,
    .disable_lfe_remixing = TRUE,
    .config_file = NULL,
    .use_pid_file = TRUE,
    .system_instance = FALSE,
    .no_cpu_limit = FALSE,
    .disable_shm = FALSE,
    .default_n_fragments = 4,
    .default_fragment_size_msec = 25,
    .default_sample_spec = { .format = PA_SAMPLE_S16NE, .rate = 44100, .channels = 2 },
    .shm_size = 0
#ifdef HAVE_SYS_RESOURCE_H
   ,.rlimit_fsize = { .value = 0, .is_set = FALSE },
    .rlimit_data = { .value = 0, .is_set = FALSE },
    .rlimit_stack = { .value = 0, .is_set = FALSE },
    .rlimit_core = { .value = 0, .is_set = FALSE },
    .rlimit_rss = { .value = 0, .is_set = FALSE }
#ifdef RLIMIT_NPROC
   ,.rlimit_nproc = { .value = 0, .is_set = FALSE }
#endif
   ,.rlimit_nofile = { .value = 256, .is_set = TRUE }
#ifdef RLIMIT_MEMLOCK
   ,.rlimit_memlock = { .value = 0, .is_set = FALSE }
#endif
   ,.rlimit_as = { .value = 0, .is_set = FALSE }
#ifdef RLIMIT_LOCKS
   ,.rlimit_locks = { .value = 0, .is_set = FALSE }
#endif
#ifdef RLIMIT_SIGPENDING
   ,.rlimit_sigpending = { .value = 0, .is_set = FALSE }
#endif
#ifdef RLIMIT_MSGQUEUE
   ,.rlimit_msgqueue = { .value = 0, .is_set = FALSE }
#endif
#ifdef RLIMIT_NICE
   ,.rlimit_nice = { .value = 31, .is_set = TRUE }     /* nice level of -11 */
#endif
#ifdef RLIMIT_RTPRIO
   ,.rlimit_rtprio = { .value = 9, .is_set = TRUE }    /* One below JACK's default for the server */
#endif
#ifdef RLIMIT_RTTIME
   ,.rlimit_rttime = { .value = PA_USEC_PER_SEC, .is_set = TRUE }
#endif
#endif
};

pa_daemon_conf* pa_daemon_conf_new(void) {
    pa_daemon_conf *c = pa_xnewdup(pa_daemon_conf, &default_conf, 1);

    c->dl_search_path = pa_xstrdup(PA_DLSEARCHPATH);
    return c;
}

void pa_daemon_conf_free(pa_daemon_conf *c) {
    pa_assert(c);
    pa_xfree(c->script_commands);
    pa_xfree(c->dl_search_path);
    pa_xfree(c->default_script_file);
    pa_xfree(c->config_file);
    pa_xfree(c);
}

int pa_daemon_conf_set_log_target(pa_daemon_conf *c, const char *string) {
    pa_assert(c);
    pa_assert(string);

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
    pa_assert(c);
    pa_assert(string);

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
    pa_assert(c);
    pa_assert(string);

    if ((m = pa_parse_resample_method(string)) < 0)
        return -1;

    c->resample_method = m;
    return 0;
}

static int parse_log_target(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_daemon_conf_set_log_target(c, rvalue) < 0) {
        pa_log(_("[%s:%u] Invalid log target '%s'."), filename, line, rvalue);
        return -1;
    }

    return 0;
}

static int parse_log_level(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_daemon_conf_set_log_level(c, rvalue) < 0) {
        pa_log(_("[%s:%u] Invalid log level '%s'."), filename, line, rvalue);
        return -1;
    }

    return 0;
}

static int parse_resample_method(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_daemon_conf_set_resample_method(c, rvalue) < 0) {
        pa_log(_("[%s:%u] Invalid resample method '%s'."), filename, line, rvalue);
        return -1;
    }

    return 0;
}

static int parse_rlimit(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
#ifdef HAVE_SYS_RESOURCE_H
    struct pa_rlimit *r = data;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(r);

    if (rvalue[strspn(rvalue, "\t ")] == 0) {
        /* Empty string */
        r->is_set = 0;
        r->value = 0;
    } else {
        int32_t k;
        if (pa_atoi(rvalue, &k) < 0) {
            pa_log(_("[%s:%u] Invalid rlimit '%s'."), filename, line, rvalue);
            return -1;
        }
        r->is_set = k >= 0;
        r->value = k >= 0 ? (rlim_t) k : 0;
    }
#else
    pa_log_warn(_("[%s:%u] rlimit not supported on this platform."), filename, line);
#endif

    return 0;
}

static int parse_sample_format(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;
    pa_sample_format_t f;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if ((f = pa_parse_sample_format(rvalue)) < 0) {
        pa_log(_("[%s:%u] Invalid sample format '%s'."), filename, line, rvalue);
        return -1;
    }

    c->default_sample_spec.format = f;
    return 0;
}

static int parse_sample_rate(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;
    uint32_t r;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atou(rvalue, &r) < 0 || r > (uint32_t) PA_RATE_MAX || r <= 0) {
        pa_log(_("[%s:%u] Invalid sample rate '%s'."), filename, line, rvalue);
        return -1;
    }

    c->default_sample_spec.rate = r;
    return 0;
}

static int parse_sample_channels(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;
    int32_t n;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atoi(rvalue, &n) < 0 || n > (int32_t) PA_CHANNELS_MAX || n <= 0) {
        pa_log(_("[%s:%u] Invalid sample channels '%s'."), filename, line, rvalue);
        return -1;
    }

    c->default_sample_spec.channels = (uint8_t) n;
    return 0;
}

static int parse_fragments(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;
    int32_t n;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atoi(rvalue, &n) < 0 || n < 2) {
        pa_log(_("[%s:%u] Invalid number of fragments '%s'."), filename, line, rvalue);
        return -1;
    }

    c->default_n_fragments = (unsigned) n;
    return 0;
}

static int parse_fragment_size_msec(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;
    int32_t n;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atoi(rvalue, &n) < 0 || n < 1) {
        pa_log(_("[%s:%u] Invalid fragment size '%s'."), filename, line, rvalue);
        return -1;
    }

    c->default_fragment_size_msec = (unsigned) n;
    return 0;
}

static int parse_nice_level(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;
    int32_t level;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atoi(rvalue, &level) < 0 || level < -20 || level > 19) {
        pa_log(_("[%s:%u] Invalid nice level '%s'."), filename, line, rvalue);
        return -1;
    }

    c->nice_level = (int) level;
    return 0;
}

static int parse_rtprio(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    pa_daemon_conf *c = data;
    int32_t rtprio;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atoi(rvalue, &rtprio) < 0 || rtprio < sched_get_priority_min(SCHED_FIFO) || rtprio > sched_get_priority_max(SCHED_FIFO)) {
        pa_log("[%s:%u] Invalid realtime priority '%s'.", filename, line, rvalue);
        return -1;
    }

    c->realtime_priority = (int) rtprio;
    return 0;
}

int pa_daemon_conf_load(pa_daemon_conf *c, const char *filename) {
    int r = -1;
    FILE *f = NULL;

    pa_config_item table[] = {
        { "daemonize",                  pa_config_parse_bool,     NULL },
        { "fail",                       pa_config_parse_bool,     NULL },
        { "high-priority",              pa_config_parse_bool,     NULL },
        { "realtime-scheduling",        pa_config_parse_bool,     NULL },
        { "disallow-module-loading",    pa_config_parse_bool,     NULL },
        { "disallow-exit",              pa_config_parse_bool,     NULL },
        { "use-pid-file",               pa_config_parse_bool,     NULL },
        { "system-instance",            pa_config_parse_bool,     NULL },
        { "no-cpu-limit",               pa_config_parse_bool,     NULL },
        { "disable-shm",                pa_config_parse_bool,     NULL },
        { "exit-idle-time",             pa_config_parse_int,      NULL },
        { "module-idle-time",           pa_config_parse_int,      NULL },
        { "scache-idle-time",           pa_config_parse_int,      NULL },
        { "realtime-priority",          parse_rtprio,             NULL },
        { "dl-search-path",             pa_config_parse_string,   NULL },
        { "default-script-file",        pa_config_parse_string,   NULL },
        { "log-target",                 parse_log_target,         NULL },
        { "log-level",                  parse_log_level,          NULL },
        { "verbose",                    parse_log_level,          NULL },
        { "resample-method",            parse_resample_method,    NULL },
        { "default-sample-format",      parse_sample_format,      NULL },
        { "default-sample-rate",        parse_sample_rate,        NULL },
        { "default-sample-channels",    parse_sample_channels,    NULL },
        { "default-fragments",          parse_fragments,          NULL },
        { "default-fragment-size-msec", parse_fragment_size_msec, NULL },
        { "nice-level",                 parse_nice_level,         NULL },
        { "disable-remixing",           pa_config_parse_bool,     NULL },
        { "disable-lfe-remixing",       pa_config_parse_bool,     NULL },
        { "load-default-script-file",   pa_config_parse_bool,     NULL },
        { "shm-size-bytes",             pa_config_parse_size,     NULL },
#ifdef HAVE_SYS_RESOURCE_H
        { "rlimit-fsize",               parse_rlimit,             NULL },
        { "rlimit-data",                parse_rlimit,             NULL },
        { "rlimit-stack",               parse_rlimit,             NULL },
        { "rlimit-core",                parse_rlimit,             NULL },
        { "rlimit-rss",                 parse_rlimit,             NULL },
        { "rlimit-nofile",              parse_rlimit,             NULL },
        { "rlimit-as",                  parse_rlimit,             NULL },
#ifdef RLIMIT_NPROC
        { "rlimit-nproc",               parse_rlimit,             NULL },
#endif
#ifdef RLIMIT_MEMLOCK
        { "rlimit-memlock",             parse_rlimit,             NULL },
#endif
#ifdef RLIMIT_LOCKS
        { "rlimit-locks",               parse_rlimit,             NULL },
#endif
#ifdef RLIMIT_SIGPENDING
        { "rlimit-sigpending",          parse_rlimit,             NULL },
#endif
#ifdef RLIMIT_MSGQUEUE
        { "rlimit-msgqueue",            parse_rlimit,             NULL },
#endif
#ifdef RLIMIT_NICE
        { "rlimit-nice",                parse_rlimit,             NULL },
#endif
#ifdef RLIMIT_RTPRIO
        { "rlimit-rtprio",              parse_rlimit,             NULL },
#endif
#ifdef RLIMIT_RTTIME
        { "rlimit-rttime",              parse_rlimit,             NULL },
#endif
#endif
        { NULL,                         NULL,                     NULL },
    };

    table[0].data = &c->daemonize;
    table[1].data = &c->fail;
    table[2].data = &c->high_priority;
    table[3].data = &c->realtime_scheduling;
    table[4].data = &c->disallow_module_loading;
    table[5].data = &c->disallow_exit;
    table[6].data = &c->use_pid_file;
    table[7].data = &c->system_instance;
    table[8].data = &c->no_cpu_limit;
    table[9].data = &c->disable_shm;
    table[10].data = &c->exit_idle_time;
    table[11].data = &c->module_idle_time;
    table[12].data = &c->scache_idle_time;
    table[13].data = c;
    table[14].data = &c->dl_search_path;
    table[15].data = &c->default_script_file;
    table[16].data = c;
    table[17].data = c;
    table[18].data = c;
    table[19].data = c;
    table[20].data = c;
    table[21].data = c;
    table[22].data = c;
    table[23].data = c;
    table[24].data = c;
    table[25].data = c;
    table[26].data = &c->disable_remixing;
    table[27].data = &c->disable_lfe_remixing;
    table[28].data = &c->load_default_script_file;
    table[29].data = &c->shm_size;
#ifdef HAVE_SYS_RESOURCE_H
    table[30].data = &c->rlimit_fsize;
    table[31].data = &c->rlimit_data;
    table[32].data = &c->rlimit_stack;
    table[33].data = &c->rlimit_as;
    table[34].data = &c->rlimit_core;
    table[35].data = &c->rlimit_nofile;
    table[36].data = &c->rlimit_as;
#ifdef RLIMIT_NPROC
    table[37].data = &c->rlimit_nproc;
#endif

#ifdef RLIMIT_MEMLOCK
#ifndef RLIMIT_NPROC
#error "Houston, we have a numbering problem!"
#endif
    table[38].data = &c->rlimit_memlock;
#endif

#ifdef RLIMIT_LOCKS
#ifndef RLIMIT_MEMLOCK
#error "Houston, we have a numbering problem!"
#endif
    table[39].data = &c->rlimit_locks;
#endif

#ifdef RLIMIT_SIGPENDING
#ifndef RLIMIT_LOCKS
#error "Houston, we have a numbering problem!"
#endif
    table[40].data = &c->rlimit_sigpending;
#endif

#ifdef RLIMIT_MSGQUEUE
#ifndef RLIMIT_SIGPENDING
#error "Houston, we have a numbering problem!"
#endif
    table[41].data = &c->rlimit_msgqueue;
#endif

#ifdef RLIMIT_NICE
#ifndef RLIMIT_MSGQUEUE
#error "Houston, we have a numbering problem!"
#endif
    table[42].data = &c->rlimit_nice;
#endif

#ifdef RLIMIT_RTPRIO
#ifndef RLIMIT_NICE
#error "Houston, we have a numbering problem!"
#endif
    table[43].data = &c->rlimit_rtprio;
#endif

#ifdef RLIMIT_RTTIME
#ifndef RLIMIT_RTTIME
#error "Houston, we have a numbering problem!"
#endif
    table[44].data = &c->rlimit_rttime;
#endif
#endif

    pa_xfree(c->config_file);
    c->config_file = NULL;

    f = filename ?
        fopen(c->config_file = pa_xstrdup(filename), "r") :
        pa_open_config_file(DEFAULT_CONFIG_FILE, DEFAULT_CONFIG_FILE_USER, ENV_CONFIG_FILE, &c->config_file);

    if (!f && errno != ENOENT) {
        pa_log_warn(_("Failed to open configuration file: %s"), pa_cstrerror(errno));
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
    pa_assert(c);

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

const char *pa_daemon_conf_get_default_script_file(pa_daemon_conf *c) {
    pa_assert(c);

    if (!c->default_script_file) {
        if (c->system_instance)
            c->default_script_file = pa_find_config_file(DEFAULT_SYSTEM_SCRIPT_FILE, NULL, ENV_SCRIPT_FILE);
        else
            c->default_script_file = pa_find_config_file(DEFAULT_SCRIPT_FILE, DEFAULT_SCRIPT_FILE_USER, ENV_SCRIPT_FILE);
    }

    return c->default_script_file;
}

FILE *pa_daemon_conf_open_default_script_file(pa_daemon_conf *c) {
    FILE *f;
    pa_assert(c);

    if (!c->default_script_file) {
        if (c->system_instance)
            f = pa_open_config_file(DEFAULT_SYSTEM_SCRIPT_FILE, NULL, ENV_SCRIPT_FILE, &c->default_script_file);
        else
            f = pa_open_config_file(DEFAULT_SCRIPT_FILE, DEFAULT_SCRIPT_FILE_USER, ENV_SCRIPT_FILE, &c->default_script_file);
    } else
        f = fopen(c->default_script_file, "r");

    return f;
}


static const char* const log_level_to_string[] = {
    [PA_LOG_DEBUG] = "debug",
    [PA_LOG_INFO] = "info",
    [PA_LOG_NOTICE] = "notice",
    [PA_LOG_WARN] = "warning",
    [PA_LOG_ERROR] = "error"
};

char *pa_daemon_conf_dump(pa_daemon_conf *c) {
    pa_strbuf *s;

    pa_assert(c);

    s = pa_strbuf_new();

    if (c->config_file)
        pa_strbuf_printf(s, _("### Read from configuration file: %s ###\n"), c->config_file);

    pa_assert(c->log_level <= PA_LOG_LEVEL_MAX);

    pa_strbuf_printf(s, "daemonize = %s\n", pa_yes_no(c->daemonize));
    pa_strbuf_printf(s, "fail = %s\n", pa_yes_no(c->fail));
    pa_strbuf_printf(s, "high-priority = %s\n", pa_yes_no(c->high_priority));
    pa_strbuf_printf(s, "nice-level = %i\n", c->nice_level);
    pa_strbuf_printf(s, "realtime-scheduling = %s\n", pa_yes_no(c->realtime_scheduling));
    pa_strbuf_printf(s, "realtime-priority = %i\n", c->realtime_priority);
    pa_strbuf_printf(s, "disallow-module-loading = %s\n", pa_yes_no(c->disallow_module_loading));
    pa_strbuf_printf(s, "disallow-exit = %s\n", pa_yes_no(c->disallow_exit));
    pa_strbuf_printf(s, "use-pid-file = %s\n", pa_yes_no(c->use_pid_file));
    pa_strbuf_printf(s, "system-instance = %s\n", pa_yes_no(c->system_instance));
    pa_strbuf_printf(s, "no-cpu-limit = %s\n", pa_yes_no(c->no_cpu_limit));
    pa_strbuf_printf(s, "disable-shm = %s\n", pa_yes_no(c->disable_shm));
    pa_strbuf_printf(s, "exit-idle-time = %i\n", c->exit_idle_time);
    pa_strbuf_printf(s, "module-idle-time = %i\n", c->module_idle_time);
    pa_strbuf_printf(s, "scache-idle-time = %i\n", c->scache_idle_time);
    pa_strbuf_printf(s, "dl-search-path = %s\n", pa_strempty(c->dl_search_path));
    pa_strbuf_printf(s, "default-script-file = %s\n", pa_strempty(pa_daemon_conf_get_default_script_file(c)));
    pa_strbuf_printf(s, "load-default-script-file = %s\n", pa_yes_no(c->load_default_script_file));
    pa_strbuf_printf(s, "log-target = %s\n", c->auto_log_target ? "auto" : (c->log_target == PA_LOG_SYSLOG ? "syslog" : "stderr"));
    pa_strbuf_printf(s, "log-level = %s\n", log_level_to_string[c->log_level]);
    pa_strbuf_printf(s, "resample-method = %s\n", pa_resample_method_to_string(c->resample_method));
    pa_strbuf_printf(s, "disable-remixing = %s\n", pa_yes_no(c->disable_remixing));
    pa_strbuf_printf(s, "disable-lfe-remixing = %s\n", pa_yes_no(c->disable_lfe_remixing));
    pa_strbuf_printf(s, "default-sample-format = %s\n", pa_sample_format_to_string(c->default_sample_spec.format));
    pa_strbuf_printf(s, "default-sample-rate = %u\n", c->default_sample_spec.rate);
    pa_strbuf_printf(s, "default-sample-channels = %u\n", c->default_sample_spec.channels);
    pa_strbuf_printf(s, "default-fragments = %u\n", c->default_n_fragments);
    pa_strbuf_printf(s, "default-fragment-size-msec = %u\n", c->default_fragment_size_msec);
    pa_strbuf_printf(s, "shm-size-bytes = %lu\n", (unsigned long) c->shm_size);
#ifdef HAVE_SYS_RESOURCE_H
    pa_strbuf_printf(s, "rlimit-fsize = %li\n", c->rlimit_fsize.is_set ? (long int) c->rlimit_fsize.value : -1);
    pa_strbuf_printf(s, "rlimit-data = %li\n", c->rlimit_data.is_set ? (long int) c->rlimit_data.value : -1);
    pa_strbuf_printf(s, "rlimit-stack = %li\n", c->rlimit_stack.is_set ? (long int) c->rlimit_stack.value : -1);
    pa_strbuf_printf(s, "rlimit-core = %li\n", c->rlimit_core.is_set ? (long int) c->rlimit_core.value : -1);
    pa_strbuf_printf(s, "rlimit-as = %li\n", c->rlimit_as.is_set ? (long int) c->rlimit_as.value : -1);
    pa_strbuf_printf(s, "rlimit-rss = %li\n", c->rlimit_rss.is_set ? (long int) c->rlimit_rss.value : -1);
#ifdef RLIMIT_NPROC
    pa_strbuf_printf(s, "rlimit-nproc = %li\n", c->rlimit_nproc.is_set ? (long int) c->rlimit_nproc.value : -1);
#endif
    pa_strbuf_printf(s, "rlimit-nofile = %li\n", c->rlimit_nofile.is_set ? (long int) c->rlimit_nofile.value : -1);
#ifdef RLIMIT_MEMLOCK
    pa_strbuf_printf(s, "rlimit-memlock = %li\n", c->rlimit_memlock.is_set ? (long int) c->rlimit_memlock.value : -1);
#endif
#ifdef RLIMIT_LOCKS
    pa_strbuf_printf(s, "rlimit-locks = %li\n", c->rlimit_locks.is_set ? (long int) c->rlimit_locks.value : -1);
#endif
#ifdef RLIMIT_SIGPENDING
    pa_strbuf_printf(s, "rlimit-sigpending = %li\n", c->rlimit_sigpending.is_set ? (long int) c->rlimit_sigpending.value : -1);
#endif
#ifdef RLIMIT_MSGQUEUE
    pa_strbuf_printf(s, "rlimit-msgqueue = %li\n", c->rlimit_msgqueue.is_set ? (long int) c->rlimit_msgqueue.value : -1);
#endif
#ifdef RLIMIT_NICE
    pa_strbuf_printf(s, "rlimit-nice = %li\n", c->rlimit_nice.is_set ? (long int) c->rlimit_nice.value : -1);
#endif
#ifdef RLIMIT_RTPRIO
    pa_strbuf_printf(s, "rlimit-rtprio = %li\n", c->rlimit_rtprio.is_set ? (long int) c->rlimit_rtprio.value : -1);
#endif
#ifdef RLIMIT_RTTIME
    pa_strbuf_printf(s, "rlimit-rttime = %li\n", c->rlimit_rttime.is_set ? (long int) c->rlimit_rttime.value : -1);
#endif
#endif

    return pa_strbuf_tostring_free(s);
}
