#ifndef foosubscribehfoo
#define foosubscribehfoo

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

#include "core.h"
#include "native-common.h"

struct pa_subscription;
struct pa_subscription_event;

struct pa_subscription* pa_subscription_new(struct pa_core *c, enum pa_subscription_mask m,  void (*callback)(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index, void *userdata), void *userdata);
void pa_subscription_free(struct pa_subscription*s);
void pa_subscription_free_all(struct pa_core *c);

void pa_subscription_post(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index);

#endif
