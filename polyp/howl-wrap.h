#ifndef foohowlwrapperhfoo
#define foohowlwrapperhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <howl.h>

#include "core.h"

pa_howl_wrapper;

pa_howl_wrapper* pa_howl_wrapper_get(pa_core *c);
pa_howl_wrapper* pa_howl_wrapper_ref(pa_howl_wrapper *h);
void pa_howl_wrapper_unref(pa_howl_wrapper *h);

sw_discovery pa_howl_wrapper_get_discovery(pa_howl_wrapper *h);

#endif
