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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sndfile.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/log.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/core-util.h>

#include "sound-file-stream.h"

typedef struct file_stream {
    pa_msgobject parent;
    pa_core *core;
    SNDFILE *sndfile;
    pa_sink_input *sink_input;
    pa_memchunk memchunk;
    sf_count_t (*readf_function)(SNDFILE *sndfile, void *ptr, sf_count_t frames);
    size_t drop;
} file_stream;

enum {
    FILE_STREAM_MESSAGE_UNLINK
};

PA_DECLARE_CLASS(file_stream);
#define FILE_STREAM(o) (file_stream_cast(o))
static PA_DEFINE_CHECK_TYPE(file_stream, pa_msgobject);

static void file_stream_unlink(file_stream *u) {
    pa_assert(u);

    if (!u->sink_input)
        return;

    pa_sink_input_unlink(u->sink_input);

    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    /* Make sure we don't decrease the ref count twice. */
    file_stream_unref(u);
}

static void file_stream_free(pa_object *o) {
    file_stream *u = FILE_STREAM(o);
    pa_assert(u);

    file_stream_unlink(u);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->sndfile)
        sf_close(u->sndfile);

    pa_xfree(u);
}

static int file_stream_process_msg(pa_msgobject *o, int code, void*userdata, int64_t offset, pa_memchunk *chunk) {
    file_stream *u = FILE_STREAM(o);
    file_stream_assert_ref(u);

    switch (code) {
        case FILE_STREAM_MESSAGE_UNLINK:
            file_stream_unlink(u);
            break;
    }

    return 0;
}

static void sink_input_kill_cb(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    file_stream_unlink(FILE_STREAM(i->userdata));
}

static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    file_stream *u;

    pa_assert(i);
    pa_assert(chunk);
    u = FILE_STREAM(i->userdata);
    file_stream_assert_ref(u);

    if (!u->sndfile)
        return -1;

    for (;;) {

        if (!u->memchunk.memblock) {

            u->memchunk.memblock = pa_memblock_new(i->sink->core->mempool, length);
            u->memchunk.index = 0;

            if (u->readf_function) {
                sf_count_t n;
                void *p;
                size_t fs = pa_frame_size(&i->sample_spec);

                p = pa_memblock_acquire(u->memchunk.memblock);
                n = u->readf_function(u->sndfile, p, length/fs);
                pa_memblock_release(u->memchunk.memblock);

                if (n <= 0)
                    n = 0;

                u->memchunk.length = n * fs;
            } else {
                sf_count_t n;
                void *p;

                p = pa_memblock_acquire(u->memchunk.memblock);
                n = sf_read_raw(u->sndfile, p, length);
                pa_memblock_release(u->memchunk.memblock);

                if (n <= 0)
                    n = 0;

                u->memchunk.length = n;
            }

            if (u->memchunk.length <= 0) {

                pa_memblock_unref(u->memchunk.memblock);
                pa_memchunk_reset(&u->memchunk);

                pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(u), FILE_STREAM_MESSAGE_UNLINK, NULL, 0, NULL, NULL);

                sf_close(u->sndfile);
                u->sndfile = NULL;

                return -1;
            }
        }

        pa_assert(u->memchunk.memblock);
        pa_assert(u->memchunk.length > 0);

        if (u->drop < u->memchunk.length) {
            u->memchunk.index += u->drop;
            u->memchunk.length -= u->drop;
            u->drop = 0;
            break;
        }

        u->drop -= u->memchunk.length;
        pa_memblock_unref(u->memchunk.memblock);
        pa_memchunk_reset(&u->memchunk);
    }

    *chunk = u->memchunk;
    pa_memblock_ref(chunk->memblock);

    pa_assert(chunk->length > 0);
    pa_assert(u->drop <= 0);

    return 0;
}

static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    file_stream *u;

    pa_assert(i);
    pa_assert(length > 0);
    u = FILE_STREAM(i->userdata);
    file_stream_assert_ref(u);

    if (u->memchunk.memblock) {

        if (length < u->memchunk.length) {
            u->memchunk.index += length;
            u->memchunk.length -= length;
            return;
        }

        length -= u->memchunk.length;
        pa_memblock_unref(u->memchunk.memblock);
        pa_memchunk_reset(&u->memchunk);
    }

    u->drop += length;
}

int pa_play_file(
        pa_sink *sink,
        const char *fname,
        const pa_cvolume *volume) {

    file_stream *u = NULL;
    SF_INFO sfinfo;
    pa_sample_spec ss;
    pa_sink_input_new_data data;
    int fd;

    pa_assert(sink);
    pa_assert(fname);

    u = pa_msgobject_new(file_stream);
    u->parent.parent.free = file_stream_free;
    u->parent.process_msg = file_stream_process_msg;
    u->core = sink->core;
    u->sink_input = NULL;
    pa_memchunk_reset(&u->memchunk);
    u->sndfile = NULL;
    u->readf_function = NULL;
    u->drop = 0;

    memset(&sfinfo, 0, sizeof(sfinfo));

    if ((fd = open(fname, O_RDONLY
#ifdef O_NOCTTY
                   |O_NOCTTY
#endif
                   )) < 0) {
        pa_log("Failed to open file %s: %s", fname, pa_cstrerror(errno));
        goto fail;
    }

    /* FIXME: For now we just use posix_fadvise to avoid page faults
     * when accessing the file data. Eventually we should move the
     * file reader into the main event loop and pass the data over the
     * asyncmsgq. */

#ifdef HAVE_POSIX_FADVISE
    if (posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) < 0) {
        pa_log_warn("POSIX_FADV_SEQUENTIAL failed: %s", pa_cstrerror(errno));
        goto fail;
    } else
        pa_log_debug("POSIX_FADV_SEQUENTIAL succeeded.");

    if (posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED) < 0) {
        pa_log_warn("POSIX_FADV_WILLNEED failed: %s", pa_cstrerror(errno));
        goto fail;
    } else
        pa_log_debug("POSIX_FADV_WILLNEED succeeded.");
#endif

    if (!(u->sndfile = sf_open_fd(fd, SFM_READ, &sfinfo, 1))) {
        pa_log("Failed to open file %s", fname);
        pa_close(fd);
        goto fail;
    }

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

    u->sink_input->peek = sink_input_peek_cb;
    u->sink_input->drop = sink_input_drop_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->userdata = u;

    pa_sink_input_put(u->sink_input);

    /* The reference to u is dangling here, because we want to keep
     * this stream around until it is fully played. */

    return 0;

fail:
    if (u)
        file_stream_unref(u);

    return -1;
}
