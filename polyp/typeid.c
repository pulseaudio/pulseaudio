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

#include <stdio.h>

#include "typeid.h"

char *pa_typeid_to_string(pa_typeid_t id, char *ret_s, size_t length) {
    if (id == PA_TYPEID_UNKNOWN)
        snprintf(ret_s, length, "????");
    else
        snprintf(ret_s, length, "%c%c%c%c", (char) (id >> 24), (char) (id >> 16), (char) (id >> 8), (char) (id));
    
    return ret_s;
}
