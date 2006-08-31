#ifndef foopulsethreadhfoo
#define foopulsethreadhfoo

/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/def.h>

typedef struct pa_thread pa_thread;

typedef void (*pa_thread_func_t) (void *userdata);

pa_thread* pa_thread_new(pa_thread_func_t thread_func, void *userdata);
void pa_thread_free(pa_thread *t);
int pa_thread_join(pa_thread *t);
int pa_thread_is_running(pa_thread *t);
pa_thread *pa_thread_self(void);
void pa_thread_yield(void);

typedef struct pa_tls pa_tls;

pa_tls* pa_tls_new(pa_free_cb_t free_cb);
void pa_tls_free(pa_tls *t);
void * pa_tls_get(pa_tls *t);
void *pa_tls_set(pa_tls *t, void *userdata);

#endif
