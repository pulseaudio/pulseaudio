#ifndef foopdispatchhfoo
#define foopdispatchhfoo

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

#include <inttypes.h>
#include "tagstruct.h"
#include "packet.h"
#include "mainloop-api.h"

struct pa_pdispatch;

/* It is safe to destroy the calling pdispatch object from all callbacks */

struct pa_pdispatch_command {
    void (*proc)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata);
};

struct pa_pdispatch* pa_pdispatch_new(struct pa_mainloop_api *m, const struct pa_pdispatch_command*table, unsigned entries);
void pa_pdispatch_free(struct pa_pdispatch *pd);

int pa_pdispatch_run(struct pa_pdispatch *pd, struct pa_packet*p, void *userdata);

void pa_pdispatch_register_reply(struct pa_pdispatch *pd, uint32_t tag, int timeout, void (*cb)(struct pa_pdispatch *pd, uint32_t command, uint32_t tag, struct pa_tagstruct *t, void *userdata), void *userdata);

int pa_pdispatch_is_pending(struct pa_pdispatch *pd);

void pa_pdispatch_set_drain_callback(struct pa_pdispatch *pd, void (*cb)(struct pa_pdispatch *pd, void *userdata), void *userdata);

/* Remove all reply slots with the give userdata parameter */
void pa_pdispatch_unregister_reply(struct pa_pdispatch *pd, void *userdata);

#endif
