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

/* An ioline wraps an iochannel for line based communication. A
 * callback function is called whenever a new line has been recieved
 * from the client */

struct pa_ioline;

struct pa_ioline* pa_ioline_new(struct pa_iochannel *io);
void pa_ioline_unref(struct pa_ioline *l);
struct pa_ioline* pa_ioline_ref(struct pa_ioline *l);
void pa_ioline_close(struct pa_ioline *l);

/* Write a string to the channel */
void pa_ioline_puts(struct pa_ioline *s, const char *c);

/* Set the callback function that is called for every recieved line */
void pa_ioline_set_callback(struct pa_ioline*io, void (*callback)(struct pa_ioline*io, const char *s, void *userdata), void *userdata);

#endif
