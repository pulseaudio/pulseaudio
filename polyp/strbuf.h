#ifndef foostrbufhfoo
#define foostrbufhfoo

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

#include "gcc-printf.h"

struct pa_strbuf;

struct pa_strbuf *pa_strbuf_new(void);
void pa_strbuf_free(struct pa_strbuf *sb);
char *pa_strbuf_tostring(struct pa_strbuf *sb);
char *pa_strbuf_tostring_free(struct pa_strbuf *sb);

int pa_strbuf_printf(struct pa_strbuf *sb, const char *format, ...)  PA_GCC_PRINTF_ATTR(2,3);
void pa_strbuf_puts(struct pa_strbuf *sb, const char *t);
void pa_strbuf_putsn(struct pa_strbuf *sb, const char *t, size_t m);

#endif
