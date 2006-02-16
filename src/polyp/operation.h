#ifndef foopolypliboperationhfoo
#define foopolypliboperationhfoo

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

#include <polyp/cdecl.h>
#include <polyp/def.h>

/** \file
 * Asynchronous operations */

PA_C_DECL_BEGIN

/** \pa_operation
 * An asynchronous operation object */
typedef struct pa_operation pa_operation;

/** Increase the reference count by one */
pa_operation *pa_operation_ref(pa_operation *o);

/** Decrease the reference count by one */
void pa_operation_unref(pa_operation *o);

/** Cancel the operation. Beware! This will not necessarily cancel the execution of the operation on the server side. */
void pa_operation_cancel(pa_operation *o);

/** Return the current status of the operation */
pa_operation_state_t pa_operation_get_state(pa_operation *o);

PA_C_DECL_END

#endif
