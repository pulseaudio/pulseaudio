#ifndef foopulsemutexhfoo
#define foopulsemutexhfoo

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

typedef struct pa_mutex pa_mutex;

pa_mutex* pa_mutex_new(int recursive);
void pa_mutex_free(pa_mutex *m);
void pa_mutex_lock(pa_mutex *m);
void pa_mutex_unlock(pa_mutex *m);

typedef struct pa_cond pa_cond;

pa_cond *pa_cond_new(void);
void pa_cond_free(pa_cond *c);
void pa_cond_signal(pa_cond *c, int broadcast);
int pa_cond_wait(pa_cond *c, pa_mutex *m);

#endif
