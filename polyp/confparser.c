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

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "confparser.h"
#include "log.h"
#include "util.h"
#include "xmalloc.h"

#define WHITESPACE " \t\n"
#define COMMENTS "#;\n"

static int next_assignment(const char *filename, unsigned line, const struct pa_config_item *t, const char *lvalue, const char *rvalue, void *userdata) {
    assert(filename && t && lvalue && rvalue);
    
    for (; t->parse; t++)
        if (!strcmp(lvalue, t->lvalue))
            return t->parse(filename, line, lvalue, rvalue, t->data, userdata);

    pa_log(__FILE__": [%s:%u] Unknown lvalue '%s'.\n", filename, line, lvalue);
    
    return -1;
}

static int in_string(char c, const char *s) {
    assert(s);
    
    for (; *s; s++)
        if (*s == c)
            return 1;
    
    return 0;
}

static char *strip(char *s) {
    char *b = s+strspn(s, WHITESPACE);
    char *e, *l = NULL;

    for (e = b; *e; e++)
        if (!in_string(*e, WHITESPACE))
            l = e;

    if (l)
        *(l+1) = 0;

    return b;
}

static int parse_line(const char *filename, unsigned line, const struct pa_config_item *t, char *l, void *userdata) {
    char *e, *c, *b = l+strspn(l, WHITESPACE);

    if ((c = strpbrk(b, COMMENTS)))
        *c = 0;
    
    if (!*b)
        return 0;

    if (!(e = strchr(b, '='))) {
        pa_log(__FILE__": [%s:%u] Missing '='.\n", filename, line);
        return -1;
    }

    *e = 0;
    e++;

    return next_assignment(filename, line, t, strip(b), strip(e), userdata);
}


int pa_config_parse(const char *filename, const struct pa_config_item *t, void *userdata) {
    FILE *f;
    int r = -1;
    unsigned line = 0;
    assert(filename && t);
    
    if (!(f = fopen(filename, "r"))) {
        if (errno == ENOENT) {
            r = 0;
            goto finish;
        }
        
        pa_log(__FILE__": WARNING: failed to open configuration file '%s': %s\n", filename, strerror(errno));
        goto finish;
    }

    while (!feof(f)) {
        char l[256];
        if (!fgets(l, sizeof(l), f)) {
            if (feof(f))
                break;
            
            pa_log(__FILE__": WARNING: failed to read configuration file '%s': %s\n", filename, strerror(errno));
            goto finish;
        }
            
        if (parse_line(filename, ++line, t,  l, userdata) < 0)
            goto finish;
    }
    
    r = 0;
    
finish:

    if (f)
        fclose(f);
    
    return r;
}

int pa_config_parse_int(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    int *i = data, k;
    char *x = NULL;
    assert(filename && lvalue && rvalue && data);
    
    k = strtol(rvalue, &x, 0); 
    if (!*rvalue || !x || *x) {
        pa_log(__FILE__": [%s:%u] Failed to parse numeric value: %s\n", filename, line, rvalue);
        return -1;
    }
    
    *i = k;
    return 0; 
}

int pa_config_parse_bool(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    int *b = data, k;
    assert(filename && lvalue && rvalue && data);
    
    if ((k = pa_parse_boolean(rvalue)) < 0) {
        pa_log(__FILE__": [%s:%u] Failed to parse boolean value: %s\n", filename, line, rvalue);
        return -1;
    }
    
    *b = k;
    
    return 0;
}

int pa_config_parse_string(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    char **s = data;
    assert(filename && lvalue && rvalue && data);

    pa_xfree(*s);
    *s = *rvalue ? pa_xstrdup(rvalue) : NULL;
    return 0;
}
