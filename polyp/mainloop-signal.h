#ifndef foomainloopsignalhfoo
#define foomainloopsignalhfoo

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

#include "mainloop-api.h"
#include "cdecl.h"

PA_C_DECL_BEGIN

int pa_signal_init(struct pa_mainloop_api *api);
void pa_signal_done(void);

struct pa_signal_event;

struct pa_signal_event* pa_signal_new(int signal, void (*callback) (struct pa_mainloop_api *api, struct pa_signal_event*e, int signal, void *userdata), void *userdata);
void pa_signal_free(struct pa_signal_event *e);

void pa_signal_set_destroy(struct pa_signal_event *e, void (*callback) (struct pa_mainloop_api *api, struct pa_signal_event*e, void *userdata));

PA_C_DECL_END

#endif
