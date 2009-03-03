/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulse/gccmacro.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/play-memblockq.h>

#include "play-memchunk.h"

int pa_play_memchunk(
        pa_sink *sink,
        const pa_sample_spec *ss,
        const pa_channel_map *map,
        const pa_memchunk *chunk,
        pa_cvolume *volume,
        pa_proplist *p,
        uint32_t *sink_input_index) {

    pa_memblockq *q;
    int r;

    pa_assert(sink);
    pa_assert(ss);
    pa_assert(chunk);

    q = pa_memblockq_new(0, chunk->length, 0, pa_frame_size(ss), 1, 1, 0, NULL);
    pa_assert_se(pa_memblockq_push(q, chunk) >= 0);

    if ((r = pa_play_memblockq(sink, ss, map, q, volume, p, sink_input_index)) < 0) {
        pa_memblockq_free(q);
        return r;
    }

    return 0;
}
