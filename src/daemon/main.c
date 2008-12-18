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
#include <pulse/i18n.h>

#include <pulsecore/lock-autospawn.h>
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
#include "polkit.h"

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

static void message_cb(pa_mainloop_api*a, pa_time_event*e, const struct timeval *tv, void *userdata) {
    MSG msg;
    struct timeval tvnext;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
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

static void signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata) {
    pa_log_info(_("Got signal %s."), pa_sig2str(sig));

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
            pa_log_info(_("Exiting."));
            m->quit(m, 1);
            break;
    }
}

#if defined(HAVE_PWD_H) && defined(HAVE_GRP_H)

static int change_user(void) {
    struct passwd *pw;
    struct group * gr;
    int r;

    /* This function is called only in system-wide mode. It creates a
     * runtime dir in /var/run/ with proper UID/GID and drops privs
     * afterwards. */

    if (!(pw = getpwnam(PA_SYSTEM_USER))) {
        pa_log(_("Failed to find user '%s'."), PA_SYSTEM_USER);
        return -1;
    }

    if (!(gr = getgrnam(PA_SYSTEM_GROUP))) {
        pa_log(_("Failed to find group '%s'."), PA_SYSTEM_GROUP);
        return -1;
    }

    pa_log_info(_("Found user '%s' (UID %lu) and group '%s' (GID %lu)."),
                PA_SYSTEM_USER, (unsigned long) pw->pw_uid,
                PA_SYSTEM_GROUP, (unsigned long) gr->gr_gid);

    if (pw->pw_gid != gr->gr_gid) {
        pa_log(_("GID of user '%s' and of group '%s' don't match."), PA_SYSTEM_USER, PA_SYSTEM_GROUP);
        return -1;
    }

    if (strcmp(pw->pw_dir, PA_SYSTEM_RUNTIME_PATH) != 0)
        pa_log_warn(_("Home directory of user '%s' is not '%s', ignoring."), PA_SYSTEM_USER, PA_SYSTEM_RUNTIME_PATH);

    if (pa_make_secure_dir(PA_SYSTEM_RUNTIME_PATH, 0755, pw->pw_uid, gr->gr_gid) < 0) {
        pa_log(_("Failed to create '%s': %s"), PA_SYSTEM_RUNTIME_PATH, pa_cstrerror(errno));
        return -1;
    }

    if (pa_make_secure_dir(PA_SYSTEM_STATE_PATH, 0700, pw->pw_uid, gr->gr_gid) < 0) {
        pa_log(_("Failed to create '%s': %s"), PA_SYSTEM_STATE_PATH, pa_cstrerror(errno));
        return -1;
    }

    /* We don't create the config dir here, because we don't need to write to it */

    if (initgroups(PA_SYSTEM_USER, gr->gr_gid) != 0) {
        pa_log(_("Failed to change group list: %s"), pa_cstrerror(errno));
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
#error "No API to drop privileges"
#endif

    if (r < 0) {
        pa_log(_("Failed to change GID: %s"), pa_cstrerror(errno));
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
#error "No API to drop privileges"
#endif

    if (r < 0) {
        pa_log(_("Failed to change UID: %s"), pa_cstrerror(errno));
        return -1;
    }

    pa_set_env("USER", PA_SYSTEM_USER);
    pa_set_env("USERNAME", PA_SYSTEM_USER);
    pa_set_env("LOGNAME", PA_SYSTEM_USER);
    pa_set_env("HOME", PA_SYSTEM_RUNTIME_PATH);

    /* Relevant for pa_runtime_path() */
    pa_set_env("PULSE_RUNTIME_PATH", PA_SYSTEM_RUNTIME_PATH);
    pa_set_env("PULSE_CONFIG_PATH", PA_SYSTEM_CONFIG_PATH);
    pa_set_env("PULSE_STATE_PATH", PA_SYSTEM_STATE_PATH);

    pa_log_info(_("Successfully dropped root privileges."));

    return 0;
}

#else /* HAVE_PWD_H && HAVE_GRP_H */

static int change_user(void) {
    pa_log(_("System wide mode unsupported on this platform."));
    return -1;
}

#endif /* HAVE_PWD_H && HAVE_GRP_H */

#ifdef HAVE_SYS_RESOURCE_H

static int set_one_rlimit(const pa_rlimit *r, int resource, const char *name) {
    struct rlimit rl;
    pa_assert(r);

    if (!r->is_set)
        return 0;

    rl.rlim_cur = rl.rlim_max = r->value;

    if (setrlimit(resource, &rl) < 0) {
        pa_log_info(_("setrlimit(%s, (%u, %u)) failed: %s"), name, (unsigned) r->value, (unsigned) r->value, pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

static void set_all_rlimits(const pa_daemon_conf *conf) {
    set_one_rlimit(&conf->rlimit_fsize, RLIMIT_FSIZE, "RLIMIT_FSIZE");
    set_one_rlimit(&conf->rlimit_data, RLIMIT_DATA, "RLIMIT_DATA");
    set_one_rlimit(&conf->rlimit_stack, RLIMIT_STACK, "RLIMIT_STACK");
    set_one_rlimit(&conf->rlimit_core, RLIMIT_CORE, "RLIMIT_CORE");
    set_one_rlimit(&conf->rlimit_rss, RLIMIT_RSS, "RLIMIT_RSS");
#ifdef RLIMIT_NPROC
    set_one_rlimit(&conf->rlimit_nproc, RLIMIT_NPROC, "RLIMIT_NPROC");
#endif
    set_one_rlimit(&conf->rlimit_nofile, RLIMIT_NOFILE, "RLIMIT_NOFILE");
#ifdef RLIMIT_MEMLOCK
    set_one_rlimit(&conf->rlimit_memlock, RLIMIT_MEMLOCK, "RLIMIT_MEMLOCK");
#endif
    set_one_rlimit(&conf->rlimit_as, RLIMIT_AS, "RLIMIT_AS");
#ifdef RLIMIT_LOCKS
    set_one_rlimit(&conf->rlimit_locks, RLIMIT_LOCKS, "RLIMIT_LOCKS");
#endif
#ifdef RLIMIT_SIGPENDING
    set_one_rlimit(&conf->rlimit_sigpending, RLIMIT_SIGPENDING, "RLIMIT_SIGPENDING");
#endif
#ifdef RLIMIT_MSGQUEUE
    set_one_rlimit(&conf->rlimit_msgqueue, RLIMIT_MSGQUEUE, "RLIMIT_MSGQUEUE");
#endif
#ifdef RLIMIT_NICE
    set_one_rlimit(&conf->rlimit_nice, RLIMIT_NICE, "RLIMIT_NICE");
#endif
#ifdef RLIMIT_RTPRIO
    set_one_rlimit(&conf->rlimit_rtprio, RLIMIT_RTPRIO, "RLIMIT_RTPRIO");
#endif
#ifdef RLIMIT_RTTIME
    set_one_rlimit(&conf->rlimit_rttime, RLIMIT_RTTIME, "RLIMIT_RTTIME");
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
    pa_bool_t suid_root, real_root;
    pa_bool_t valid_pid_file = FALSE;
    gid_t gid = (gid_t) -1;
    pa_bool_t ltdl_init = FALSE;
    int passed_fd = -1;
    const char *e;
#ifdef HAVE_FORK
    int daemon_pipe[2] = { -1, -1 };
#endif
#ifdef OS_IS_WIN32
    pa_time_event *win32_timer;
    struct timeval win32_tv;
#endif
    char *lf = NULL;
    int autospawn_fd = -1;
    pa_bool_t autospawn_locked = FALSE;

    pa_log_set_maximal_level(PA_LOG_INFO);
    pa_log_set_ident("pulseaudio");

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

        pa_set_env("LD_BIND_NOW", "1");
        pa_assert_se(rp = pa_readlink("/proc/self/exe"));
        pa_assert_se(execv(rp, argv) == 0);
    }
#endif

#ifdef HAVE_GETUID
    real_root = getuid() == 0;
    suid_root = !real_root && geteuid() == 0;
#else
    real_root = FALSE;
    suid_root = FALSE;
#endif

    if (!real_root) {
        /* Drop all capabilities except CAP_SYS_NICE  */
        pa_limit_caps();

        /* Drop privileges, but keep CAP_SYS_NICE */
        pa_drop_root();

        /* After dropping root, the effective set is reset, hence,
         * let's raise it again */
        pa_limit_caps();

        /* When capabilities are not supported we will not be able to
         * aquire RT sched anymore. But yes, that's the way it is. It
         * is just too risky tun let PA run as root all the time. */
    }

    if ((e = getenv("PULSE_PASSED_FD"))) {
        passed_fd = atoi(e);

        if (passed_fd <= 2)
            passed_fd = -1;
    }

    pa_close_all(passed_fd, -1);

    pa_reset_sigs(-1);
    pa_unblock_sigs(-1);

    /* At this point, we are a normal user, possibly with CAP_NICE if
     * we were started SUID. If we are started as normal root, than we
     * still are normal root. */

    setlocale(LC_ALL, "");
    pa_init_i18n();

    conf = pa_daemon_conf_new();

    if (pa_daemon_conf_load(conf, NULL) < 0)
        goto finish;

    if (pa_daemon_conf_env(conf) < 0)
        goto finish;

    if (pa_cmdline_parse(conf, argc, argv, &d) < 0) {
        pa_log(_("Failed to parse command line."));
        goto finish;
    }

    pa_log_set_maximal_level(conf->log_level);
    pa_log_set_target(conf->auto_log_target ? PA_LOG_STDERR : conf->log_target, NULL);

    pa_log_debug("Started as real root: %s, suid root: %s", pa_yes_no(real_root), pa_yes_no(suid_root));

    if (!real_root && pa_have_caps()) {
#ifdef HAVE_SYS_RESOURCE_H
        struct rlimit rl;
#endif
        pa_bool_t allow_high_priority = FALSE, allow_realtime = FALSE;

        /* Let's better not enable high prio or RT by default */

        if (conf->high_priority && !allow_high_priority) {
            if (pa_own_uid_in_group(PA_REALTIME_GROUP, &gid) > 0) {
                pa_log_info(_("We're in the group '%s', allowing high-priority scheduling."), PA_REALTIME_GROUP);
                allow_high_priority = TRUE;
            }
        }

        if (conf->realtime_scheduling && !allow_realtime) {
            if (pa_own_uid_in_group(PA_REALTIME_GROUP, &gid) > 0) {
                pa_log_info(_("We're in the group '%s', allowing real-time scheduling."), PA_REALTIME_GROUP);
                allow_realtime = TRUE;
            }
        }

#ifdef HAVE_POLKIT
        if (conf->high_priority && !allow_high_priority) {
            if (pa_polkit_check("org.pulseaudio.acquire-high-priority") > 0) {
                pa_log_info(_("PolicyKit grants us acquire-high-priority privilege."));
                allow_high_priority = TRUE;
            } else
                pa_log_info(_("PolicyKit refuses acquire-high-priority privilege."));
        }

        if (conf->realtime_scheduling && !allow_realtime) {
            if (pa_polkit_check("org.pulseaudio.acquire-real-time") > 0) {
                pa_log_info(_("PolicyKit grants us acquire-real-time privilege."));
                allow_realtime = TRUE;
            } else
                pa_log_info(_("PolicyKit refuses acquire-real-time privilege."));
        }
#endif

        if (!allow_high_priority && !allow_realtime) {

            /* OK, there's no further need to keep CAP_NICE. Hence
             * let's give it up early */

            pa_drop_caps();
        }

#ifdef RLIMIT_RTPRIO
        if (getrlimit(RLIMIT_RTPRIO, &rl) >= 0)
            if (rl.rlim_cur > 0) {
                pa_log_info("RLIMIT_RTPRIO is set to %u, allowing real-time scheduling.", (unsigned) rl.rlim_cur);
                allow_realtime = TRUE;
            }
#endif
#ifdef RLIMIT_NICE
        if (getrlimit(RLIMIT_NICE, &rl) >= 0)
            if (rl.rlim_cur > 20 ) {
                pa_log_info("RLIMIT_NICE is set to %u, allowing high-priority scheduling.", (unsigned) rl.rlim_cur);
                allow_high_priority = TRUE;
            }
#endif

        if ((conf->high_priority && !allow_high_priority) ||
            (conf->realtime_scheduling && !allow_realtime))
            pa_log_notice(_("Called SUID root and real-time and/or high-priority scheduling was requested in the configuration. However, we lack the necessary privileges:\n"
                            "We are not in group '"PA_REALTIME_GROUP"', PolicyKit refuse to grant us the requested privileges and we have no increase RLIMIT_NICE/RLIMIT_RTPRIO resource limits.\n"
                            "For enabling real-time/high-priority scheduling please acquire the appropriate PolicyKit privileges, or become a member of '"PA_REALTIME_GROUP"', or increase the RLIMIT_NICE/RLIMIT_RTPRIO resource limits for this user."));


        if (!allow_realtime)
            conf->realtime_scheduling = FALSE;

        if (!allow_high_priority)
            conf->high_priority = FALSE;
    }

#ifdef HAVE_SYS_RESOURCE_H
    /* Reset resource limits. If we are run as root (for system mode)
     * this might end up increasing the limits, which is intended
     * behaviour. For all other cases, i.e. started as normal user, or
     * SUID root at this point we should have no CAP_SYS_RESOURCE and
     * increasing the limits thus should fail. Which is, too, intended
     * behaviour */

    set_all_rlimits(conf);
#endif

    if (conf->high_priority && !pa_can_high_priority()) {
        pa_log_warn(_("High-priority scheduling enabled in configuration but not allowed by policy."));
        conf->high_priority = FALSE;
    }

    if (conf->high_priority && (conf->cmd == PA_CMD_DAEMON || conf->cmd == PA_CMD_START))
        pa_raise_priority(conf->nice_level);

    pa_log_debug("Can realtime: %s, can high-priority: %s", pa_yes_no(pa_can_realtime()), pa_yes_no(pa_can_high_priority()));

    if (!real_root && pa_have_caps()) {
        pa_bool_t drop;

        drop = (conf->cmd != PA_CMD_DAEMON && conf->cmd != PA_CMD_START) || !conf->realtime_scheduling;

#ifdef RLIMIT_RTPRIO
        if (!drop) {
            struct rlimit rl;
            /* At this point we still have CAP_NICE if we were loaded
             * SUID root. If possible let's acquire RLIMIT_RTPRIO
             * instead and give CAP_NICE up. */

            if (getrlimit(RLIMIT_RTPRIO, &rl) >= 0) {

                if (rl.rlim_cur >= 9)
                    drop = TRUE;
                else {
                    rl.rlim_max = rl.rlim_cur = 9;

                    if (setrlimit(RLIMIT_RTPRIO, &rl) >= 0) {
                        pa_log_info(_("Successfully increased RLIMIT_RTPRIO"));
                        drop = TRUE;
                    } else
                        pa_log_warn(_("RLIMIT_RTPRIO failed: %s"), pa_cstrerror(errno));
                }
            }
        }
#endif

        if (drop)  {
            pa_log_info(_("Giving up CAP_NICE"));
            pa_drop_caps();
            suid_root = FALSE;
        }
    }

    if (conf->realtime_scheduling && !pa_can_realtime()) {
        pa_log_warn(_("Real-time scheduling enabled in configuration but not allowed by policy."));
        conf->realtime_scheduling = FALSE;
    }

    pa_log_debug("Can realtime: %s, can high-priority: %s", pa_yes_no(pa_can_realtime()), pa_yes_no(pa_can_high_priority()));

    LTDL_SET_PRELOADED_SYMBOLS();
    pa_ltdl_init();
    ltdl_init = TRUE;

    if (conf->dl_search_path)
        lt_dlsetsearchpath(conf->dl_search_path);

#ifdef OS_IS_WIN32
    {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 0), &data);
    }
#endif

    pa_random_seed();

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

            if (pa_pid_file_check_running(&pid, "pulseaudio") < 0)
                pa_log_info(_("Daemon not running"));
            else {
                pa_log_info(_("Daemon running as PID %u"), pid);
                retval = 0;
            }

            goto finish;

        }
        case PA_CMD_KILL:

            if (pa_pid_file_kill(SIGINT, NULL, "pulseaudio") < 0)
                pa_log(_("Failed to kill daemon: %s"), pa_cstrerror(errno));
            else
                retval = 0;

            goto finish;

        case PA_CMD_CLEANUP_SHM:

            if (pa_shm_cleanup() >= 0)
                retval = 0;

            goto finish;

        default:
            pa_assert(conf->cmd == PA_CMD_DAEMON || conf->cmd == PA_CMD_START);
    }

    if (real_root && !conf->system_instance)
        pa_log_warn(_("This program is not intended to be run as root (unless --system is specified)."));
    else if (!real_root && conf->system_instance) {
        pa_log(_("Root privileges required."));
        goto finish;
    }

    if (conf->cmd == PA_CMD_START && conf->system_instance) {
        pa_log(_("--start not supported for system instances."));
        goto finish;
    }

    if (conf->system_instance && !conf->disallow_exit)
        pa_log_warn(_("Running in system mode, but --disallow-exit not set!"));

    if (conf->system_instance && !conf->disallow_module_loading)
        pa_log_warn(_("Running in system mode, but --disallow-module-loading not set!"));

    if (conf->system_instance && !conf->disable_shm) {
        pa_log_notice(_("Running in system mode, forcibly disabling SHM mode!"));
        conf->disable_shm = TRUE;
    }

    if (conf->system_instance && conf->exit_idle_time >= 0) {
        pa_log_notice(_("Running in system mode, forcibly disabling exit idle time!"));
        conf->exit_idle_time = -1;
    }

    if (conf->cmd == PA_CMD_START) {
        /* If we shall start PA only when it is not running yet, we
         * first take the autospawn lock to make things
         * synchronous. */

        if ((autospawn_fd = pa_autospawn_lock_init()) < 0) {
            pa_log("Failed to initialize autospawn lock");
            goto finish;
        }

        if ((pa_autospawn_lock_acquire(TRUE) < 0)) {
            pa_log("Failed to acquire autospawn lock");
            goto finish;
        }

        autospawn_locked = TRUE;
    }

    if (conf->daemonize) {
        pid_t child;
        int tty_fd;

        if (pa_stdio_acquire() < 0) {
            pa_log(_("Failed to acquire stdio."));
            goto finish;
        }

#ifdef HAVE_FORK
        if (pipe(daemon_pipe) < 0) {
            pa_log(_("pipe failed: %s"), pa_cstrerror(errno));
            goto finish;
        }

        if ((child = fork()) < 0) {
            pa_log(_("fork() failed: %s"), pa_cstrerror(errno));
            goto finish;
        }

        if (child != 0) {
            ssize_t n;
            /* Father */

            pa_assert_se(pa_close(daemon_pipe[1]) == 0);
            daemon_pipe[1] = -1;

            if ((n = pa_loop_read(daemon_pipe[0], &retval, sizeof(retval), NULL)) != sizeof(retval)) {

                if (n < 0)
                    pa_log(_("read() failed: %s"), pa_cstrerror(errno));

                retval = 1;
            }

            if (retval)
                pa_log(_("Daemon startup failed."));
            else
                pa_log_info(_("Daemon startup successful."));

            goto finish;
        }

        if (autospawn_fd >= 0) {
            /* The lock file is unlocked from the parent, so we need
             * to close it in the child */

            pa_autospawn_lock_release();
            pa_autospawn_lock_done(TRUE);

            autospawn_locked = FALSE;
            autospawn_fd = -1;
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

        pa_assert_se(open("/dev/null", O_RDONLY) == 0);
        pa_assert_se(open("/dev/null", O_WRONLY) == 1);
        pa_assert_se(open("/dev/null", O_WRONLY) == 2);
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

    pa_set_env("PULSE_INTERNAL", "1");
    pa_assert_se(chdir("/") == 0);
    umask(0022);

    if (conf->system_instance)
        if (change_user() < 0)
            goto finish;

    pa_set_env("PULSE_SYSTEM", conf->system_instance ? "1" : "0");

    pa_log_info(_("This is PulseAudio %s"), PACKAGE_VERSION);
    pa_log_debug(_("Compilation host: %s"), CANONICAL_HOST);
    pa_log_debug(_("Compilation CFLAGS: %s"), PA_CFLAGS);

    s = pa_uname_string();
    pa_log_debug(_("Running on host: %s"), s);
    pa_xfree(s);

    pa_log_info(_("Page size is %lu bytes"), (unsigned long) PA_PAGE_SIZE);

#ifdef HAVE_VALGRIND_MEMCHECK_H
    pa_log_debug(_("Compiled with Valgrind support: yes"));
#else
    pa_log_debug(_("Compiled with Valgrind support: no"));
#endif

    pa_log_debug(_("Running in valgrind mode: %s"), pa_yes_no(pa_in_valgrind()));

#ifdef __OPTIMIZE__
    pa_log_debug(_("Optimized build: yes"));
#else
    pa_log_debug(_("Optimized build: no"));
#endif

    if (!(s = pa_machine_id())) {
        pa_log(_("Failed to get machine ID"));
        goto finish;
    }
    pa_log_info(_("Machine ID is %s."), s);
    pa_xfree(s);

    if (!(s = pa_get_runtime_dir()))
        goto finish;
    pa_log_info(_("Using runtime directory %s."), s);
    pa_xfree(s);

    if (!(s = pa_get_state_dir()))
        goto finish;
    pa_log_info(_("Using state directory %s."), s);
    pa_xfree(s);

    pa_log_info(_("Running in system mode: %s"), pa_yes_no(pa_in_system_mode()));

    if (conf->use_pid_file) {
        int z;

        if ((z = pa_pid_file_create("pulseaudio")) != 0) {

            if (conf->cmd == PA_CMD_START && z > 0) {
                /* If we are already running and with are run in
                 * --start mode, then let's return this as success. */

                retval = 0;
                goto finish;
            }

            pa_log(_("pa_pid_file_create() failed."));
            goto finish;
        }

        valid_pid_file = TRUE;
    }

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    if (pa_rtclock_hrtimer())
        pa_log_info(_("Fresh high-resolution timers available! Bon appetit!"));
    else
        pa_log_info(_("Dude, your kernel stinks! The chef's recommendation today is Linux with high-resolution timers enabled!"));

#ifdef SIGRTMIN
    /* Valgrind uses SIGRTMAX. To easy debugging we don't use it here */
    pa_rtsig_configure(SIGRTMIN, SIGRTMAX-1);
#endif

    pa_assert_se(mainloop = pa_mainloop_new());

    if (!(c = pa_core_new(pa_mainloop_get_api(mainloop), !conf->disable_shm, conf->shm_size))) {
        pa_log(_("pa_core_new() failed."));
        goto finish;
    }

    c->default_sample_spec = conf->default_sample_spec;
    c->default_n_fragments = conf->default_n_fragments;
    c->default_fragment_size_msec = conf->default_fragment_size_msec;
    c->exit_idle_time = conf->exit_idle_time;
    c->module_idle_time = conf->module_idle_time;
    c->scache_idle_time = conf->scache_idle_time;
    c->resample_method = conf->resample_method;
    c->realtime_priority = conf->realtime_priority;
    c->realtime_scheduling = !!conf->realtime_scheduling;
    c->disable_remixing = !!conf->disable_remixing;
    c->disable_lfe_remixing = !!conf->disable_lfe_remixing;
    c->running_as_daemon = !!conf->daemonize;
    c->disallow_exit = conf->disallow_exit;

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
    win32_timer = pa_mainloop_get_api(mainloop)->time_new(pa_mainloop_get_api(mainloop), pa_gettimeofday(&win32_tv), message_cb, NULL);
#endif

    oil_init();

    if (!conf->no_cpu_limit)
        pa_assert_se(pa_cpu_limit_init(pa_mainloop_get_api(mainloop)) == 0);

    buf = pa_strbuf_new();
    if (conf->load_default_script_file) {
        FILE *f;

        if ((f = pa_daemon_conf_open_default_script_file(conf))) {
            r = pa_cli_command_execute_file_stream(c, f, buf, &conf->fail);
            fclose(f);
        }
    }

    if (r >= 0)
        r = pa_cli_command_execute(c, conf->script_commands, buf, &conf->fail);

    pa_log_error("%s", s = pa_strbuf_tostring_free(buf));
    pa_xfree(s);

    /* We completed the initial module loading, so let's disable it
     * from now on, if requested */
    c->disallow_module_loading = !!conf->disallow_module_loading;

    if (r < 0 && conf->fail) {
        pa_log(_("Failed to initialize daemon."));
        goto finish;
    }

    if (!c->modules || pa_idxset_size(c->modules) == 0) {
        pa_log(_("Daemon startup without any loaded modules, refusing to work."));
        goto finish;
    }

    if (c->default_sink_name && !pa_namereg_get(c, c->default_sink_name, PA_NAMEREG_SINK, TRUE) && conf->fail) {
        pa_log_error(_("Default sink name (%s) does not exist in name register."), c->default_sink_name);
        goto finish;
    }

#ifdef HAVE_FORK
    if (daemon_pipe[1] >= 0) {
        int ok = 0;
        pa_loop_write(daemon_pipe[1], &ok, sizeof(ok), NULL);
        pa_close(daemon_pipe[1]);
        daemon_pipe[1] = -1;
    }
#endif

    pa_log_info(_("Daemon startup complete."));

    retval = 0;
    if (pa_mainloop_run(mainloop, &retval) < 0)
        goto finish;

    pa_log_info(_("Daemon shutdown initiated."));

finish:

    if (autospawn_fd >= 0) {
        if (autospawn_locked)
            pa_autospawn_lock_release();

        pa_autospawn_lock_done(FALSE);
    }

    if (lf)
        pa_xfree(lf);

#ifdef OS_IS_WIN32
    if (win32_timer)
        pa_mainloop_get_api(mainloop)->time_free(win32_timer);
#endif

    if (c) {
        pa_core_unref(c);
        pa_log_info(_("Daemon terminated."));
    }

    if (!conf->no_cpu_limit)
        pa_cpu_limit_done();

    pa_signal_done();

#ifdef HAVE_FORK
    if (daemon_pipe[1] >= 0)
        pa_loop_write(daemon_pipe[1], &retval, sizeof(retval), NULL);

    pa_close_pipe(daemon_pipe);
#endif

    if (mainloop)
        pa_mainloop_free(mainloop);

    if (conf)
        pa_daemon_conf_free(conf);

    if (valid_pid_file)
        pa_pid_file_remove();

#ifdef OS_IS_WIN32
    WSACleanup();
#endif

    if (ltdl_init)
        pa_ltdl_done();

#ifdef HAVE_DBUS
    dbus_shutdown();
#endif

    return retval;
}
