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

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/stat.h>

#include "cmdline.h"
#include "util.h"
#include "strbuf.h"
#include "xmalloc.h"

enum {
    ARG_HELP = 256,
    ARG_VERSION,
    ARG_DUMP_CONF,
    ARG_DUMP_MODULES,
    ARG_DAEMONIZE,
    ARG_FAIL,
    ARG_VERBOSE,
    ARG_HIGH_PRIORITY,
    ARG_DISALLOW_MODULE_LOADING,
    ARG_EXIT_IDLE_TIME,
    ARG_MODULE_IDLE_TIME,
    ARG_LOG_TARGET,
    ARG_LOAD,
    ARG_FILE,
    ARG_DL_SEARCH_PATH,
};

static struct option long_options[] = {
    {"help",                        0, 0, ARG_HELP},
    {"version",                     0, 0, ARG_VERSION},
    {"dump-conf",                   0, 0, ARG_DUMP_CONF},
    {"dump-modules",                0, 0, ARG_DUMP_MODULES},
    {"daemonize",                   2, 0, ARG_DAEMONIZE},
    {"fail",                        2, 0, ARG_FAIL},
    {"verbose",                     2, 0, ARG_VERBOSE},
    {"high-priority",               2, 0, ARG_HIGH_PRIORITY},
    {"disallow-module-loading",     2, 0, ARG_DISALLOW_MODULE_LOADING},
    {"exit-idle-time",              2, 0, ARG_EXIT_IDLE_TIME},
    {"module-idle-time",            2, 0, ARG_MODULE_IDLE_TIME},
    {"log-target",                  1, 0, ARG_LOG_TARGET},
    {"load",                        1, 0, ARG_LOAD},
    {"file",                        1, 0, ARG_FILE},
    {"dl-search-path",              1, 0, ARG_DL_SEARCH_PATH},
    {NULL, 0, 0, 0}
};


void pa_cmdline_help(const char *argv0) {
    const char *e;

    if ((e = strrchr(argv0, '/')))
        e++;
    else
        e = argv0;
    
    printf("%s [options]\n"
           "  -h, --help                            Show this help\n"
           "      --version                         Show version\n"
           "      --dump-conf                       Dump default configuration\n"
           "      --dump-modules                    Dump list of available modules\n\n"

           "  -D, --daemonize[=BOOL]                Daemonize after startup\n"
           "      --fail[=BOOL]                     Quit when startup fails\n"
           "      --verbose[=BOOL]                  Be slightly more verbose\n"
           "      --high-priority[=BOOL]            Try to set high process priority (only available as root)\n"
           "      --disallow-module-loading[=BOOL]  Disallow module loading after startup\n"
           "      --exit-idle-time=SECS             Terminate the daemon when idle and this time passed\n"
           "      --module-idle-time=SECS           Unload autoloaded modules when idle and this time passed\n"
           "      --log-target={auto,syslog,stderr} Specify the log target\n"
           "  -p, --dl-search-path=PATH             Set the search path for dynamic shared objects (plugins)\n\n"     
           
           "  -L, --load=\"MODULE ARGUMENTS\"         Load the specified plugin module with the specified argument\n"
           "  -F, --file=FILENAME                   Run the specified script\n"
           "  -C                                    Open a command line on the running TTY after startup\n\n"
           
           "  -n                                    Don't load default script file\n", e);
}

int pa_cmdline_parse(struct pa_conf *conf, int argc, char *const argv [], int *d) {
    struct pa_strbuf *buf = NULL;
    int c;
    assert(conf && argc && argv);

    buf = pa_strbuf_new();

    if (conf->script_commands)
        pa_strbuf_puts(buf, conf->script_commands);
    
    while ((c = getopt_long(argc, argv, "L:F:ChDnp:", long_options, NULL)) != -1) {
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
                
            case ARG_LOAD:
            case 'L':
                pa_strbuf_printf(buf, "load %s\n", optarg);
                break;
                
            case ARG_FILE:
            case 'F':
                pa_strbuf_printf(buf, ".include %s\n", optarg);
                break;
                
            case 'C':
                pa_strbuf_puts(buf, "load module-cli\n");
                break;
                
            case ARG_DAEMONIZE:
            case 'D':
                if ((conf->daemonize = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --daemonize expects boolean argument\n");
                    goto fail;
                }
                break;

            case ARG_FAIL:
                if ((conf->fail = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --fail expects boolean argument\n");
                    goto fail;
                }
                break;

            case ARG_VERBOSE:
                if ((conf->verbose = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --verbose expects boolean argument\n");
                    goto fail;
                }
                break;

            case ARG_HIGH_PRIORITY:
                if ((conf->high_priority = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --high-priority expects boolean argument\n");
                    goto fail;
                }
                break;

            case ARG_DISALLOW_MODULE_LOADING:
                if ((conf->disallow_module_loading = optarg ? pa_parse_boolean(optarg) : 1) < 0) {
                    pa_log(__FILE__": --disallow-module-loading expects boolean argument\n");
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
                if (!strcmp(optarg, "syslog")) {
                    conf->auto_log_target = 0;
                    conf->log_target = PA_LOG_SYSLOG;
                } else if (!strcmp(optarg, "stderr")) {
                    conf->auto_log_target = 0;
                    conf->log_target = PA_LOG_STDERR;
                } else if (!strcmp(optarg, "auto"))
                    conf->auto_log_target = 1;
                else {
                    pa_log(__FILE__": Invalid log target: use either 'syslog', 'stderr' or 'auto'.\n");
                    goto fail;
                }
                break;

            case ARG_EXIT_IDLE_TIME:
                conf->exit_idle_time = atoi(optarg);
                break;

            case ARG_MODULE_IDLE_TIME:
                conf->module_idle_time = atoi(optarg);
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
