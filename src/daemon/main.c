/* $Id$ */

/***
  This file is part of PulseAudio.
 
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

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <stddef.h>
#include <assert.h>
#include <ltdl.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>

#include <liboil/liboil.h>

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_LIBWRAP
#include <syslog.h>
#include <tcpd.h>
#endif

#include "../pulsecore/winsock.h"

#include <pulse/mainloop.h>
#include <pulse/mainloop-signal.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core.h>
#include <pulsecore/memblock.h>
#include <pulsecore/module.h>
#include <pulsecore/cli-command.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sioman.h>
#include <pulsecore/cli-text.h>
#include <pulsecore/pid.h>
#include <pulsecore/namereg.h>
#include <pulsecore/random.h>

#include "cmdline.h"
#include "cpulimit.h"
#include "daemon-conf.h"
#include "dumpmodules.h"
#include "caps.h"

#ifdef HAVE_LIBWRAP
/* Only one instance of these variables */
int allow_severity = LOG_INFO;
int deny_severity = LOG_WARNING;
#endif

#ifdef HAVE_OSS
/* padsp looks for this symbol in the running process and disables
 * itself if it finds it and it is set to 7 (which is actually a bit
 * mask). For details see padsp. */
int __padsp_disabled__ = 7;
#endif

#ifdef OS_IS_WIN32

static void message_cb(pa_mainloop_api*a, pa_time_event*e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    MSG msg;
    struct timeval tvnext;

    while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            raise(SIGTERM);
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    pa_timeval_add(pa_gettimeofday(&tvnext), 100000);
    a->time_restart(e, &tvnext);
}

#endif

static void signal_callback(pa_mainloop_api*m, PA_GCC_UNUSED pa_signal_event *e, int sig, void *userdata) {
    pa_log_info(__FILE__": Got signal %s.", pa_strsignal(sig));

    switch (sig) {
#ifdef SIGUSR1
        case SIGUSR1:
            pa_module_load(userdata, "module-cli", NULL);
            break;
#endif
            
#ifdef SIGUSR2
        case SIGUSR2:
            pa_module_load(userdata, "module-cli-protocol-unix", NULL);
            break;
#endif

#ifdef SIGHUP
        case SIGHUP: {
            char *c = pa_full_status_string(userdata);
            pa_log_notice("%s", c);
            pa_xfree(c);
            return;
        }
#endif

        case SIGINT:
        case SIGTERM:
        default:
            pa_log_info(__FILE__": Exiting.");
            m->quit(m, 1);
            break;
    }
}

static void close_pipe(int p[2]) {
    if (p[0] != -1)
        close(p[0]);
    if (p[1] != -1)
        close(p[1]);
    p[0] = p[1] = -1;
}

#define set_env(key, value) putenv(pa_sprintf_malloc("%s=%s", (key), (value)))

static int change_user(void) {
    struct passwd *pw;
    struct group * gr;
    int r;

    /* This function is called only in system-wide mode. It creates a
     * runtime dir in /var/run/ with proper UID/GID and drops privs
     * afterwards. */
    
    if (!(pw = getpwnam(PA_SYSTEM_USER))) {
        pa_log(__FILE__": Failed to find user '%s'.", PA_SYSTEM_USER);
        return -1;
    }

    if (!(gr = getgrnam(PA_SYSTEM_GROUP))) {
        pa_log(__FILE__": Failed to find group '%s'.", PA_SYSTEM_GROUP);
        return -1;
    }

    pa_log_info(__FILE__": Found user '%s' (UID %lu) and group '%s' (GID %lu).",
                PA_SYSTEM_USER, (unsigned long) pw->pw_uid,
                PA_SYSTEM_GROUP, (unsigned long) gr->gr_gid);

    if (pw->pw_gid != gr->gr_gid) {
        pa_log(__FILE__": GID of user '%s' and of group '%s' don't match.", PA_SYSTEM_USER, PA_SYSTEM_GROUP);
        return -1;
    }

    if (strcmp(pw->pw_dir, PA_SYSTEM_RUNTIME_PATH) != 0)
        pa_log_warn(__FILE__": Warning: home directory of user '%s' is not '%s', ignoring.", PA_SYSTEM_USER, PA_SYSTEM_RUNTIME_PATH);

    if (pa_make_secure_dir(PA_SYSTEM_RUNTIME_PATH, 0755, pw->pw_uid, gr->gr_gid) < 0) {
        pa_log(__FILE__": Failed to create '%s': %s", PA_SYSTEM_RUNTIME_PATH, pa_cstrerror(errno));
        return -1;
    }
    
    if (initgroups(PA_SYSTEM_USER, gr->gr_gid) != 0) {
        pa_log(__FILE__": Failed to change group list: %s", pa_cstrerror(errno));
        return -1;
    }

#if defined(HAVE_SETRESGID)
    r = setresgid(gr->gr_gid, gr->gr_gid, gr->gr_gid);
#elif defined(HAVE_SETEGID)
    if ((r = setgid(gr->gr_gid)) >= 0)
        r = setegid(gr->gr_gid);
#elif defined(HAVE_SETREGID)
    r = setregid(gr->gr_gid, gr->gr_gid);
#else
#error "No API to drop priviliges"
#endif

    if (r < 0) {
        pa_log(__FILE__": Failed to change GID: %s", pa_cstrerror(errno));
        return -1;
    }

#if defined(HAVE_SETRESUID)
    r = setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid);
#elif defined(HAVE_SETEUID)
    if ((r = setuid(pw->pw_uid)) >= 0)
        r = seteuid(pw->pw_uid);
#elif defined(HAVE_SETREUID)
    r = setreuid(pw->pw_uid, pw->pw_uid);
#else
#error "No API to drop priviliges"
#endif

    if (r < 0) {
        pa_log(__FILE__": Failed to change UID: %s", pa_cstrerror(errno));
        return -1;
    }

    set_env("USER", PA_SYSTEM_USER);
    set_env("LOGNAME", PA_SYSTEM_GROUP);
    set_env("HOME", PA_SYSTEM_RUNTIME_PATH);

    /* Relevant for pa_runtime_path() */
    set_env("PULSE_RUNTIME_PATH", PA_SYSTEM_RUNTIME_PATH);
    set_env("PULSE_CONFIG_PATH", PA_SYSTEM_RUNTIME_PATH);

    pa_log_info(__FILE__": Successfully dropped root privileges.");

    return 0;
}

static int create_runtime_dir(void) {
    char fn[PATH_MAX];

    pa_runtime_path(NULL, fn, sizeof(fn));

    /* This function is called only when the daemon is started in
     * per-user mode. We create the runtime directory somewhere in
     * /tmp/ with the current UID/GID */
    
    if (pa_make_secure_dir(fn, 0700, (uid_t)-1, (gid_t)-1) < 0) {
        pa_log(__FILE__": Failed to create '%s': %s", fn, pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    pa_core *c;
    pa_strbuf *buf = NULL;
    pa_daemon_conf *conf;
    pa_mainloop *mainloop;

    char *s; 
    int r, retval = 1, d = 0;
    int daemon_pipe[2] = { -1, -1 };
    int suid_root;
    int valid_pid_file = 0;

#ifdef HAVE_GETUID
    gid_t gid = (gid_t) -1;
#endif

#ifdef OS_IS_WIN32
    pa_time_event *timer;
    struct timeval tv;
#endif

    setlocale(LC_ALL, "");

    if (getuid() != 0)
        pa_limit_caps();

#ifdef HAVE_GETUID
    suid_root = getuid() != 0 && geteuid() == 0;
    
    if (suid_root && (pa_own_uid_in_group(PA_REALTIME_GROUP, &gid) <= 0 || gid >= 1000)) {
        pa_log_warn(__FILE__": WARNING: called SUID root, but not in group '"PA_REALTIME_GROUP"'.");
        pa_drop_root();
    }
#else
    suid_root = 0;
#endif
    
    LTDL_SET_PRELOADED_SYMBOLS();
    
    r = lt_dlinit();
    assert(r == 0);

#ifdef OS_IS_WIN32
    {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 0), &data);
    }
#endif

    pa_random_seed();
    
    pa_log_set_ident("pulseaudio");
    
    conf = pa_daemon_conf_new();
    
    if (pa_daemon_conf_load(conf, NULL) < 0)
        goto finish;

    if (pa_daemon_conf_env(conf) < 0)
        goto finish;

    if (pa_cmdline_parse(conf, argc, argv, &d) < 0) {
        pa_log(__FILE__": failed to parse command line.");
        goto finish;
    }

    pa_log_set_maximal_level(conf->log_level);
    pa_log_set_target(conf->auto_log_target ? PA_LOG_STDERR : conf->log_target, NULL);

    if (conf->high_priority && conf->cmd == PA_CMD_DAEMON)
        pa_raise_priority();

    if (getuid() != 0)
        pa_drop_caps();

    if (suid_root)
        pa_drop_root();
    
    if (conf->dl_search_path)
        lt_dlsetsearchpath(conf->dl_search_path);

    switch (conf->cmd) {
        case PA_CMD_DUMP_MODULES:
            pa_dump_modules(conf, argc-d, argv+d);
            retval = 0;
            goto finish;

        case PA_CMD_DUMP_CONF: {
            s = pa_daemon_conf_dump(conf);
            fputs(s, stdout);
            pa_xfree(s);
            retval = 0;
            goto finish;
        }

        case PA_CMD_HELP :
            pa_cmdline_help(argv[0]);
            retval = 0;
            goto finish;

        case PA_CMD_VERSION :
            printf(PACKAGE_NAME" "PACKAGE_VERSION"\n");
            retval = 0;
            goto finish;

        case PA_CMD_CHECK: {
            pid_t pid;

            if (pa_pid_file_check_running(&pid) < 0) {
                pa_log_info(__FILE__": daemon not running");
            } else {
                pa_log_info(__FILE__": daemon running as PID %u", pid);
                retval = 0;
            }

            goto finish;

        }
        case PA_CMD_KILL:

            if (pa_pid_file_kill(SIGINT, NULL) < 0)
                pa_log(__FILE__": failed to kill daemon.");
            else
                retval = 0;
            
            goto finish;
            
        default:
            assert(conf->cmd == PA_CMD_DAEMON);
    }

    if (getuid() == 0 && !conf->system_instance) {
        pa_log(__FILE__": This program is not intended to be run as root (unless --system is specified).");
        goto finish;
    } else if (getuid() != 0 && conf->system_instance) {
        pa_log(__FILE__": Root priviliges required.");
        goto finish;
    }

    if (conf->daemonize) {
        pid_t child;
        int tty_fd;

        if (pa_stdio_acquire() < 0) {
            pa_log(__FILE__": failed to acquire stdio.");
            goto finish;
        }

#ifdef HAVE_FORK
        if (pipe(daemon_pipe) < 0) {
            pa_log(__FILE__": failed to create pipe.");
            goto finish;
        }
        
        if ((child = fork()) < 0) {
            pa_log(__FILE__": fork() failed: %s", pa_cstrerror(errno));
            goto finish;
        }

        if (child != 0) {
            /* Father */

            close(daemon_pipe[1]);
            daemon_pipe[1] = -1;

            if (pa_loop_read(daemon_pipe[0], &retval, sizeof(retval), NULL) != sizeof(retval)) {
                pa_log(__FILE__": read() failed: %s", pa_cstrerror(errno));
                retval = 1;
            }

            if (retval)
                pa_log(__FILE__": daemon startup failed.");
            else
                pa_log_info(__FILE__": daemon startup successful.");
            
            goto finish;
        }

        close(daemon_pipe[0]);
        daemon_pipe[0] = -1;
#endif

        if (conf->auto_log_target)
            pa_log_set_target(PA_LOG_SYSLOG, NULL);

#ifdef HAVE_SETSID
        setsid();
#endif
#ifdef HAVE_SETPGID
        setpgid(0,0);
#endif

#ifndef OS_IS_WIN32
        close(0);
        close(1);
        close(2);

        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
#else
        FreeConsole();
#endif

#ifdef SIGTTOU
        signal(SIGTTOU, SIG_IGN);
#endif
#ifdef SIGTTIN
        signal(SIGTTIN, SIG_IGN);
#endif
#ifdef SIGTSTP
        signal(SIGTSTP, SIG_IGN);
#endif
        
#ifdef TIOCNOTTY
        if ((tty_fd = open("/dev/tty", O_RDWR)) >= 0) {
            ioctl(tty_fd, TIOCNOTTY, (char*) 0);
            close(tty_fd);
        }
#endif
    }

    chdir("/");
    umask(0022);
    
    if (conf->system_instance) {
        if (change_user() < 0)
            goto finish;
    } else if (create_runtime_dir() < 0)
        goto finish;
    
    if (conf->use_pid_file) {
        if (pa_pid_file_create() < 0) {
            pa_log(__FILE__": pa_pid_file_create() failed.");
#ifdef HAVE_FORK
            if (conf->daemonize)
                pa_loop_write(daemon_pipe[1], &retval, sizeof(retval), NULL);
#endif
            goto finish;
        }

        valid_pid_file = 1;
    }

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    mainloop = pa_mainloop_new();
    assert(mainloop);

    c = pa_core_new(pa_mainloop_get_api(mainloop));
    assert(c);
    c->is_system_instance = !!conf->system_instance;

    r = pa_signal_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    pa_signal_new(SIGINT, signal_callback, c);
    pa_signal_new(SIGTERM, signal_callback, c);

#ifdef SIGUSR1
    pa_signal_new(SIGUSR1, signal_callback, c);
#endif
#ifdef SIGUSR2
    pa_signal_new(SIGUSR2, signal_callback, c);
#endif
#ifdef SIGHUP
    pa_signal_new(SIGHUP, signal_callback, c);
#endif
    
#ifdef OS_IS_WIN32
    timer = pa_mainloop_get_api(mainloop)->time_new(
        pa_mainloop_get_api(mainloop), pa_gettimeofday(&tv), message_cb, NULL);
    assert(timer);
#endif

    if (conf->daemonize)
        c->running_as_daemon = 1;

    oil_init();
    
    r = pa_cpu_limit_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    
    buf = pa_strbuf_new();
    if (conf->default_script_file)
        r = pa_cli_command_execute_file(c, conf->default_script_file, buf, &conf->fail);

    if (r >= 0)
        r = pa_cli_command_execute(c, conf->script_commands, buf, &conf->fail);
    pa_log_error("%s", s = pa_strbuf_tostring_free(buf));
    pa_xfree(s);
    
    if (r < 0 && conf->fail) {
        pa_log(__FILE__": failed to initialize daemon.");
#ifdef HAVE_FORK
        if (conf->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval), NULL);
#endif
    } else if (!c->modules || pa_idxset_size(c->modules) == 0) {
        pa_log(__FILE__": daemon startup without any loaded modules, refusing to work.");
#ifdef HAVE_FORK
        if (conf->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval), NULL);
#endif
    } else {

        retval = 0;
#ifdef HAVE_FORK
        if (conf->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval), NULL);
#endif

        c->disallow_module_loading = conf->disallow_module_loading;
        c->exit_idle_time = conf->exit_idle_time;
        c->module_idle_time = conf->module_idle_time;
        c->scache_idle_time = conf->scache_idle_time;
        c->resample_method = conf->resample_method;

        if (c->default_sink_name &&
            pa_namereg_get(c, c->default_sink_name, PA_NAMEREG_SINK, 1) == NULL) {
            pa_log_error("%s : Fatal error. Default sink name (%s) does not exist in name register.", __FILE__, c->default_sink_name);
            retval = 1;
        } else {
            pa_log_info(__FILE__": Daemon startup complete.");
            if (pa_mainloop_run(mainloop, &retval) < 0)
                retval = 1;
            pa_log_info(__FILE__": Daemon shutdown initiated.");
        }
    }

#ifdef OS_IS_WIN32
    pa_mainloop_get_api(mainloop)->time_free(timer);
#endif

    pa_core_free(c);

    pa_cpu_limit_done();
    pa_signal_done();
    pa_mainloop_free(mainloop);
    
    pa_log_info(__FILE__": Daemon terminated.");
    
finish:

    if (conf)
        pa_daemon_conf_free(conf);

    if (valid_pid_file)
        pa_pid_file_remove();
    
    close_pipe(daemon_pipe);

#ifdef OS_IS_WIN32
    WSACleanup();
#endif

    lt_dlexit();
    
    return retval;
}
