#ifndef footypeidhfoo
#define footypeidhfoo

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

#include <inttypes.h>
#include <sys/types.h>

typedef uint32_t pa_typeid_t;

#define PA_TYPEID_UNKNOWN ((pa_typeid_t) -1)

char *pa_typeid_to_string(pa_typeid_t id, char *ret_s, size_t length);

#define PA_TYPEID_MAKE(a,b,c,d) (\
    (((pa_typeid_t) a & 0xFF) << 24) | \
    (((pa_typeid_t) b & 0xFF) << 16) | \
    (((pa_typeid_t) c & 0xFF) << 8) | \
    (((pa_typeid_t) d & 0xFF)))

#endif
