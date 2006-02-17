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
#include <sys/types.h>
#include <liboil/liboil.h>

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_LIBWRAP
#include <syslog.h>
#include <tcpd.h>
#endif

#include "../polypcore/winsock.h"

#include <polyp/mainloop.h>
#include <polyp/mainloop-signal.h>

#include <polypcore/core.h>
#include <polypcore/memblock.h>
#include <polypcore/module.h>
#include <polypcore/cli-command.h>
#include <polypcore/log.h>
#include <polypcore/util.h>
#include <polypcore/sioman.h>
#include <polypcore/xmalloc.h>
#include <polypcore/cli-text.h>
#include <polypcore/pid.h>
#include <polypcore/namereg.h>

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

#ifdef OS_IS_WIN32

static void message_cb(pa_mainloop_api*a, pa_defer_event *e, void *userdata) {
    MSG msg;

    while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            raise(SIGTERM);
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

#endif

static void signal_callback(pa_mainloop_api*m, PA_GCC_UNUSED pa_signal_event *e, int sig, void *userdata) {
    pa_log_info(__FILE__": Got signal %s.\n", pa_strsignal(sig));

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
            pa_log_notice(c);
            pa_xfree(c);
            return;
        }
#endif

        case SIGINT:
        case SIGTERM:
        default:
            pa_log_info(__FILE__": Exiting.\n");
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
    pa_defer_event *defer;
#endif

    pa_limit_caps();

#ifdef HAVE_GETUID
    suid_root = getuid() != 0 && geteuid() == 0;
    
    if (suid_root && (pa_uid_in_group("realtime", &gid) <= 0 || gid >= 1000)) {
        pa_log_warn(__FILE__": WARNING: called SUID root, but not in group 'realtime'.\n");
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

    pa_log_set_ident("polypaudio");
    
    conf = pa_daemon_conf_new();
    
    if (pa_daemon_conf_load(conf, NULL) < 0)
        goto finish;

    if (pa_daemon_conf_env(conf) < 0)
        goto finish;

    if (pa_cmdline_parse(conf, argc, argv, &d) < 0) {
        pa_log(__FILE__": failed to parse command line.\n");
        goto finish;
    }

    pa_log_set_maximal_level(conf->log_level);
    pa_log_set_target(conf->auto_log_target ? PA_LOG_STDERR : conf->log_target, NULL);

    if (conf->high_priority && conf->cmd == PA_CMD_DAEMON)
        pa_raise_priority();

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
                pa_log_info(__FILE__": daemon not running\n");
            } else {
                pa_log_info(__FILE__": daemon running as PID %u\n", pid);
                retval = 0;
            }

            goto finish;

        }
        case PA_CMD_KILL:

            if (pa_pid_file_kill(SIGINT, NULL) < 0)
                pa_log(__FILE__": failed to kill daemon.\n");
            else
                retval = 0;
            
            goto finish;
            
        default:
            assert(conf->cmd == PA_CMD_DAEMON);
    }

    if (conf->daemonize) {
        pid_t child;
        int tty_fd;

        if (pa_stdio_acquire() < 0) {
            pa_log(__FILE__": failed to acquire stdio.\n");
            goto finish;
        }

#ifdef HAVE_FORK
        if (pipe(daemon_pipe) < 0) {
            pa_log(__FILE__": failed to create pipe.\n");
            goto finish;
        }
        
        if ((child = fork()) < 0) {
            pa_log(__FILE__": fork() failed: %s\n", strerror(errno));
            goto finish;
        }

        if (child != 0) {
            /* Father */

            close(daemon_pipe[1]);
            daemon_pipe[1] = -1;

            if (pa_loop_read(daemon_pipe[0], &retval, sizeof(retval)) != sizeof(retval)) {
                pa_log(__FILE__": read() failed: %s\n", strerror(errno));
                retval = 1;
            }

            if (retval)
                pa_log(__FILE__": daemon startup failed.\n");
            else
                pa_log_info(__FILE__": daemon startup successful.\n");
            
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
    
    if (conf->use_pid_file) {
        if (pa_pid_file_create() < 0) {
            pa_log(__FILE__": pa_pid_file_create() failed.\n");
#ifdef HAVE_FORK
            if (conf->daemonize)
                pa_loop_write(daemon_pipe[1], &retval, sizeof(retval));
#endif
            goto finish;
        }

        valid_pid_file = 1;
    }

    mainloop = pa_mainloop_new();
    assert(mainloop);

    c = pa_core_new(pa_mainloop_get_api(mainloop));
    assert(c);

    r = pa_signal_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    pa_signal_new(SIGINT, signal_callback, c);
    pa_signal_new(SIGTERM, signal_callback, c);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef OS_IS_WIN32
    defer = pa_mainloop_get_api(mainloop)->defer_new(pa_mainloop_get_api(mainloop), message_cb, NULL);
    assert(defer);
#endif

    if (conf->daemonize)
        c->running_as_daemon = 1;

#ifdef SIGUSR1
    pa_signal_new(SIGUSR1, signal_callback, c);
#endif
#ifdef SIGUSR2
    pa_signal_new(SIGUSR2, signal_callback, c);
#endif
#ifdef SIGHUP
    pa_signal_new(SIGHUP, signal_callback, c);
#endif

    oil_init();
    
    r = pa_cpu_limit_init(pa_mainloop_get_api(mainloop));
    assert(r == 0);
    
    buf = pa_strbuf_new();
    assert(buf);
    if (conf->default_script_file)
        r = pa_cli_command_execute_file(c, conf->default_script_file, buf, &conf->fail);

    if (r >= 0)
        r = pa_cli_command_execute(c, conf->script_commands, buf, &conf->fail);
    pa_log(s = pa_strbuf_tostring_free(buf));
    pa_xfree(s);
    
    if (r < 0 && conf->fail) {
        pa_log(__FILE__": failed to initialize daemon.\n");
#ifdef HAVE_FORK
        if (conf->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval));
#endif
    } else if (!c->modules || pa_idxset_size(c->modules) == 0) {
        pa_log(__FILE__": daemon startup without any loaded modules, refusing to work.\n");
#ifdef HAVE_FORK
        if (conf->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval));
#endif
    } else {

        retval = 0;
#ifdef HAVE_FORK
        if (conf->daemonize)
            pa_loop_write(daemon_pipe[1], &retval, sizeof(retval));
#endif

        c->disallow_module_loading = conf->disallow_module_loading;
        c->exit_idle_time = conf->exit_idle_time;
        c->module_idle_time = conf->module_idle_time;
        c->scache_idle_time = conf->scache_idle_time;
        c->resample_method = conf->resample_method;

        if (c->default_sink_name &&
            pa_namereg_get(c, c->default_sink_name, PA_NAMEREG_SINK, 1) == NULL) {
            pa_log_error("%s : Fatal error. Default sink name (%s) does not exist in name register.\n", __FILE__, c->default_sink_name);
            retval = 1;
        } else {
            pa_log_info(__FILE__": Daemon startup complete.\n");
            if (pa_mainloop_run(mainloop, &retval) < 0)
                retval = 1;
            pa_log_info(__FILE__": Daemon shutdown initiated.\n");
        }
    }

#ifdef OS_IS_WIN32
    pa_mainloop_get_api(mainloop)->defer_free(defer);
#endif

    pa_core_free(c);

    pa_cpu_limit_done();
    pa_signal_done();
    pa_mainloop_free(mainloop);
    
    pa_log_info(__FILE__": Daemon terminated.\n");
    
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
