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

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "iochannel.h"
#include "sink.h"
#include "module.h"
#include "util.h"
#include "modargs.h"
#include "xmalloc.h"
#include "log.h"
#include "module-null-sink-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Clocked NULL sink")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("format=<sample format> channels=<number of channels> rate=<sample rate> sink_name=<name of sink>")

#define DEFAULT_SINK_NAME "null"

struct userdata {
    struct pa_core *core;
    struct pa_module *module;
    struct pa_sink *sink;
    struct pa_time_event *time_event;
    size_t block_size;
};

static const char* const valid_modargs[] = {
    "rate",
    "format",
    "channels",
    "sink_name",
    NULL
};

static void time_callback(struct pa_mainloop_api *m, struct pa_time_event*e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct pa_memchunk chunk;
    struct timeval ntv = *tv;
    size_t l;

    assert(u);

    if (pa_sink_render(u->sink, u->block_size, &chunk) >= 0) {
        l = chunk.length;
        pa_memblock_unref(chunk.memblock);
    } else
        l = u->block_size;

    pa_timeval_add(&ntv, pa_bytes_to_usec(l, &u->sink->sample_spec));
    m->time_restart(e, &ntv);
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u = NULL;
    struct pa_sample_spec ss;
    struct pa_modargs *ma = NULL;
    struct timeval tv;
    assert(c && m);
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments.\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": invalid sample format specification.\n");
        goto fail;
    }
    
    u = pa_xmalloc0(sizeof(struct userdata));
    u->core = c;
    u->module = m;
    m->userdata = u;
    
    if (!(u->sink = pa_sink_new(c, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss))) {
        pa_log(__FILE__": failed to create sink.\n");
        goto fail;
    }
    
    u->sink->userdata = u;
    pa_sink_set_owner(u->sink, m);
    u->sink->description = pa_sprintf_malloc("NULL sink");

    gettimeofday(&tv, NULL);
    u->time_event = c->mainloop->time_new(c->mainloop, &tv, time_callback, u);

    u->block_size = pa_bytes_per_second(&ss) / 10;
    
    pa_modargs_free(ma);
    
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
        
    pa__done(c, m);

    return -1;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    pa_sink_disconnect(u->sink);
    pa_sink_unref(u->sink);

    u->core->mainloop->time_free(u->time_event);

    pa_xfree(u);
}
