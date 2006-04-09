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

#include <inttypes.h>

#include <polyp/def.h>
#include <polyp/context.h>
#include <polyp/cdecl.h>

/** \page subscribe Event subscription
 *
 * \section overv_sec Overview
 *
 * The application can be notified, asynchronously, whenever the internal
 * layout of the server changes. Possible notifications are desribed in the
 * \ref pa_subscription_event_type and \ref pa_subscription_mask
 * enumerations.
 *
 * The application sets the notification mask using pa_context_subscribe()
 * and the function that will be called whenever a notification occurs using
 * pa_context_set_subscribe_callback().
 */

/** \file
 * Daemon introspection event subscription subsystem. */

PA_C_DECL_BEGIN

/** Subscription event callback prototype */
typedef void (*pa_context_subscribe_cb_t)(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata);

/** Enable event notification */
pa_operation* pa_context_subscribe(pa_context *c, pa_subscription_mask_t m, pa_context_success_cb_t cb, void *userdata);

/** Set the context specific call back function that is called whenever the state of the daemon changes */
void pa_context_set_subscribe_callback(pa_context *c, pa_context_subscribe_cb_t cb, void *userdata);

PA_C_DECL_END

#endif
