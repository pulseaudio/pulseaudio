#ifndef foopdispatchhfoo
#define foopdispatchhfoo

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

#include <inttypes.h>
#include "tagstruct.h"
#include "packet.h"
#include "mainloop-api.h"

typedef struct pa_pdispatch pa_pdispatch;

typedef void (*pa_pdispatch_callback)(pa_pdispatch *pd, uint32_t command, uint32_t tag, pa_tagstruct *t, void *userdata);

pa_pdispatch* pa_pdispatch_new(pa_mainloop_api *m, const pa_pdispatch_callback*table, unsigned entries);
void pa_pdispatch_unref(pa_pdispatch *pd);
pa_pdispatch* pa_pdispatch_ref(pa_pdispatch *pd);

int pa_pdispatch_run(pa_pdispatch *pd, pa_packet*p, void *userdata);

void pa_pdispatch_register_reply(pa_pdispatch *pd, uint32_t tag, int timeout, pa_pdispatch_callback callback, void *userdata);

int pa_pdispatch_is_pending(pa_pdispatch *pd);

typedef void (*pa_pdispatch_drain_callback)(pa_pdispatch *pd, void *userdata);
void pa_pdispatch_set_drain_callback(pa_pdispatch *pd, pa_pdispatch_drain_callback callback, void *userdata);

/* Remove all reply slots with the give userdata parameter */
void pa_pdispatch_unregister_reply(pa_pdispatch *pd, void *userdata);

#endif
