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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <assert.h>

#include "strlist.h"
#include "xmalloc.h"
#include "strbuf.h"
#include "util.h"

struct pa_strlist {
    struct pa_strlist *next;
    char *str;
};

struct pa_strlist* pa_strlist_prepend(struct pa_strlist *l, const char *s) {
    struct pa_strlist *n;
    assert(s);
    n = pa_xmalloc(sizeof(struct pa_strlist));
    n->str = pa_xstrdup(s);
    n->next = l;
    return  n;
}

char *pa_strlist_tostring(struct pa_strlist *l) {
    int first = 1;
    struct pa_strbuf *b;

    b = pa_strbuf_new();
    for (; l; l = l->next) {
        if (!first)
            pa_strbuf_puts(b, " ");
        first = 0;
        pa_strbuf_puts(b, l->str);
    }

    return pa_strbuf_tostring_free(b);
}

struct pa_strlist* pa_strlist_remove(struct pa_strlist *l, const char *s) {
    struct pa_strlist *ret = l, *prev = NULL;
    assert(l && s);

    while (l) {
        if (!strcmp(l->str, s)) {
            struct pa_strlist *n = l->next;
            
            if (!prev) {
                assert(ret == l);
                ret = n;
            } else
                prev->next = n;

            pa_xfree(l->str);
            pa_xfree(l);

            l = n;
            
        } else {
            prev = l;
            l = l->next;
        }
    }

    return ret;
}

void pa_strlist_free(struct pa_strlist *l) {
    while (l) {
        struct pa_strlist *c = l;
        l = l->next;

        pa_xfree(c->str);
        pa_xfree(c);
    }
}

struct pa_strlist* pa_strlist_pop(struct pa_strlist *l, char **s) {
    struct pa_strlist *r;
    assert(s);
    
    if (!l) {
        *s = NULL;
        return NULL;
    }
        
    *s = l->str;
    r = l->next;
    pa_xfree(l);
    return r;
}

struct pa_strlist* pa_strlist_parse(const char *s) {
    struct pa_strlist *head = NULL, *p = NULL;
    const char *state = NULL;
    char *r;

    while ((r = pa_split_spaces(s, &state))) {
        struct pa_strlist *n;

        n = pa_xmalloc(sizeof(struct pa_strlist));
        n->str = r;
        n->next = NULL;

        if (p)
            p->next = n;
        else
            head = n;

        p = n;
    }

    return head;
}
