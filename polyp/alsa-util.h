#ifndef fooalsautilhfoo
#define fooalsautilhfoo

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

#include <asoundlib.h>

#include "sample.h"
#include "mainloop-api.h"

int pa_alsa_set_hw_params(snd_pcm_t *pcm_handle, struct pa_sample_spec *ss, uint32_t *periods, snd_pcm_uframes_t *buffer_size);

int pa_create_io_events(snd_pcm_t *pcm_handle, struct pa_mainloop_api *m, struct pa_io_event ***io_events, unsigned *n_io_events, void (*cb)(struct pa_mainloop_api*a, struct pa_io_event *e, int fd, enum pa_io_event_flags events, void *userdata), void *userdata);
void pa_free_io_events(struct pa_mainloop_api* m, struct pa_io_event **io_sources, unsigned n_io_sources);

#endif
