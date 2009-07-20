#ifndef foostrbufhfoo
#define foostrbufhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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

#include <pulse/gccmacro.h>
#include <pulsecore/macro.h>

typedef struct pa_strbuf pa_strbuf;

pa_strbuf *pa_strbuf_new(void);
void pa_strbuf_free(pa_strbuf *sb);
char *pa_strbuf_tostring(pa_strbuf *sb);
char *pa_strbuf_tostring_free(pa_strbuf *sb);

size_t pa_strbuf_printf(pa_strbuf *sb, const char *format, ...)  PA_GCC_PRINTF_ATTR(2,3);
void pa_strbuf_puts(pa_strbuf *sb, const char *t);
void pa_strbuf_putsn(pa_strbuf *sb, const char *t, size_t m);
void pa_strbuf_putc(pa_strbuf *sb, char c);

pa_bool_t pa_strbuf_isempty(pa_strbuf *sb);

#endif
