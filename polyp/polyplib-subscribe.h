#ifndef foopolyplibsubscribehfoo
#define foopolyplibsubscribehfoo

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

#include <inttypes.h>

#include "polyplib-def.h"
#include "polyplib-context.h"
#include "cdecl.h"

/** \file
 * Daemon introspection event subscription subsystem. Use this
 * to be notified whenever the internal layout of daemon changes:
 * i.e. entities such as sinks or sources are create, removed or
 * modified. */

PA_C_DECL_BEGIN

/** Enable event notification */
struct pa_operation* pa_context_subscribe(struct pa_context *c, enum pa_subscription_mask m, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);

/** Set the context specific call back function that is called whenever the state of the daemon changes */
void pa_context_set_subscribe_callback(struct pa_context *c, void (*cb)(struct pa_context *c, enum pa_subscription_event_type t, uint32_t index, void *userdata), void *userdata);

PA_C_DECL_END

#endif
