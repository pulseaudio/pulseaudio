#ifndef foostrlisthfoo
#define foostrlisthfoo

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

struct pa_strlist;

/* Add the specified server string to the list, return the new linked list head */
struct pa_strlist* pa_strlist_prepend(struct pa_strlist *l, const char *s);

/* Remove the specified string from the list, return the new linked list head */
struct pa_strlist* pa_strlist_remove(struct pa_strlist *l, const char *s);

/* Make a whitespace separated string of all server stringes. Returned memory has to be freed with pa_xfree() */
char *pa_strlist_tostring(struct pa_strlist *l);

/* Free the entire list */
void pa_strlist_free(struct pa_strlist *l);

/* Return the next entry in the list in *string and remove it from
 * the list. Returns the new list head. The memory *string points to
 * has to be freed with pa_xfree() */
struct pa_strlist* pa_strlist_pop(struct pa_strlist *l, char **s);

/* Parse a whitespace separated server list */
struct pa_strlist* pa_strlist_parse(const char *s);

#endif
