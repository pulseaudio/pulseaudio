#ifndef foopulseclienthfoo
#define foopulseclienthfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>

typedef struct pa_client pa_client;

#include <pulse/proplist.h>
#include <pulsecore/core.h>
#include <pulsecore/module.h>

/* Every connection to the server should have a pa_client
 * attached. That way the user may generate a listing of all connected
 * clients easily and kill them if he wants.*/

struct pa_client {
    uint32_t index;
    pa_core *core;

    pa_proplist *proplist;
    pa_module *module;
    char *driver;

    void (*kill)(pa_client *c);
    void *userdata;
};

pa_client *pa_client_new(pa_core *c, const char *driver, const char *name);

/* This function should be called only by the code that created the client */
void pa_client_free(pa_client *c);

/* Code that didn't create the client should call this function to
 * request destruction of the client */
void pa_client_kill(pa_client *c);

/* Rename the client */
void pa_client_set_name(pa_client *c, const char *name);

#endif
