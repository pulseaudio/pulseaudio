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

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "source-output.h"
#include "namereg.h"
#include "xmalloc.h"
#include "subscribe.h"

struct pa_source* pa_source_new(struct pa_core *core, const char *name, int fail, const struct pa_sample_spec *spec) {
    struct pa_source *s;
    char st[256];
    int r;
    assert(core && spec && name && *name);

    s = pa_xmalloc(sizeof(struct pa_source));

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SOURCE, s, fail))) {
        pa_xfree(s);
        return NULL;
    }

    s->name = pa_xstrdup(name);
    s->description = NULL;

    s->owner = NULL;
    s->core = core;
    s->sample_spec = *spec;
    s->outputs = pa_idxset_new(NULL, NULL);
    s->monitor_of = NULL;

    s->notify = NULL;
    s->userdata = NULL;

    r = pa_idxset_put(core->sources, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);

    pa_sample_spec_snprint(st, sizeof(st), spec);
    fprintf(stderr, "source: created %u \"%s\" with sample spec \"%s\"\n", s->index, s->name, st);

    pa_subscription_post(core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    
    return s;
}

void pa_source_free(struct pa_source *s) {
    struct pa_source_output *o, *j = NULL;
    assert(s);

    pa_namereg_unregister(s->core, s->name);
    
    while ((o = pa_idxset_first(s->outputs, NULL))) {
        assert(o != j);
        pa_source_output_kill(o);
        j = o;
    }
    pa_idxset_free(s->outputs, NULL, NULL);
    
    pa_idxset_remove_by_data(s->core->sources, s, NULL);

    fprintf(stderr, "source: freed %u \"%s\"\n", s->index, s->name);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
    
    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s);
}

void pa_source_notify(struct pa_source*s) {
    assert(s);

    if (s->notify)
        s->notify(s);
}

static int do_post(void *p, uint32_t index, int *del, void*userdata) {
    struct pa_memchunk *chunk = userdata;
    struct pa_source_output *o = p;
    assert(o && o->push && del && chunk);

    pa_source_output_push(o, chunk);
    return 0;
}

void pa_source_post(struct pa_source*s, struct pa_memchunk *chunk) {
    assert(s && chunk);

    pa_idxset_foreach(s->outputs, do_post, chunk);
}

void pa_source_set_owner(struct pa_source *s, struct pa_module *m) {
    assert(s);
    s->owner = m;
}
