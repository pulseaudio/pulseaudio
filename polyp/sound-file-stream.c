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
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sndfile.h>

#include "sound-file-stream.h"
#include "sink-input.h"
#include "xmalloc.h"
#include "log.h"

#define BUF_SIZE (1024*10)

struct userdata {
    SNDFILE *sndfile;
    struct pa_sink_input *sink_input;
    struct pa_memchunk memchunk;
};

static void free_userdata(struct userdata *u) {
    assert(u);
    if (u->sink_input) {
        pa_sink_input_disconnect(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }
    
    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);
    if (u->sndfile)
        sf_close(u->sndfile);

    pa_xfree(u);
}

static void sink_input_kill(struct pa_sink_input *i) {
    assert(i && i->userdata);
    free_userdata(i->userdata);
}

static void si_kill(struct pa_mainloop_api *m, void *i) {
    sink_input_kill(i);
}

static int sink_input_peek(struct pa_sink_input *i, struct pa_memchunk *chunk) {
    struct userdata *u;
    assert(i && chunk && i->userdata);
    u = i->userdata;

    if (!u->memchunk.memblock) {
        uint32_t fs = pa_frame_size(&i->sample_spec);
        sf_count_t samples = BUF_SIZE/fs;

        u->memchunk.memblock = pa_memblock_new(BUF_SIZE, i->sink->core->memblock_stat);
        u->memchunk.index = 0;
        samples = sf_readf_float(u->sndfile, u->memchunk.memblock->data, samples);
        u->memchunk.length = samples*fs;
        
        if (!u->memchunk.length) {
            pa_memblock_unref(u->memchunk.memblock);
            u->memchunk.memblock = NULL;
            u->memchunk.index = u->memchunk.length = 0;
            pa_mainloop_api_once(i->sink->core->mainloop, si_kill, i);
            return -1;
        }
    }

    *chunk = u->memchunk;
    pa_memblock_ref(chunk->memblock);
    assert(chunk->length);
    return 0;
}

static void sink_input_drop(struct pa_sink_input *i, const struct pa_memchunk*chunk, size_t length) {
    struct userdata *u;
    assert(i && chunk && length && i->userdata);
    u = i->userdata;

    assert(!memcmp(chunk, &u->memchunk, sizeof(chunk)));
    assert(length <= u->memchunk.length);

    u->memchunk.index += length;
    u->memchunk.length -= length;

    if (u->memchunk.length <= 0) {
        pa_memblock_unref(u->memchunk.memblock);
        u->memchunk.memblock = NULL;
        u->memchunk.index = u->memchunk.length = 0;
    }
}

int pa_play_file(struct pa_sink *sink, const char *fname, pa_volume_t volume) {
    struct userdata *u = NULL;
    SF_INFO sfinfo;
    struct pa_sample_spec ss;
    assert(sink && fname);

    if (volume <= 0)
        goto fail;

    u = pa_xmalloc(sizeof(struct userdata));
    u->sink_input = NULL;
    u->memchunk.memblock = NULL;
    u->memchunk.index = u->memchunk.length = 0;
    u->sndfile = NULL;

    memset(&sfinfo, 0, sizeof(sfinfo));

    if (!(u->sndfile = sf_open(fname, SFM_READ, &sfinfo))) {
        pa_log(__FILE__": Failed to open file %s\n", fname);
        goto fail;
    }

    ss.format = PA_SAMPLE_FLOAT32;
    ss.rate = sfinfo.samplerate;
    ss.channels = sfinfo.channels;

    if (!pa_sample_spec_valid(&ss)) {
        pa_log(__FILE__": Unsupported sample format in file %s\n", fname);
        goto fail;
    }
    
    if (!(u->sink_input = pa_sink_input_new(sink, fname, &ss, 0, -1)))
        goto fail;

    u->sink_input->volume = volume;
    u->sink_input->peek = sink_input_peek;
    u->sink_input->drop = sink_input_drop;
    u->sink_input->kill = sink_input_kill;
    u->sink_input->userdata = u;
    
    pa_sink_notify(sink);

    return 0;

fail:
    if (u)
        free_userdata(u);
    
    return -1;
}
