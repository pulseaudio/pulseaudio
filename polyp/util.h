#ifndef fooutilhfoo
#define fooutilhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <sys/types.h>
#include <inttypes.h>
#include <stdarg.h>

#include "gcc-printf.h"
#include "sample.h"

void pa_make_nonblock_fd(int fd);

int pa_make_secure_dir(const char* dir);

ssize_t pa_loop_read(int fd, void*data, size_t size);
ssize_t pa_loop_write(int fd, const void*data, size_t size);

void pa_check_signal_is_blocked(int sig);

char *pa_sprintf_malloc(const char *format, ...) PA_GCC_PRINTF_ATTR(1,2);
char *pa_vsprintf_malloc(const char *format, va_list ap);

char *pa_get_user_name(char *s, size_t l);
char *pa_get_host_name(char *s, size_t l);
char *pa_get_binary_name(char *s, size_t l);

char *pa_path_get_filename(const char *p);

pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b);
int pa_timeval_cmp(const struct timeval *a, const struct timeval *b);
pa_usec_t pa_age(const struct timeval *tv);
void pa_timeval_add(struct timeval *tv, pa_usec_t v);

void pa_raise_priority(void);
void pa_reset_priority(void);

int pa_fd_set_cloexec(int fd, int b);

int pa_parse_boolean(const char *s);

char *pa_split(const char *c, const char*delimiters, const char **state);
char *pa_split_spaces(const char *c, const char **state);

const char *pa_strsignal(int sig);

int pa_parse_resample_method(const char *string);


#endif
