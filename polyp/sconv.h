#ifndef foosconvhfoo
#define foosconvhfoo

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

#include "sample.h"

typedef void (*pa_convert_to_float32_func_t)(unsigned n, const void *a, unsigned an, float *b);
typedef void (*pa_convert_from_float32_func_t)(unsigned n, const float *a, void *b, unsigned bn);

pa_convert_to_float32_func_t pa_get_convert_to_float32_function(enum pa_sample_format f);
pa_convert_from_float32_func_t pa_get_convert_from_float32_function(enum pa_sample_format f);

#endif
