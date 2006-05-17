#ifndef fooutf8hfoo
#define fooutf8hfoo

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

#include <polyp/cdecl.h>

/** \file
 * UTF8 Validation functions
 */

PA_C_DECL_BEGIN

/** Test if the specified strings qualifies as valid UTF8. Return the string if so, otherwise NULL */
const char *pa_utf8_valid(const char *str);

/** Filter all invalid UTF8 characters from the specified string, returning a new fully UTF8 valid string. Don't forget to free the returned string with pa_xfree() */
char *pa_utf8_filter(const char *str);

PA_C_DECL_END

#endif
