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

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "autoload.h"
#include "module.h"
#include "xmalloc.h"

static void entry_free(struct pa_autoload_entry *e) {
    assert(e);
    pa_xfree(e->name);
    pa_xfree(e->module);
    pa_xfree(e->argument);
    pa_xfree(e);
}

void pa_autoload_add(struct pa_core *c, const char*name, enum pa_namereg_type type, const char*module, const char *argument) {
    struct pa_autoload_entry *e = NULL;
    assert(c && name && module);
    
    if (c->autoload_hashmap && (e = pa_hashmap_get(c->autoload_hashmap, name))) {
        pa_xfree(e->module);
        pa_xfree(e->argument);
    } else {
        e = pa_xmalloc(sizeof(struct pa_autoload_entry));
        e->name = pa_xstrdup(name);

        if (!c->autoload_hashmap)
            c->autoload_hashmap = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        assert(c->autoload_hashmap);

        pa_hashmap_put(c->autoload_hashmap, e->name, e);
    }

    e->module = pa_xstrdup(module);
    e->argument = pa_xstrdup(argument);
    e->type = type;
}

int pa_autoload_remove(struct pa_core *c, const char*name, enum pa_namereg_type type) {
    struct pa_autoload_entry *e;
    assert(c && name && type);

    if (!c->autoload_hashmap || !(e = pa_hashmap_get(c->autoload_hashmap, name)))
        return -1;

    pa_hashmap_remove(c->autoload_hashmap, e->name);
    entry_free(e);
    return 0;
}

void pa_autoload_request(struct pa_core *c, const char *name, enum pa_namereg_type type) {
    struct pa_autoload_entry *e;
    struct pa_module *m;
    assert(c && name);

    if (!c->autoload_hashmap || !(e = pa_hashmap_get(c->autoload_hashmap, name)) || (e->type != type))
        return;

    if ((m = pa_module_load(c, e->module, e->argument)))
        m->auto_unload = 1;
}

static void free_func(void *p, void *userdata) {
    struct pa_autoload_entry *e = p;
    entry_free(e);
}

void pa_autoload_free(struct pa_core *c) {
    if (!c->autoload_hashmap)
        return;

    pa_hashmap_free(c->autoload_hashmap, free_func, NULL);
}
