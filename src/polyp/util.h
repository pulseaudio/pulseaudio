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

struct timeval;

char *pa_get_user_name(char *s, size_t l);
char *pa_get_host_name(char *s, size_t l);
char *pa_get_fqdn(char *s, size_t l);
char *pa_get_home_dir(char *s, size_t l);

char *pa_get_binary_name(char *s, size_t l);
const char *pa_path_get_filename(const char *p);

struct timeval *pa_gettimeofday(struct timeval *tv);
pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b);
int pa_timeval_cmp(const struct timeval *a, const struct timeval *b);
pa_usec_t pa_timeval_age(const struct timeval *tv);
void pa_timeval_add(struct timeval *tv, pa_usec_t v);

int pa_msleep(unsigned long t);

#endif
