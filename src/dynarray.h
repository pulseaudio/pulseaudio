#ifndef foodynarrayhfoo
#define foodynarrayhfoo

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

struct pa_dynarray;

struct pa_dynarray* pa_dynarray_new(void);
void pa_dynarray_free(struct pa_dynarray* a, void (*func)(void *p, void *userdata), void *userdata);

void pa_dynarray_put(struct pa_dynarray*a, unsigned i, void *p);
unsigned pa_dynarray_append(struct pa_dynarray*a, void *p);

void *pa_dynarray_get(struct pa_dynarray*a, unsigned i);

unsigned pa_dynarray_ncontents(struct pa_dynarray*a);

#endif
