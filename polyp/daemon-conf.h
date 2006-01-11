#ifndef foodaemonconfhfoo
#define foodaemonconfhfoo

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

#include "log.h"

/* The actual command to execute */
typedef enum pa_daemon_conf_cmd {
    PA_CMD_DAEMON,  /* the default */
    PA_CMD_HELP,
    PA_CMD_VERSION,
    PA_CMD_DUMP_CONF,
    PA_CMD_DUMP_MODULES,
    PA_CMD_KILL,
    PA_CMD_CHECK
} pa_daemon_conf_cmd;

/* A structure containing configuration data for the Polypaudio server . */
typedef struct pa_daemon_conf {
    pa_daemon_conf_cmd cmd;
    int daemonize,
        fail,
        high_priority,
        disallow_module_loading,
        exit_idle_time,
        module_idle_time,
        scache_idle_time,
        auto_log_target,
        use_pid_file;
    char *script_commands, *dl_search_path, *default_script_file;
    pa_log_target log_target;
    pa_log_level log_level;
    int resample_method;
    char *config_file;
} pa_daemon_conf;

/* Allocate a new structure and fill it with sane defaults */
pa_daemon_conf* pa_daemon_conf_new(void);
void pa_daemon_conf_free(pa_daemon_conf*c);

/* Load configuration data from the specified file overwriting the
 * current settings in *c. If filename is NULL load the default daemon
 * configuration file */
int pa_daemon_conf_load(pa_daemon_conf *c, const char *filename);

/* Pretty print the current configuration data of the daemon. The
 * returned string has to be freed manually. The output of this
 * function may be parsed with pa_daemon_conf_load(). */
char *pa_daemon_conf_dump(pa_daemon_conf *c);

/* Load the configuration data from the process' environment
 * overwriting the current settings in *c. */
int pa_daemon_conf_env(pa_daemon_conf *c);

/* Set these configuration variables in the structure by passing a string */
int pa_daemon_conf_set_log_target(pa_daemon_conf *c, const char *string);
int pa_daemon_conf_set_log_level(pa_daemon_conf *c, const char *string);
int pa_daemon_conf_set_resample_method(pa_daemon_conf *c, const char *string);

#endif
