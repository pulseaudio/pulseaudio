#ifndef fooiolinehfoo
#define fooiolinehfoo

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

#include "iochannel.h"

struct pa_ioline;

struct pa_ioline* pa_ioline_new(struct pa_iochannel *io);
void pa_ioline_free(struct pa_ioline *l);

void pa_ioline_puts(struct pa_ioline *s, const char *c);
void pa_ioline_set_callback(struct pa_ioline*io, void (*callback)(struct pa_ioline*io, const char *s, void *userdata), void *userdata);

#endif
