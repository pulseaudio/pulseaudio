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

#define ENV_CONFIG_FILE "POLYP_CONFIG"

char* config_file(void) {
    char *p, *h;

    if ((p = getenv(ENV_CONFIG_FILE)))
        return pa_xstrdup(p);

    if ((h = getenv("HOME"))) {
        struct stat st;
        p = pa_sprintf_malloc("%s/.polypaudio", h);
        if (stat(p, &st) >= 0)
            return p;
        
        pa_xfree(p);
    }

    return pa_xstrdup(DEFAULT_CONFIG_FILE);
}

void pa_cmdline_help(const char *argv0) {
    const char *e;
    char *cfg = config_file();

    if ((e = strrchr(argv0, '/')))
        e++;
    else
        e = argv0;
    
    printf("%s [options]\n"
           "  -r         Try to set high process priority (only available as root)\n"
           "  -R         Don't drop root if SETUID root\n"
           "  -L MODULE  Load the specified plugin module with the specified argument\n"
           "  -F FILE    Run the specified script\n"
           "  -C         Open a command line on the running TTY\n"
           "  -n         Don't load configuration file (%s)\n"
           "  -D         Daemonize after loading the modules\n"
           "  -d         Disallow module loading after startup\n"
           "  -f         Dont quit when the startup fails\n"
           "  -v         Verbose startup\n"
           "  -X SECS    Terminate the daemon after the last client quit and this time passed\n"
           "  -h         Show this help\n"
           "  -l TARGET  Specify the log target (syslog, stderr, auto)\n"
           "  -V         Show version\n", e, cfg);

    pa_xfree(cfg);
}

struct pa_cmdline* pa_cmdline_parse(int argc, char * const argv []) {
    char c, *cfg;
    struct pa_cmdline *cmdline = NULL;
    struct pa_strbuf *buf = NULL;
    int no_default_config_file = 0;
    assert(argc && argv);

    cmdline = pa_xmalloc(sizeof(struct pa_cmdline));
    cmdline->daemonize =
        cmdline->help =
        cmdline->verbose =
        cmdline->high_priority =
        cmdline->stay_root =
        cmdline->version =
        cmdline->disallow_module_loading = 0;
    cmdline->fail = cmdline->auto_log_target = 1;
    cmdline->quit_after_last_client_time = -1;
    cmdline->log_target = -1;

    buf = pa_strbuf_new();
    assert(buf);
    
    while ((c = getopt(argc, argv, "L:F:CDhfvrRVndX:l:")) != -1) {
        switch (c) {
            case 'L':
                pa_strbuf_printf(buf, "load %s\n", optarg);
                break;
            case 'F':
                pa_strbuf_printf(buf, ".include %s\n", optarg);
                break;
            case 'C':
                pa_strbuf_puts(buf, "load module-cli\n");
                break;
            case 'D':
                cmdline->daemonize = 1;
                break;
            case 'h':
                cmdline->help = 1;
                break;
            case 'f':
                cmdline->fail = 0;
                break;
            case 'v':
                cmdline->verbose = 1;
                break;
            case 'r':
                cmdline->high_priority = 1;
                break;
            case 'R':
                cmdline->stay_root = 1;
                break;
            case 'V':
                cmdline->version = 1;
                break;
            case 'n':
                no_default_config_file = 1;
                break;
            case 'd':
                cmdline->disallow_module_loading = 1;
                break;
            case 'X':
                cmdline->quit_after_last_client_time = atoi(optarg);
                break;
            case 'l':
                if (!strcmp(optarg, "syslog")) {
                    cmdline->auto_log_target = 0;
                    cmdline->log_target = PA_LOG_SYSLOG;
                } else if (!strcmp(optarg, "stderr")) {
                    cmdline->auto_log_target = 0;
                    cmdline->log_target = PA_LOG_STDERR;
                } else if (!strcmp(optarg, "auto"))
                    cmdline->auto_log_target = 1;
                else {
                    pa_log(__FILE__": Invalid log target: use either 'syslog', 'stderr' or 'auto'.\n");
                    goto fail;
                }
                break;
            default:
                goto fail;
        }
    }

    if (!no_default_config_file) {
        cfg = config_file();
        pa_strbuf_printf(buf, ".include %s\n", cfg);
        pa_xfree(cfg);
    }

    cmdline->cli_commands = pa_strbuf_tostring_free(buf);
    return cmdline;
    
fail:
    if (cmdline)
        pa_cmdline_free(cmdline);
    if (buf)
        pa_strbuf_free(buf);
    return NULL;
}

void pa_cmdline_free(struct pa_cmdline *cmd) {
    assert(cmd);
    pa_xfree(cmd->cli_commands);
    pa_xfree(cmd);
}
