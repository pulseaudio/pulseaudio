#ifndef foocoreutilhfoo
#define foocoreutilhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006-2007 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <sys/types.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include <pulsecore/gccmacro.h>
#include <pulsecore/macro.h>

struct timeval;

void pa_make_fd_nonblock(int fd);
void pa_make_fd_cloexec(int fd);

int pa_make_secure_dir(const char* dir, mode_t m, uid_t uid, gid_t gid);
int pa_make_secure_parent_dir(const char *fn, mode_t, uid_t uid, gid_t gid);

ssize_t pa_read(int fd, void *buf, size_t count, int *type);
ssize_t pa_write(int fd, const void *buf, size_t count, int *type);
ssize_t pa_loop_read(int fd, void*data, size_t size, int *type);
ssize_t pa_loop_write(int fd, const void*data, size_t size, int *type);

int pa_close(int fd);

void pa_check_signal_is_blocked(int sig);

char *pa_sprintf_malloc(const char *format, ...) PA_GCC_PRINTF_ATTR(1,2);
char *pa_vsprintf_malloc(const char *format, va_list ap);

char *pa_strlcpy(char *b, const char *s, size_t l);

char *pa_parent_dir(const char *fn);

int pa_make_realtime(int rtprio);
int pa_raise_priority(int nice_level);
void pa_reset_priority(void);

int pa_parse_boolean(const char *s) PA_GCC_PURE;

static inline const char *pa_yes_no(pa_bool_t b) {
    return b ? "yes" : "no";
}

char *pa_split(const char *c, const char*delimiters, const char **state);
char *pa_split_spaces(const char *c, const char **state);

char *pa_strip_nl(char *s);

const char *pa_sig2str(int sig) PA_GCC_PURE;

int pa_own_uid_in_group(const char *name, gid_t *gid);
int pa_uid_in_group(uid_t uid, const char *name);
gid_t pa_get_gid_of_group(const char *name);
int pa_check_in_group(gid_t g);

int pa_lock_fd(int fd, int b);

int pa_lock_lockfile(const char *fn);
int pa_unlock_lockfile(const char *fn, int fd);

FILE *pa_open_config_file(const char *global, const char *local, const char *env, char **result, const char *mode);

char *pa_hexstr(const uint8_t* d, size_t dlength, char *s, size_t slength);
size_t pa_parsehex(const char *p, uint8_t *d, size_t dlength);

int pa_startswith(const char *s, const char *pfx) PA_GCC_PURE;
int pa_endswith(const char *s, const char *sfx) PA_GCC_PURE;

char *pa_runtime_path(const char *fn, char *s, size_t l);

int pa_atoi(const char *s, int32_t *ret_i);
int pa_atou(const char *s, uint32_t *ret_u);
int pa_atof(const char *s, float *ret_f);

int pa_snprintf(char *str, size_t size, const char *format, ...);

char *pa_truncate_utf8(char *c, size_t l);

char *pa_getcwd(void);
char *pa_make_path_absolute(const char *p);

void *pa_will_need(const void *p, size_t l);

static inline int pa_is_power_of_two(unsigned n) {
    return !(n & (n - 1));
}

static inline unsigned pa_make_power_of_two(unsigned n) {
    unsigned j = n;

    if (pa_is_power_of_two(n))
        return n;

    while (j) {
        j = j >> 1;
        n = n | j;
    }

    return n + 1;
}

void pa_close_pipe(int fds[2]);

char *pa_readlink(const char *p);

#endif
