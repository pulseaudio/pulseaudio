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

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "play-memchunk.h"
#include "sink-input.h"
#include "xmalloc.h"
#include "gccmacro.h"

static void sink_input_kill(pa_sink_input *i) {
    pa_memchunk *c;
    assert(i && i->userdata);
    c = i->userdata;

    pa_sink_input_disconnect(i);
    pa_sink_input_unref(i);

    pa_memblock_unref(c->memblock);
    pa_xfree(c);
}

static int sink_input_peek(pa_sink_input *i, pa_memchunk *chunk) {
    pa_memchunk *c;
    assert(i && chunk && i->userdata);
    c = i->userdata;

    if (c->length <= 0)
        return -1;
    
    assert(c->memblock && c->memblock->length);
    *chunk = *c;
    pa_memblock_ref(c->memblock);

    return 0;
}

static void si_kill(PA_GCC_UNUSED pa_mainloop_api *m, void *i) {
    sink_input_kill(i);
}

static void sink_input_drop(pa_sink_input *i, const pa_memchunk*chunk, size_t length) {
    pa_memchunk *c;
    assert(i && length && i->userdata);
    c = i->userdata;

    assert(!memcmp(chunk, c, sizeof(chunk)));
    assert(length <= c->length);

    c->length -= length;
    c->index += length;

    if (c->length <= 0)
        pa_mainloop_api_once(i->sink->core->mainloop, si_kill, i);
}

int pa_play_memchunk(
    pa_sink *sink,
    const char *name,
    const pa_sample_spec *ss,
    const pa_channel_map *map,
    const pa_memchunk *chunk,
    pa_cvolume *cvolume) {
    
    pa_sink_input *si;
    pa_memchunk *nchunk;

    assert(sink);
    assert(ss);
    assert(chunk);

    if (cvolume && pa_cvolume_is_muted(cvolume))
        return 0;

    if (!(si = pa_sink_input_new(sink, name, __FILE__, ss, map, 0, PA_RESAMPLER_INVALID)))
        return -1;

    if (cvolume)
        si->volume = *cvolume;
    
    si->peek = sink_input_peek;
    si->drop = sink_input_drop;
    si->kill = sink_input_kill;
    
    si->userdata = nchunk = pa_xnew(pa_memchunk, 1);
    *nchunk = *chunk;
    
    pa_memblock_ref(chunk->memblock);

    pa_sink_notify(sink);
    
    return 0;
}
