#ifndef fooclienthfoo
#define fooclienthfoo

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

#include "core.h"
#include "module.h"

/* Every connection to the server should have a pa_client
 * attached. That way the user may generate a listing of all connected
 * clients easily and kill them if he wants.*/

struct pa_client {
    uint32_t index;

    struct pa_module *owner;
    char *name;
    struct pa_core *core;
    const char *protocol_name;

    void (*kill)(struct pa_client *c);
    void *userdata;
};

/* Protocol name should be something like "ESOUND", "NATIVE", ... */
struct pa_client *pa_client_new(struct pa_core *c, const char *protocol_name, char *name);

/* This function should be called only by the code that created the client */
void pa_client_free(struct pa_client *c);

/* Code that didn't create the client should call this function to
 * request destruction of the client */
void pa_client_kill(struct pa_client *c);

/* Rename the client */
void pa_client_set_name(struct pa_client *c, const char *name);

#endif
