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
#include <ltdl.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>
#include <sys/types.h>

#include <liboil/liboil.h>

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif

#ifdef HAVE_LIBWRAP
#include <syslog.h>
#include <tcpd.h>
#endif

#ifdef HAVE_DBUS
#include <dbus/dbus.h>
#endif

#include <pulse/mainloop.h>
#include <pulse/mainloop-signal.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/winsock.h>
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
#include <pulsecore/rtsig.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/macro.h>
#include <pulsecore/mutex.h>
#include <pulsecore/thread.h>
#include <pulsecore/once.h>
#include <pulsecore/shm.h>

#include "cmdline.h"
#include "cpulimit.h"
#include "daemon-conf.h"
#include "dumpmodules.h"
#include "caps.h"
#include "ltdl-bind-now.h"

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
    pa_log_info("Got signal %s.", pa_sig2str(sig));

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
            pa_log_info("Exiting.");
            m->quit(m, 1);
            break;
    }
}

#define set_env(key, value) putenv(pa_sprintf_malloc("%s=%s", (key), (value)))

#if defined(HAVE_PWD_H) && defined(HAVE_GRP_H)

static int change_user(void) {
    struct passwd *pw;
    struct group * gr;
    int r;

    /* This function is called only in system-wide mode. It creates a
     * runtime dir in /var/run/ with proper UID/GID and drops privs
     * afterwards. */

    if (!(pw = getpwnam(PA_SYSTEM_USER))) {
        pa_log("Failed to find user '%s'.", PA_SYSTEM_USER);
        return -1;
    }

    if (!(gr = getgrnam(PA_SYSTEM_GROUP))) {
        pa_log("Failed to find group '%s'.", PA_SYSTEM_GROUP);
        return -1;
    }

    pa_log_info("Found user '%s' (UID %lu) and group '%s' (GID %lu).",
                PA_SYSTEM_USER, (unsigned long) pw->pw_uid,
                PA_SYSTEM_GROUP, (unsigned long) gr->gr_gid);

    if (pw->pw_gid != gr->gr_gid) {
        pa_log("GID of user '%s' and of group '%s' don't match.", PA_SYSTEM_USER, PA_SYSTEM_GROUP);
        return -1;
    }

    if (strcmp(pw->pw_dir, PA_SYSTEM_RUNTIME_PATH) != 0)
        pa_log_warn("Warning: home directory of user '%s' is not '%s', ignoring.", PA_SYSTEM_USER, PA_SYSTEM_RUNTIME_PATH);

    if (pa_make_secure_dir(PA_SYSTEM_RUNTIME_PATH, 0755, pw->pw_uid, gr->gr_gid) < 0) {
        pa_log("Failed to create '%s': %s", PA_SYSTEM_RUNTIME_PATH, pa_cstrerror(errno));
        return -1;
    }

    if (initgroups(PA_SYSTEM_USER, gr->gr_gid) != 0) {
        pa_log("Failed to change group list: %s", pa_cstrerror(errno));
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
        pa_log("Failed to change GID: %s", pa_cstrerror(errno));
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
        pa_log("Failed to change UID: %s", pa_cstrerror(errno));
        return -1;
    }

    set_env("USER", PA_SYSTEM_USER);
    set_env("LOGNAME", PA_SYSTEM_GROUP);
    set_env("HOME", PA_SYSTEM_RUNTIME_PATH);

    /* Relevant for pa_runtime_path() */
    set_env("PULSE_RUNTIME_PATH", PA_SYSTEM_RUNTIME_PATH);
    set_env("PULSE_CONFIG_PATH", PA_SYSTEM_RUNTIME_PATH);

    pa_log_info("Successfully dropped root privileges.");

    return 0;
}

#else /* HAVE_PWD_H && HAVE_GRP_H */

static int change_user(void) {
    pa_log("System wide mode unsupported on this platform.");
    return -1;
}

#endif /* HAVE_PWD_H && HAVE_GRP_H */

static int create_runtime_dir(void) {
    char fn[PATH_MAX];

    pa_runtime_path(NULL, fn, sizeof(fn));

    /* This function is called only when the daemon is started in
     * per-user mode. We create the runtime directory somewhere in
     * /tmp/ with the current UID/GID */

    if (pa_make_secure_dir(fn, 0700, (uid_t)-1, (gid_t)-1) < 0) {
        pa_log("Failed to create '%s': %s", fn, pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

#ifdef HAVE_SYS_RESOURCE_H

static void set_one_rlimit(const pa_rlimit *r, int resource, const char *name) {
    struct rlimit rl;
    pa_assert(r);

    if (!r->is_set)
        return;

    rl.rlim_cur = rl.rlim_max = r->value;

    if (setrlimit(resource, &rl) < 0)
        pa_log_warn("setrlimit(%s, (%u, %u)) failed: %s", name, (unsigned) r->value, (unsigned) r->value, pa_cstrerror(errno));
}

static void set_all_rlimits(const pa_daemon_conf *conf) {
    set_one_rlimit(&conf->rlimit_as, RLIMIT_AS, "RLIMIT_AS");
    set_one_rlimit(&conf->rlimit_core, RLIMIT_CORE, "RLIMIT_CORE");
    set_one_rlimit(&conf->rlimit_data, RLIMIT_DATA, "RLIMIT_DATA");
    set_one_rlimit(&conf->rlimit_fsize, RLIMIT_FSIZE, "RLIMIT_FSIZE");
    set_one_rlimit(&conf->rlimit_nofile, RLIMIT_NOFILE, "RLIMIT_NOFILE");
    set_one_rlimit(&conf->rlimit_stack, RLIMIT_STACK, "RLIMIT_STACK");
#ifdef RLIMIT_NPROC
    set_one_rlimit(&conf->rlimit_nproc, RLIMIT_NPROC, "RLIMIT_NPROC");
#endif
#ifdef RLIMIT_MEMLOCK
    set_one_rlimit(&conf->rlimit_memlock, RLIMIT_MEMLOCK, "RLIMIT_MEMLOCK");
#endif
}
#endif

int main(int argc, char *argv[]) {
    pa_core *c = NULL;
    pa_strbuf *buf = NULL;
    pa_daemon_conf *conf = NULL;
    pa_mainloop *mainloop = NULL;
    char *s;
    int r = 0, retval = 1, d = 0;
    int daemon_pipe[2] = { -1, -1 };
    int suid_root, real_root;
    int valid_pid_file = 0;
    gid_t gid = (gid_t) -1;

#ifdef OS_IS_WIN32
    pa_time_event *timer;
    struct timeval tv;
#endif


#if defined(__linux__) && defined(__OPTIMIZE__)
    /*
       Disable lazy relocations to make usage of external libraries
       more deterministic for our RT threads. We abuse __OPTIMIZE__ as
       a check whether we are a debug build or not.
    */

    if (!getenv("LD_BIND_NOW")) {
        char *rp;

        /* We have to execute ourselves, because the libc caches the
         * value of $LD_BIND_NOW on initialization. */

        putenv(pa_xstrdup("LD_BIND_NOW=1"));
        pa_assert_se(rp = pa_readlink("/proc/self/exe"));
        pa_assert_se(execv(rp, argv) == 0);
    }
#endif

#ifdef HAVE_GETUID
    real_root = getuid() == 0;
    suid_root = !real_root && geteuid() == 0;
#else
    real_root = 0;
    suid_root = 0;
#endif

    if (suid_root) {
        /* Drop all capabilities except CAP_SYS_NICE  */
        pa_limit_caps();

        /* Drop priviliges, but keep CAP_SYS_NICE */
        pa_drop_root();

        /* After dropping root, the effective set is reset, hence,
         * let's raise it again */
        pa_limit_caps();

        /* When capabilities are not supported we will not be able to
         * aquire RT sched anymore. But yes, that's the way it is. It
         * is just too risky tun let PA run as root all the time. */
    }

    setlocale(LC_ALL, "");

    if (suid_root && (pa_own_uid_in_group(PA_REALTIME_GROUP, &gid) <= 0)) {
        pa_log_info("Warning: Called SUID root, but not in group '"PA_REALTIME_GROUP"'. "
                    "For enabling real-time scheduling please become a member of '"PA_REALTIME_GROUP"' , or increase the RLIMIT_RTPRIO user limit.");
        pa_drop_caps();
        pa_drop_root();
        suid_root = real_root = 0;
    }

    LTDL_SET_PRELOADED_SYMBOLS();

    pa_ltdl_init();

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
        pa_log("failed to parse command line.");
        goto finish;
    }

    pa_log_set_maximal_level(conf->log_level);
    pa_log_set_target(conf->auto_log_target ? PA_LOG_STDERR : conf->log_target, NULL);

    if (conf->high_priority && conf->cmd == PA_CMD_DAEMON)
        pa_raise_priority();

    if (suid_root && (conf->cmd != PA_CMD_DAEMON || !conf->high_priority)) {
        pa_drop_caps();
        pa_drop_root();
    }

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

        case PA_CMD_DUMP_RESAMPLE_METHODS: {
            int i;

            for (i = 0; i < PA_RESAMPLER_MAX; i++)
                if (pa_resample_method_supported(i))
                    printf("%s\n", pa_resample_method_to_string(i));

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
                pa_log_info("daemon not running");
            } else {
                pa_log_info("daemon running as PID %u", pid);
                retval = 0;
            }

            goto finish;

        }
        case PA_CMD_KILL:

            if (pa_pid_file_kill(SIGINT, NULL) < 0)
                pa_log("failed to kill daemon.");
            else
                retval = 0;

            goto finish;

        case PA_CMD_CLEANUP_SHM:

            if (pa_shm_cleanup() >= 0)
                retval = 0;

            goto finish;

        default:
            pa_assert(conf->cmd == PA_CMD_DAEMON);
    }

    if (real_root && !conf->system_instance) {
        pa_log_warn("This program is not intended to be run as root (unless --system is specified).");
    } else if (!real_root && conf->system_instance) {
        pa_log("Root priviliges required.");
        goto finish;
    }

    if (conf->daemonize) {
        pid_t child;
        int tty_fd;

        if (pa_stdio_acquire() < 0) {
            pa_log("failed to acquire stdio.");
            goto finish;
        }

#ifdef HAVE_FORK
        if (pipe(daemon_pipe) < 0) {
            pa_log("failed to create pipe.");
            goto finish;
        }

        if ((child = fork()) < 0) {
            pa_log("fork() failed: %s", pa_cstrerror(errno));
            goto finish;
        }

        if (child != 0) {
            /* Father */

            pa_assert_se(pa_close(daemon_pipe[1]) == 0);
            daemon_pipe[1] = -1;

            if (pa_loop_read(daemon_pipe[0], &retval, sizeof(retval), NULL) != sizeof(retval)) {
                pa_log("read() failed: %s", pa_cstrerror(errno));
                retval = 1;
            }

            if (retval)
                pa_log("daemon startup failed.");
            else
                pa_log_info("daemon startup successful.");

            goto finish;
        }

        pa_assert_se(pa_close(daemon_pipe[0]) == 0);
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
        pa_close(0);
        pa_close(1);
        pa_close(2);

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
            pa_assert_se(pa_close(tty_fd) == 0);
        }
#endif
    }

    pa_assert_se(chdir("/") == 0);
    umask(0022);

    if (conf->system_instance) {
        if (change_user() < 0)
            goto finish;
    } else if (create_runtime_dir() < 0)
        goto finish;

    if (conf->use_pid_file) {
        if (pa_pid_file_create() < 0) {
            pa_log("pa_pid_file_create() failed.");
#ifdef HAVE_FORK
            if (conf->daemonize)
                pa_loop_write(daemon_pipe[1], &retval, sizeof(retval), NULL);
#endif
            goto finish;
        }

        valid_pid_file = 1;
    }

#ifdef HAVE_SYS_RESOURCE_H
    set_all_rlimits(conf);
#endif

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    pa_log_info("Page size is %lu bytes", (unsigned long) PA_PAGE_SIZE);

    if (pa_rtclock_hrtimer())
        pa_log_info("Fresh high-resolution timers available! Bon appetit!");
    else
        pa_log_info("Dude, your kernel stinks! The chef's recommendation today is Linux with high-resolution timers enabled!");

#ifdef SIGRTMIN
    /* Valgrind uses SIGRTMAX. To easy debugging we don't use it here */
    pa_rtsig_configure(SIGRTMIN, SIGRTMAX-1);
#endif

    pa_assert_se(mainloop = pa_mainloop_new());

    if (!(c = pa_core_new(pa_mainloop_get_api(mainloop), !conf->disable_shm))) {
        pa_log("pa_core_new() failed.");
        goto finish;
    }

    c->is_system_instance = !!conf->system_instance;
    c->high_priority = !!conf->high_priority;
    c->default_sample_spec = conf->default_sample_spec;
    c->default_n_fragments = conf->default_n_fragments;
    c->default_fragment_size_msec = conf->default_fragment_size_msec;
    c->disallow_module_loading = conf->disallow_module_loading;
    c->exit_idle_time = conf->exit_idle_time;
    c->module_idle_time = conf->module_idle_time;
    c->scache_idle_time = conf->scache_idle_time;
    c->resample_method = conf->resample_method;

    pa_assert_se(pa_signal_init(pa_mainloop_get_api(mainloop)) == 0);
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
    pa_assert_se(timer = pa_mainloop_get_api(mainloop)->time_new(pa_mainloop_get_api(mainloop), pa_gettimeofday(&tv), message_cb, NULL));
#endif

    if (conf->daemonize)
        c->running_as_daemon = 1;

    oil_init();

    if (!conf->no_cpu_limit)
        pa_assert_se(pa_cpu_limit_init(pa_mainloop_get_api(mainloop)) == 0);

    buf = pa_strbuf_new();
    if (conf->default_script_file)
        r = pa_cli_command_execute_file(c, conf->default_script_file, buf, &conf->fail);

    if (r >= 0)
        r = pa_cli_command_execute(c, conf->script_commands, buf, &conf->fail);
    pa_log_error("%s", s = pa_strbuf_tostring_free(buf));
    pa_xfree(s);

    if (r < 0 && conf->fail) {
        pa_log("failed to initialize daemon.");
#ifdef HAVE_FORK
        if (conf->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval), NULL);
#endif
    } else if (!c->modules || pa_idxset_size(c->modules) == 0) {
        pa_log("daemon startup without any loaded modules, refusing to work.");
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

        if (c->default_sink_name &&
            pa_namereg_get(c, c->default_sink_name, PA_NAMEREG_SINK, 1) == NULL) {
            pa_log_error("%s : Fatal error. Default sink name (%s) does not exist in name register.", __FILE__, c->default_sink_name);
            retval = 1;
        } else {
            pa_log_info("Daemon startup complete.");
            if (pa_mainloop_run(mainloop, &retval) < 0)
                retval = 1;
            pa_log_info("Daemon shutdown initiated.");
        }
    }

#ifdef OS_IS_WIN32
    pa_mainloop_get_api(mainloop)->time_free(timer);
#endif

    pa_core_unref(c);

    if (!conf->no_cpu_limit)
        pa_cpu_limit_done();

    pa_signal_done();

    pa_log_info("Daemon terminated.");

finish:

    if (mainloop)
        pa_mainloop_free(mainloop);

    if (conf)
        pa_daemon_conf_free(conf);

    if (valid_pid_file)
        pa_pid_file_remove();

    pa_close_pipe(daemon_pipe);

#ifdef OS_IS_WIN32
    WSACleanup();
#endif

    pa_ltdl_done();

#ifdef HAVE_DBUS
    dbus_shutdown();
#endif

    return retval;
}
