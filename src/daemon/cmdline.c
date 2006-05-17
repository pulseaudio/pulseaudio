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

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/stat.h>

#include <polyp/xmalloc.h>

#include <polypcore/core-util.h>
#include <polypcore/strbuf.h>

#include "cmdline.h"

/* Argument codes for getopt_long() */
enum {
    ARG_HELP = 256,
    ARG_VERSION,
    ARG_DUMP_CONF,
    ARG_DUMP_MODULES,
    ARG_DAEMONIZE,
    ARG_FAIL,
    ARG_LOG_LEVEL,
    ARG_HIGH_PRIORITY,
    ARG_DISALLOW_MODULE_LOADING,
    ARG_EXIT_IDLE_TIME,
    ARG_MODULE_IDLE_TIME,
    ARG_SCACHE_IDLE_TIME,
    ARG_LOG_TARGET,
    ARG_LOAD,
    ARG_FILE,
    ARG_DL_SEARCH_PATH,
    ARG_RESAMPLE_METHOD,
    ARG_KILL,
    ARG_USE_PID_FILE,
    ARG_CHECK
};

/* Tabel for getopt_long() */
static struct option long_options[] = {
    {"help",                        0, 0, ARG_HELP},
    {"version",                     0, 0, ARG_VERSION},
    {"dump-conf",                   0, 0, ARG_DUMP_CONF},
    {"dump-modules",                0, 0, ARG_DUMP_MODULES},
    {"daemonize",                   2, 0, ARG_DAEMONIZE},
    {"fail",                        2, 0, ARG_FAIL},
    {"verbose",                     2, 0, ARG_LOG_LEVEL},
    {"log-level",                   2, 0, ARG_LOG_LEVEL},
    {"high-priority",               2, 0, ARG_HIGH_PRIORITY},
    {"disallow-module-loading",     2, 0, ARG_DISALLOW_MODULE_LOADING},
    {"exit-idle-time",              2, 0, ARG_EXIT_IDLE_TIME},
    {"module-idle-time",            2, 0, ARG_MODULE_IDLE_TIME},
    {"scache-idle-time",            2, 0, ARG_SCACHE_IDLE_TIME},
    {"log-target",                  1, 0, ARG_LOG_TARGET},
    {"load",                        1, 0, ARG_LOAD},
    {"file",                        1, 0, ARG_FILE},
    {"dl-search-path",              1, 0, ARG_DL_SEARCH_PATH},
    {"resample-method",             1, 0, ARG_RESAMPLE_METHOD},
    {"kill",                        0, 0, ARG_KILL},
    {"use-pid-file",                2, 0, ARG_USE_PID_FILE},
    {"check",                       0, 0, ARG_CHECK},
    {NULL, 0, 0, 0}
};

void pa_cmdline_help(const char *argv0) {
    const char *e;

    if ((e = strrchr(argv0, '/')))
        e++;
    else
        e = argv0;
    
    printf("%s [options]\n\n"
           "COMMANDS:\n"
           "  -h, --help                            Show this help\n"
           "      --version                         Show version\n"
           "      --dump-conf                       Dump default configuration\n"
           "      --dump-modules                    Dump list of available modules\n"
           "  -k  --kill                            Kill a running daemon\n"
           "      --check                           Check for a running daemon\n\n"

           "OPTIONS:\n"
           "  -D, --daemonize[=BOOL]                Daemonize after startup\n"
           "      --fail[=BOOL]                     Quit when startup fails\n"
           "      --high-priority[=BOOL]            Try to set high process priority\n"
           "                                        (only available as root)\n"
           "      --disallow-module-loading[=BOOL]  Disallow module loading after startup\n"
           "      --exit-idle-time=SECS             Terminate the daemon when idle and this\n"
           "                                        time passed\n"
           "      --module-idle-time=SECS           Unload autoloaded modules when idle and\n"
           "                                        this time passed\n"
           "      --scache-idle-time=SECS           Unload autoloaded samples when idle and\n"
           "                                        this time passed\n"
           "      --log-level[=LEVEL]               Increase or set verbosity level\n"
           "  -v                                    Increase the verbosity level\n" 
           "      --log-target={auto,syslog,stderr} Specify the log target\n"
           "  -p, --dl-search-path=PATH             Set the search path for dynamic shared\n"
           "                                        objects (plugins)\n"
           "      --resample-method=[METHOD]        Use the specified resampling method\n"
           "                                        (one of src-sinc-medium-quality,\n"
           "                                        src-sinc-best-quality,src-sinc-fastest\n"
           "                                        src-zero-order-hold,src-linear,trivial)\n"
           "      --use-pid-file[=BOOL]             Create a PID file\n\n"

           "STARTUP SCRIPT:\n"
           "  -L, --load=\"MODULE ARGUMENTS\"         Load the specified plugin module with\n"
           "                                        the specified argument\n"
           "  -F, --file=FILENAME                   Run the specified script\n"
           "  -C                                    Open a command line on the running TTY\n"
           "                                        after startup\n\n"
           
           "  -n                                    Don't load default script file\n", e);
}

int pa_cmdline_parse(pa_daemon_conf *conf, int argc, char *const argv [], int *d) {
    pa_strbuf *buf = NULL;
    int c;
    assert(conf && argc && argv);

    buf = pa_strbuf_new();

    if (conf->script_commands)
        pa_strbuf_puts(buf, conf->script_commands);
    
    while ((c = getopt_long(argc, argv, "L:F:ChDnp:kv", long_options, NULL)) != -1) {
        switch (c) {
            case ARG_HELP:
            case 'h':
                conf->cmd = PA_CMD_HELP;
                break;

            case ARG_VERSION:
                conf->cmd = PA_CMD_VERSION;
                break;

            case ARG_DUMP_CONF:
                conf->cmd = PA_CMD_DUMP_CONF;
                break;

            case ARG_DUMP_MODULES:
                conf->cmd = PA_CMD_DUMP_MODULES;
                break;

            case 'k':
            case ARG_KILL:
                conf->cmd = PA_CMD_KILL;
                break;

            case ARG_CHECK:
                conf->cmd = PA_CMD_CHECK;
                break;
                
            case ARG_LOAD:
            case 'L':
                pa_strbuf_printf(buf, "load-module %s\n", optarg);
                break;
                
            case ARG_FILE:
            case 'F':
                pa_strbuf_printf(buf, ".include %s\n", optarg);
                break;
                
            case 'C':
                pa_strbuf_puts(buf, "load-module module-cli\n");
                break;
                
            case ARG_DAEMONIZE:
            case 'D':
                if ((conf->daemonize = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --daemonize expects boolean argument");
                    goto fail;
                }
                break;

            case ARG_FAIL:
                if ((conf->fail = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --fail expects boolean argument");
                    goto fail;
                }
                break;

            case 'v':
            case ARG_LOG_LEVEL:

                if (optarg) {
                    if (pa_daemon_conf_set_log_level(conf, optarg) < 0) {
                        pa_log(__FILE__": --log-level expects log level argument (either numeric in range 0..4 or one of debug, info, notice, warn, error).");
                        goto fail;
                    }
                } else {
                    if (conf->log_level < PA_LOG_LEVEL_MAX-1)
                        conf->log_level++;
                }
                
                break;

            case ARG_HIGH_PRIORITY:
                if ((conf->high_priority = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --high-priority expects boolean argument");
                    goto fail;
                }
                break;

            case ARG_DISALLOW_MODULE_LOADING:
                if ((conf->disallow_module_loading = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --disallow-module-loading expects boolean argument");
                    goto fail;
                }
                break;

            case ARG_USE_PID_FILE:
                if ((conf->use_pid_file = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --use-pid-file expects boolean argument");
                    goto fail;
                }
                break;
                
            case 'p':
            case ARG_DL_SEARCH_PATH:
                pa_xfree(conf->dl_search_path);
                conf->dl_search_path = *optarg ? pa_xstrdup(optarg) : NULL;
                break;
                
            case 'n':
                pa_xfree(conf->default_script_file);
                conf->default_script_file = NULL;
                break;

            case ARG_LOG_TARGET:
                if (pa_daemon_conf_set_log_target(conf, optarg) < 0) {
                    pa_log(__FILE__": Invalid log target: use either 'syslog', 'stderr' or 'auto'.");
                    goto fail;
                }
                break;

            case ARG_EXIT_IDLE_TIME:
                conf->exit_idle_time = atoi(optarg);
                break;

            case ARG_MODULE_IDLE_TIME:
                conf->module_idle_time = atoi(optarg);
                break;

            case ARG_SCACHE_IDLE_TIME:
                conf->scache_idle_time = atoi(optarg);
                break;

            case ARG_RESAMPLE_METHOD:
                if (pa_daemon_conf_set_resample_method(conf, optarg) < 0) {
                    pa_log(__FILE__": Invalid resample method '%s'.", optarg);
                    goto fail;
                }
                break;
                
            default:
                goto fail;
        }
    }

    pa_xfree(conf->script_commands);
    conf->script_commands = pa_strbuf_tostring_free(buf);

    if (!conf->script_commands) {
        pa_xfree(conf->script_commands);
        conf->script_commands = NULL;
    }

    *d = optind;
    
    return 0;
    
fail:
    if (buf)
        pa_strbuf_free(buf);
    
    return -1;
}
