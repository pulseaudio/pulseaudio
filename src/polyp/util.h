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

#include <polyp/sample.h>
#include <polyp/cdecl.h>

/** \file
 * Assorted utility functions */

PA_C_DECL_BEGIN

struct timeval;

/** Return the current username in the specified string buffer. */
char *pa_get_user_name(char *s, size_t l);

/** Return the current hostname in the specified buffer. */
char *pa_get_host_name(char *s, size_t l);

/** Return the fully qualified domain name in s */
char *pa_get_fqdn(char *s, size_t l);

/** Return the home directory of the current user */
char *pa_get_home_dir(char *s, size_t l);

/** Return the binary file name of the current process. This is not
 * supported on all architectures, in which case NULL is returned. */
char *pa_get_binary_name(char *s, size_t l);

/** Return a pointer to the filename inside a path (which is the last
 * component). */
const char *pa_path_get_filename(const char *p);

/** Return the current timestamp, just like UNIX gettimeofday() */
struct timeval *pa_gettimeofday(struct timeval *tv);

/** Calculate the difference between the two specified timeval
 * structs. */
pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b);

/** Compare the two timeval structs and return 0 when equal, negative when a < b, positive otherwse */
int pa_timeval_cmp(const struct timeval *a, const struct timeval *b);

/** Return the time difference between now and the specified timestamp */
pa_usec_t pa_timeval_age(const struct timeval *tv);

/** Add the specified time inmicroseconds to the specified timeval structure */
struct timeval* pa_timeval_add(struct timeval *tv, pa_usec_t v);

/** Wait t milliseconds */
int pa_msleep(unsigned long t);

PA_C_DECL_END

#endif
