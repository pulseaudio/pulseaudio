#ifndef foomemoryhfoo
#define foomemoryhfoo

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

#include <sys/types.h>
#include <stdlib.h>

void* pa_xmalloc(size_t l);
void *pa_xmalloc0(size_t l);
void *pa_xrealloc(void *ptr, size_t size);
#define pa_xfree free

char *pa_xstrdup(const char *s);
char *pa_xstrndup(const char *s, size_t l);

void* pa_xmemdup(const void *p, size_t l);

#endif
