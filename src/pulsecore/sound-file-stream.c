/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sndfile.h>

#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/log.h>

#include "sound-file-stream.h"

#define BUF_SIZE (1024*10)

struct userdata {
    SNDFILE *sndfile;
    pa_sink_input *sink_input;
    pa_memchunk memchunk;
    sf_count_t (*readf_function)(SNDFILE *sndfile, void *ptr, sf_count_t frames);
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

static void sink_input_kill(pa_sink_input *i) {
    assert(i && i->userdata);
    free_userdata(i->userdata);
}

static int sink_input_peek(pa_sink_input *i, pa_memchunk *chunk) {
    struct userdata *u;
    assert(i && chunk && i->userdata);
    u = i->userdata;

    if (!u->memchunk.memblock) {
        uint32_t fs = pa_frame_size(&i->sample_spec);
        sf_count_t n;

        u->memchunk.memblock = pa_memblock_new(i->sink->core->mempool, BUF_SIZE);
        u->memchunk.index = 0;

        if (u->readf_function) {
            if ((n = u->readf_function(u->sndfile, u->memchunk.memblock->data, BUF_SIZE/fs)) <= 0)
                n = 0;

            u->memchunk.length = n * fs;
        } else {
            if ((n = sf_read_raw(u->sndfile, u->memchunk.memblock->data, BUF_SIZE)) <= 0)
                n = 0;

            u->memchunk.length = n;
        }

        if (!u->memchunk.length) {
            free_userdata(u);
            return -1;
        }
    }

    *chunk = u->memchunk;
    pa_memblock_ref(chunk->memblock);
    assert(chunk->length);
    return 0;
}

static void sink_input_drop(pa_sink_input *i, const pa_memchunk*chunk, size_t length) {
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

int pa_play_file(
        pa_sink *sink,
        const char *fname,
        const pa_cvolume *volume) {

    struct userdata *u = NULL;
    SF_INFO sfinfo;
    pa_sample_spec ss;
    pa_sink_input_new_data data;

    assert(sink);
    assert(fname);

    u = pa_xnew(struct userdata, 1);
    u->sink_input = NULL;
    u->memchunk.memblock = NULL;
    u->memchunk.index = u->memchunk.length = 0;
    u->sndfile = NULL;

    memset(&sfinfo, 0, sizeof(sfinfo));

    if (!(u->sndfile = sf_open(fname, SFM_READ, &sfinfo))) {
        pa_log("Failed to open file %s", fname);
        goto fail;
    }

    u->readf_function = NULL;

    switch (sfinfo.format & 0xFF) {
        case SF_FORMAT_PCM_16:
        case SF_FORMAT_PCM_U8:
        case SF_FORMAT_PCM_S8:
            ss.format = PA_SAMPLE_S16NE;
            u->readf_function = (sf_count_t (*)(SNDFILE *sndfile, void *ptr, sf_count_t frames)) sf_readf_short;
            break;

        case SF_FORMAT_ULAW:
            ss.format = PA_SAMPLE_ULAW;
            break;

        case SF_FORMAT_ALAW:
            ss.format = PA_SAMPLE_ALAW;
            break;

        case SF_FORMAT_FLOAT:
        default:
            ss.format = PA_SAMPLE_FLOAT32NE;
            u->readf_function = (sf_count_t (*)(SNDFILE *sndfile, void *ptr, sf_count_t frames)) sf_readf_float;
            break;
    }

    ss.rate = sfinfo.samplerate;
    ss.channels = sfinfo.channels;

    if (!pa_sample_spec_valid(&ss)) {
        pa_log("Unsupported sample format in file %s", fname);
        goto fail;
    }

    pa_sink_input_new_data_init(&data);
    data.sink = sink;
    data.driver = __FILE__;
    data.name = fname;
    pa_sink_input_new_data_set_sample_spec(&data, &ss);
    pa_sink_input_new_data_set_volume(&data, volume);

    if (!(u->sink_input = pa_sink_input_new(sink->core, &data, 0)))
        goto fail;

    u->sink_input->peek = sink_input_peek;
    u->sink_input->drop = sink_input_drop;
    u->sink_input->kill = sink_input_kill;
    u->sink_input->userdata = u;

    pa_sink_notify(u->sink_input->sink);

    return 0;

fail:
    if (u)
        free_userdata(u);

    return -1;
}
