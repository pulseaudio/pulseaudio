/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <pulsecore/module.h>
#include <pulsecore/core.h>
#include <pulsecore/llist.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulse/mainloop-api.h>
#include <pulse/xmalloc.h>
#include <pulsecore/core-error.h>

#include "module-gconf-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("GConf Adapter")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("")

#define MAX_MODULES 10
#define BUF_MAX 2048

/* #undef PA_GCONF_HELPER */
/* #define PA_GCONF_HELPER "/home/lennart/projects/pulseaudio/src/gconf-helper" */

struct module_item {
    char *name;
    char *args;
    uint32_t index;
};

struct module_info {
    char *name;

    struct module_item items[MAX_MODULES];
    unsigned n_items;
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_hashmap *module_infos;

    pid_t pid;

    int fd;
    int fd_type;
    pa_io_event *io_event;

    char buf[BUF_MAX];
    size_t buf_fill;
};

static int fill_buf(struct userdata *u) {
    ssize_t r;
    pa_assert(u);

    if (u->buf_fill >= BUF_MAX) {
        pa_log("read buffer overflow");
        return -1;
    }

    if ((r = pa_read(u->fd, u->buf + u->buf_fill, BUF_MAX - u->buf_fill, &u->fd_type)) <= 0)
        return -1;

    u->buf_fill += r;
    return 0;
}

static int read_byte(struct userdata *u) {
    int ret;
    pa_assert(u);

    if (u->buf_fill < 1)
        if (fill_buf(u) < 0)
            return -1;

    ret = u->buf[0];
    pa_assert(u->buf_fill > 0);
    u->buf_fill--;
    memmove(u->buf, u->buf+1, u->buf_fill);
    return ret;
}

static char *read_string(struct userdata *u) {
    pa_assert(u);

    for (;;) {
        char *e;

        if ((e = memchr(u->buf, 0, u->buf_fill))) {
            char *ret = pa_xstrdup(u->buf);
            u->buf_fill -= e - u->buf +1;
            memmove(u->buf, e+1, u->buf_fill);
            return ret;
        }

        if (fill_buf(u) < 0)
            return NULL;
    }
}

static void unload_one_module(struct userdata *u, struct module_info*m, unsigned i) {
    pa_assert(u);
    pa_assert(m);
    pa_assert(i < m->n_items);

    if (m->items[i].index == PA_INVALID_INDEX)
        return;

    pa_log_debug("Unloading module #%i", m->items[i].index);
    pa_module_unload_by_index(u->core, m->items[i].index);
    m->items[i].index = PA_INVALID_INDEX;
    pa_xfree(m->items[i].name);
    pa_xfree(m->items[i].args);
    m->items[i].name = m->items[i].args = NULL;
}

static void unload_all_modules(struct userdata *u, struct module_info*m) {
    unsigned i;

    pa_assert(u);
    pa_assert(m);

    for (i = 0; i < m->n_items; i++)
        unload_one_module(u, m, i);

    m->n_items = 0;
}

static void load_module(
        struct userdata *u,
        struct module_info *m,
        int i,
        const char *name,
        const char *args,
        int is_new) {

    pa_module *mod;

    pa_assert(u);
    pa_assert(m);
    pa_assert(name);
    pa_assert(args);

    if (!is_new) {
        if (m->items[i].index != PA_INVALID_INDEX &&
            strcmp(m->items[i].name, name) == 0 &&
            strcmp(m->items[i].args, args) == 0)
            return;

        unload_one_module(u, m, i);
    }

    pa_log_debug("Loading module '%s' with args '%s' due to GConf configuration.", name, args);

    m->items[i].name = pa_xstrdup(name);
    m->items[i].args = pa_xstrdup(args);
    m->items[i].index = PA_INVALID_INDEX;

    if (!(mod = pa_module_load(u->core, name, args))) {
        pa_log("pa_module_load() failed");
        return;
    }

    m->items[i].index = mod->index;
}

static void module_info_free(void *p, void *userdata) {
    struct module_info *m = p;
    struct userdata *u = userdata;

    pa_assert(m);
    pa_assert(u);

    unload_all_modules(u, m);
    pa_xfree(m->name);
    pa_xfree(m);
}

static int handle_event(struct userdata *u) {
    int opcode;
    int ret = 0;

    do {
        if ((opcode = read_byte(u)) < 0)
            goto fail;

        switch (opcode) {
            case '!':
                /* The helper tool is now initialized */
                ret = 1;
                break;

            case '+': {
                char *name;
                struct module_info *m;
                unsigned i, j;

                if (!(name = read_string(u)))
                    goto fail;

                if (!(m = pa_hashmap_get(u->module_infos, name))) {
                    m = pa_xnew(struct module_info, 1);
                    m->name = name;
                    m->n_items = 0;
                    pa_hashmap_put(u->module_infos, m->name, m);
                } else
                    pa_xfree(name);

                i = 0;
                while (i < MAX_MODULES) {
                    char *module, *args;

                    if (!(module = read_string(u))) {
                        if (i > m->n_items) m->n_items = i;
                        goto fail;
                    }

                    if (!*module) {
                        pa_xfree(module);
                        break;
                    }

                    if (!(args = read_string(u))) {
                        pa_xfree(module);

                        if (i > m->n_items) m->n_items = i;
                        goto fail;
                    }

                    load_module(u, m, i, module, args, i >= m->n_items);

                    i++;

                    pa_xfree(module);
                    pa_xfree(args);
                }

                /* Unload all removed modules */
                for (j = i; j < m->n_items; j++)
                    unload_one_module(u, m, j);

                m->n_items = i;

                break;
            }

            case '-': {
                char *name;
                struct module_info *m;

                if (!(name = read_string(u)))
                    goto fail;

                if ((m = pa_hashmap_get(u->module_infos, name))) {
                    pa_hashmap_remove(u->module_infos, name);
                    module_info_free(m, u);
                }

                pa_xfree(name);

                break;
            }
        }
    } while (u->buf_fill > 0 && ret == 0);

    return ret;

fail:
    pa_log("Unable to read or parse data from client.");
    return -1;
}

static void io_event_cb(
        pa_mainloop_api*a,
        pa_io_event* e,
        int fd,
        pa_io_event_flags_t events,
        void *userdata) {

    struct userdata *u = userdata;

    if (handle_event(u) < 0) {

        if (u->io_event) {
            u->core->mainloop->io_free(u->io_event);
            u->io_event = NULL;
        }

        pa_module_unload_request(u->module);
    }
}

static int start_client(const char *n, pid_t *pid) {
    pid_t child;
    int pipe_fds[2] = { -1, -1 };

    if (pipe(pipe_fds) < 0) {
        pa_log("pipe() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if ((child = fork()) == (pid_t) -1) {
        pa_log("fork() failed: %s", pa_cstrerror(errno));
        goto fail;
    } else if (child != 0) {

        /* Parent */
        close(pipe_fds[1]);

        if (pid)
            *pid = child;

        return pipe_fds[0];
    } else {
#ifdef __linux__
        DIR* d;
#endif
        int max_fd, i;
        /* child */

        close(pipe_fds[0]);
        dup2(pipe_fds[1], 1);

        if (pipe_fds[1] != 1)
            close(pipe_fds[1]);

        close(0);
        open("/dev/null", O_RDONLY);

        close(2);
        open("/dev/null", O_WRONLY);

#ifdef __linux__

        if ((d = opendir("/proc/self/fd/"))) {

            struct dirent *de;

            while ((de = readdir(d))) {
                char *e = NULL;
                int fd;

                if (de->d_name[0] == '.')
                    continue;

                errno = 0;
                fd = strtol(de->d_name, &e, 10);
                pa_assert(errno == 0 && e && *e == 0);

                if (fd >= 3 && dirfd(d) != fd)
                    close(fd);
            }

            closedir(d);
        } else {

#endif

            max_fd = 1024;

#ifdef HAVE_SYS_RESOURCE_H
            {
                struct rlimit r;
                if (getrlimit(RLIMIT_NOFILE, &r) == 0)
                    max_fd = r.rlim_max;
            }
#endif

            for (i = 3; i < max_fd; i++)
                close(i);
#
#ifdef __linux__
        }
#endif

#ifdef PR_SET_PDEATHSIG
        /* On Linux we can use PR_SET_PDEATHSIG to have the helper
        process killed when the daemon dies abnormally. On non-Linux
        machines the client will die as soon as it writes data to
        stdout again (SIGPIPE) */

        prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
#endif

#ifdef SIGPIPE
        /* Make sure that SIGPIPE kills the child process */
        signal(SIGPIPE, SIG_DFL);
#endif

        execl(n, n, NULL);
        _exit(1);
    }

fail:
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);

    if (pipe_fds[1] >= 0)
        close(pipe_fds[1]);

    return -1;
}

int pa__init(pa_module*m) {
    struct userdata *u;
    int r;

    u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;
    u->module_infos = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    u->pid = (pid_t) -1;
    u->fd = -1;
    u->fd_type = 0;
    u->io_event = NULL;
    u->buf_fill = 0;

    if ((u->fd = start_client(PA_GCONF_HELPER, &u->pid)) < 0)
        goto fail;

    u->io_event = m->core->mainloop->io_new(
            m->core->mainloop,
            u->fd,
            PA_IO_EVENT_INPUT,
            io_event_cb,
            u);

    do {
        if ((r = handle_event(u)) < 0)
            goto fail;

        /* Read until the client signalled us that it is ready with
         * initialization */
    } while (r != 1);

    return 0;

fail:
    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->io_event)
        m->core->mainloop->io_free(u->io_event);

    if (u->fd >= 0)
        close(u->fd);

    if (u->pid != (pid_t) -1) {
        kill(u->pid, SIGTERM);
        waitpid(u->pid, NULL, 0);
    }

    if (u->module_infos)
        pa_hashmap_free(u->module_infos, module_info_free, u);

    pa_xfree(u);
}

