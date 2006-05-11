#ifndef fooutilhfoo
#define fooutilhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <sys/types.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include <polyp/sample.h>
#include <polypcore/gccmacro.h>

struct timeval;

void pa_make_nonblock_fd(int fd);

int pa_make_secure_dir(const char* dir);
int pa_make_secure_parent_dir(const char *fn);

ssize_t pa_read(int fd, void *buf, size_t count);
ssize_t pa_write(int fd, const void *buf, size_t count);
ssize_t pa_loop_read(int fd, void*data, size_t size);
ssize_t pa_loop_write(int fd, const void*data, size_t size);

void pa_check_signal_is_blocked(int sig);

char *pa_sprintf_malloc(const char *format, ...) PA_GCC_PRINTF_ATTR(1,2);
char *pa_vsprintf_malloc(const char *format, va_list ap);

char *pa_strlcpy(char *b, const char *s, size_t l);

char *pa_get_user_name(char *s, size_t l);
char *pa_get_host_name(char *s, size_t l);
char *pa_get_fqdn(char *s, size_t l);
char *pa_get_binary_name(char *s, size_t l);
char *pa_get_home_dir(char *s, size_t l);

const char *pa_path_get_filename(const char *p);

char *pa_parent_dir(const char *fn);

struct timeval *pa_gettimeofday(struct timeval *tv);
pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b);
int pa_timeval_cmp(const struct timeval *a, const struct timeval *b);
pa_usec_t pa_timeval_age(const struct timeval *tv);
void pa_timeval_add(struct timeval *tv, pa_usec_t v);

void pa_raise_priority(void);
void pa_reset_priority(void);

int pa_fd_set_cloexec(int fd, int b);

int pa_parse_boolean(const char *s);

char *pa_split(const char *c, const char*delimiters, const char **state);
char *pa_split_spaces(const char *c, const char **state);

char *pa_strip_nl(char *s);

const char *pa_strsignal(int sig);

int pa_own_uid_in_group(const char *name, gid_t *gid);
int pa_uid_in_group(uid_t uid, const char *name);

int pa_lock_fd(int fd, int b);

int pa_lock_lockfile(const char *fn);
int pa_unlock_lockfile(const char *fn, int fd);

FILE *pa_open_config_file(const char *env, const char *global, const char *local, char **result);

char *pa_hexstr(const uint8_t* d, size_t dlength, char *s, size_t slength);
size_t pa_parsehex(const char *p, uint8_t *d, size_t dlength);

int pa_startswith(const char *s, const char *pfx);
int pa_endswith(const char *s, const char *sfx);

char *pa_runtime_path(const char *fn, char *s, size_t l);

int pa_msleep(unsigned long t);

int pa_atoi(const char *s, int32_t *ret_i);
int pa_atou(const char *s, uint32_t *ret_u);

#endif
