#ifndef foopulseanotifyhfoo
#define foopulseanotifyhfoo

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

/* Asynchronous thread-safe notification of mainloops */


#include <inttypes.h>
#include <pulse/mainloop-api.h>

typedef struct pa_anotify pa_anotify;
typedef void (*pa_anotify_cb_t)(uint8_t event, void *userdata);

pa_anotify *pa_anotify_new(pa_mainloop_api*api, pa_anotify_cb_t cb, void *userdata);
void pa_anotify_free(pa_anotify *a);
int pa_anotify_signal(pa_anotify *a, uint8_t event);

#endif
