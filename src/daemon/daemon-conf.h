#ifndef foodaemonconfhfoo
#define foodaemonconfhfoo

/* $Id$ */

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

#include <pulsecore/log.h>
#include <pulse/sample.h>

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

/* The actual command to execute */
typedef enum pa_daemon_conf_cmd {
    PA_CMD_DAEMON,  /* the default */
    PA_CMD_HELP,
    PA_CMD_VERSION,
    PA_CMD_DUMP_CONF,
    PA_CMD_DUMP_MODULES,
    PA_CMD_KILL,
    PA_CMD_CHECK,
    PA_CMD_DUMP_RESAMPLE_METHODS,
    PA_CMD_CLEANUP_SHM
} pa_daemon_conf_cmd_t;

#ifdef HAVE_SYS_RESOURCE_H
typedef struct pa_rlimit {
    rlim_t value;
    int is_set;
} pa_rlimit;
#endif

/* A structure containing configuration data for the PulseAudio server . */
typedef struct pa_daemon_conf {
    pa_daemon_conf_cmd_t cmd;
    int daemonize,
        fail,
        high_priority,
        disallow_module_loading,
        exit_idle_time,
        module_idle_time,
        scache_idle_time,
        auto_log_target,
        use_pid_file,
        system_instance,
        no_cpu_limit,
        disable_shm;
    char *script_commands, *dl_search_path, *default_script_file;
    pa_log_target_t log_target;
    pa_log_level_t log_level;
    int resample_method;
    char *config_file;

#ifdef HAVE_SYS_RESOURCE_H
    pa_rlimit rlimit_as, rlimit_core, rlimit_data, rlimit_fsize, rlimit_nofile, rlimit_stack;
#ifdef RLIMIT_NPROC
    pa_rlimit rlimit_nproc;
#endif
#ifdef RLIMIT_MEMLOCK
    pa_rlimit rlimit_memlock;
#endif
#endif

    unsigned default_n_fragments, default_fragment_size_msec;
    pa_sample_spec default_sample_spec;
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
