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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "strbuf.h"

struct chunk {
    struct chunk *next;
    size_t length;
    char text[];
};

struct pa_strbuf {
    size_t length;
    struct chunk *head, *tail;
};

struct pa_strbuf *pa_strbuf_new(void) {
    struct pa_strbuf *sb = malloc(sizeof(struct pa_strbuf));
    assert(sb);
    sb->length = 0;
    sb->head = sb->tail = NULL;
    return sb;
}

void pa_strbuf_free(struct pa_strbuf *sb) {
    assert(sb);
    while (sb->head) {
        struct chunk *c = sb->head;
        sb->head = sb->head->next;
        free(c);
    }

    free(sb);
}

char *pa_strbuf_tostring(struct pa_strbuf *sb) {
    char *t, *e;
    struct chunk *c;
    assert(sb);

    t = malloc(sb->length+1);
    assert(t);

    e = t;
    for (c = sb->head; c; c = c->next) {
        memcpy(e, c->text, c->length);
        e += c->length;
    }

    *e = 0;
    
    return t;
}

char *pa_strbuf_tostring_free(struct pa_strbuf *sb) {
    char *t;
    assert(sb);
    t = pa_strbuf_tostring(sb);
    pa_strbuf_free(sb);
    return t;
}

void pa_strbuf_puts(struct pa_strbuf *sb, const char *t) {
    assert(sb && t);
    pa_strbuf_putsn(sb, t, strlen(t));
} 

void pa_strbuf_putsn(struct pa_strbuf *sb, const char *t, size_t l) {
    struct chunk *c;
    assert(sb && t);
    
    if (!l)
       return;
   
    c = malloc(sizeof(struct chunk)+l);
    assert(c);

    c->next = NULL;
    c->length = l;
    memcpy(c->text, t, l);

    if (sb->tail) {
        assert(sb->head);
        sb->tail->next = c;
    } else {
        assert(!sb->head);
        sb->head = c;
    }

    sb->tail = c;
    sb->length += l;
}

/* The following is based on an example from the GNU libc documentation */

int pa_strbuf_printf(struct pa_strbuf *sb, const char *format, ...) {
    int r, size = 100;
    struct chunk *c = NULL;

    assert(sb);
    
    for(;;) {
        va_list ap;

        c = realloc(c, sizeof(struct chunk)+size);
        assert(c);

        va_start(ap, format);
        r = vsnprintf(c->text, size, format, ap);
        va_end(ap);
        
        if (r > -1 && r < size) {
            c->length = r;
            c->next = NULL;
            
            if (sb->tail) {
                assert(sb->head);
                sb->tail->next = c;
            } else {
                assert(!sb->head);
                sb->head = c;
            }
            
            sb->tail = c;
            sb->length += r;
            
            return r;
        }

        if (r > -1)    /* glibc 2.1 */
            size = r+1; 
        else           /* glibc 2.0 */
            size *= 2;
    }
}
