/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_SIGXCPU

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>

#include "cpulimit.h"
#include "util.h"
#include "log.h"


/* This module implements a watchdog that makes sure that the current
 * process doesn't consume more than 70% CPU time for 10 seconds. This
 * is very useful when using SCHED_FIFO scheduling which effectively
 * disables multitasking. */

/* Method of operation: Using SIGXCPU a signal handler is called every
 * 10s process CPU time. That function checks if less than 14s system
 * time have passed. In that case, it tries to contact the main event
 * loop through a pipe. After two additional seconds it is checked
 * whether the main event loop contact was successful. If not, the
 * program is terminated forcibly. */

/* Utilize this much CPU time at maximum */
#define CPUTIME_PERCENT 70

/* Check every 10s */
#define CPUTIME_INTERVAL_SOFT (10)

/* Recheck after 2s */
#define CPUTIME_INTERVAL_HARD (2)

/* Time of the last CPU load check */
static time_t last_time = 0;

/* Pipe for communicating with the main loop */
static int the_pipe[2] = {-1, -1};

/* Main event loop and IO event for the FIFO */
static struct pa_mainloop_api *api = NULL;
static struct pa_io_event *io_event = NULL;

/* Saved sigaction struct for SIGXCPU */
static struct sigaction sigaction_prev;

/* Nonzero after pa_cpu_limit_init() */
static int installed = 0; 

/* The current state of operation */
static enum  {
    PHASE_IDLE,   /* Normal state */
    PHASE_SOFT    /* After CPU overload has been detected */
} phase = PHASE_IDLE;

/* Reset the SIGXCPU timer to the next t seconds */
static void reset_cpu_time(int t) {
    int r;
    long n;
    struct rlimit rl;
    struct rusage ru;

    /* Get the current CPU time of the current process */
    r = getrusage(RUSAGE_SELF, &ru);
    assert(r >= 0);

    n = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + t;

    r = getrlimit(RLIMIT_CPU, &rl);
    assert(r >= 0);

    rl.rlim_cur = n;
    r = setrlimit(RLIMIT_CPU, &rl);
    assert(r >= 0);
}

/* A simple, thread-safe puts() work-alike */
static void write_err(const char *p) {
    pa_loop_write(2, p, strlen(p));
}

/* The signal handler, called on every SIGXCPU */
static void signal_handler(int sig) {
    assert(sig == SIGXCPU);

    if (phase == PHASE_IDLE) {
        time_t now;

#ifdef PRINT_CPU_LOAD
        char t[256];
#endif

        time(&now);

#ifdef PRINT_CPU_LOAD
        snprintf(t, sizeof(t), "Using %0.1f%% CPU\n", (double)CPUTIME_INTERVAL_SOFT/(now-last_time)*100);
        write_err(t);
#endif
        
        if (CPUTIME_INTERVAL_SOFT >= ((now-last_time)*(double)CPUTIME_PERCENT/100)) {
            static const char c = 'X';

            write_err("Soft CPU time limit exhausted, terminating.\n");
            
            /* Try a soft cleanup */
            write(the_pipe[1], &c, sizeof(c));
            phase = PHASE_SOFT;
            reset_cpu_time(CPUTIME_INTERVAL_HARD);
            
        } else {

            /* Everything's fine */
            reset_cpu_time(CPUTIME_INTERVAL_SOFT);
            last_time = now;
        }
        
    } else if (phase == PHASE_SOFT) {
        write_err("Hard CPU time limit exhausted, terminating forcibly.\n");
        _exit(1); /* Forced exit */
    }
}

/* Callback for IO events on the FIFO */
static void callback(struct pa_mainloop_api*m, struct pa_io_event*e, int fd, enum pa_io_event_flags f, void *userdata) {
    char c;
    assert(m && e && f == PA_IO_EVENT_INPUT && e == io_event && fd == the_pipe[0]);
    read(the_pipe[0], &c, sizeof(c));
    m->quit(m, 1); /* Quit the main loop */
}

/* Initializes CPU load limiter */
int pa_cpu_limit_init(struct pa_mainloop_api *m) {
    struct sigaction sa;
    assert(m && !api && !io_event && the_pipe[0] == -1 && the_pipe[1] == -1 && !installed);
    
    time(&last_time);

    /* Prepare the main loop pipe */
    if (pipe(the_pipe) < 0) {
        pa_log(__FILE__": pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    pa_make_nonblock_fd(the_pipe[0]);
    pa_make_nonblock_fd(the_pipe[1]);
    pa_fd_set_cloexec(the_pipe[0], 1);
    pa_fd_set_cloexec(the_pipe[1], 1);

    api = m;
    io_event = api->io_new(m, the_pipe[0], PA_IO_EVENT_INPUT, callback, NULL);

    phase = PHASE_IDLE;

    /* Install signal handler for SIGXCPU */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(SIGXCPU, &sa, &sigaction_prev) < 0) {
        pa_cpu_limit_done();
        return -1;
    }

    installed = 1;

    reset_cpu_time(CPUTIME_INTERVAL_SOFT);
    
    return 0;
}

/* Shutdown CPU load limiter */
void pa_cpu_limit_done(void) {
    int r;

    if (io_event) {
        assert(api);
        api->io_free(io_event);
        io_event = NULL;
        api = NULL;
    }

    if (the_pipe[0] >= 0)
        close(the_pipe[0]);
    if (the_pipe[1] >= 0)
        close(the_pipe[1]);
    the_pipe[0] = the_pipe[1] = -1;

    if (installed) {
        r = sigaction(SIGXCPU, &sigaction_prev, NULL);
        assert(r >= 0);
        installed = 0;
    }
}

#else /* HAVE_SIGXCPU */

struct pa_mainloop_api;

int pa_cpu_limit_init(struct pa_mainloop_api *m) {
    return 0;
}

void pa_cpu_limit_done(void) {
}

#endif
