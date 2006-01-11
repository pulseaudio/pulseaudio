#ifndef foosubscribehfoo
#define foosubscribehfoo

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

typedef struct pa_subscription pa_subscription;
typedef struct pa_subscription_event pa_subscription_event;

#include "core.h"
#include "native-common.h"

pa_subscription* pa_subscription_new(pa_core *c, pa_subscription_mask m,  void (*callback)(pa_core *c, pa_subscription_event_type t, uint32_t index, void *userdata), void *userdata);
void pa_subscription_free(pa_subscription*s);
void pa_subscription_free_all(pa_core *c);

void pa_subscription_post(pa_core *c, pa_subscription_event_type t, uint32_t idx);

#endif
