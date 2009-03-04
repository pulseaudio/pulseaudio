/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sndfile.h>

#include <pulse/sample.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>

#include "sound-file.h"
#include "core-scache.h"

int pa_sound_file_load(
        pa_mempool *pool,
        const char *fname,
        pa_sample_spec *ss,
        pa_channel_map *map,
        pa_memchunk *chunk) {

    SNDFILE *sf = NULL;
    SF_INFO sfinfo;
    int ret = -1;
    size_t l;
    sf_count_t (*readf_function)(SNDFILE *sndfile, void *ptr, sf_count_t frames) = NULL;
    void *ptr = NULL;
    int fd;

    pa_assert(fname);
    pa_assert(ss);
    pa_assert(chunk);

    pa_memchunk_reset(chunk);
    memset(&sfinfo, 0, sizeof(sfinfo));

    if ((fd = open(fname, O_RDONLY
#ifdef O_NOCTTY
                   |O_NOCTTY
#endif
                   )) < 0) {
        pa_log("Failed to open file %s: %s", fname, pa_cstrerror(errno));
        goto finish;
    }

#ifdef HAVE_POSIX_FADVISE
    if (posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) < 0) {
        pa_log_warn("POSIX_FADV_SEQUENTIAL failed: %s", pa_cstrerror(errno));
        goto finish;
    } else
        pa_log_debug("POSIX_FADV_SEQUENTIAL succeeded.");
#endif

    if (!(sf = sf_open_fd(fd, SFM_READ, &sfinfo, 1))) {
        pa_log("Failed to open file %s", fname);
        pa_close(fd);
        goto finish;
    }

    switch (sfinfo.format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_16:
        case SF_FORMAT_PCM_U8:
        case SF_FORMAT_PCM_S8:
            ss->format = PA_SAMPLE_S16NE;
            readf_function = (sf_count_t (*)(SNDFILE *sndfile, void *_ptr, sf_count_t frames)) sf_readf_short;
            break;

        case SF_FORMAT_ULAW:
            ss->format = PA_SAMPLE_ULAW;
            break;

        case SF_FORMAT_ALAW:
            ss->format = PA_SAMPLE_ALAW;
            break;

        case SF_FORMAT_FLOAT:
        case SF_FORMAT_DOUBLE:
        default:
            ss->format = PA_SAMPLE_FLOAT32NE;
            readf_function = (sf_count_t (*)(SNDFILE *sndfile, void *_ptr, sf_count_t frames)) sf_readf_float;
            break;
    }

    ss->rate = (uint32_t) sfinfo.samplerate;
    ss->channels = (uint8_t) sfinfo.channels;

    if (!pa_sample_spec_valid(ss)) {
        pa_log("Unsupported sample format in file %s", fname);
        goto finish;
    }

    if (map)
        pa_channel_map_init_extend(map, ss->channels, PA_CHANNEL_MAP_DEFAULT);

    if ((l = pa_frame_size(ss) * (size_t) sfinfo.frames) > PA_SCACHE_ENTRY_SIZE_MAX) {
        pa_log("File too large");
        goto finish;
    }

    chunk->memblock = pa_memblock_new(pool, l);
    chunk->index = 0;
    chunk->length = l;

    ptr = pa_memblock_acquire(chunk->memblock);

    if ((readf_function && readf_function(sf, ptr, sfinfo.frames) != sfinfo.frames) ||
        (!readf_function && sf_read_raw(sf, ptr, (sf_count_t) l) != (sf_count_t) l)) {
        pa_log("Premature file end");
        goto finish;
    }

    ret = 0;

finish:

    if (sf)
        sf_close(sf);

    if (ptr)
        pa_memblock_release(chunk->memblock);

    if (ret != 0 && chunk->memblock)
        pa_memblock_unref(chunk->memblock);

    return ret;
}

int pa_sound_file_too_big_to_cache(const char *fname) {

    SNDFILE*sf = NULL;
    SF_INFO sfinfo;
    pa_sample_spec ss;

    pa_assert(fname);

    if (!(sf = sf_open(fname, SFM_READ, &sfinfo))) {
        pa_log("Failed to open file %s", fname);
        return -1;
    }

    sf_close(sf);

    switch (sfinfo.format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_16:
        case SF_FORMAT_PCM_U8:
        case SF_FORMAT_PCM_S8:
            ss.format = PA_SAMPLE_S16NE;
            break;

        case SF_FORMAT_ULAW:
            ss.format = PA_SAMPLE_ULAW;
            break;

        case SF_FORMAT_ALAW:
            ss.format = PA_SAMPLE_ALAW;
            break;

        case SF_FORMAT_DOUBLE:
        case SF_FORMAT_FLOAT:
        default:
            ss.format = PA_SAMPLE_FLOAT32NE;
            break;
    }

    ss.rate = (uint32_t) sfinfo.samplerate;
    ss.channels = (uint8_t) sfinfo.channels;

    if (!pa_sample_spec_valid(&ss)) {
        pa_log("Unsupported sample format in file %s", fname);
        return -1;
    }

    if ((pa_frame_size(&ss) * (size_t) sfinfo.frames) > PA_SCACHE_ENTRY_SIZE_MAX) {
        pa_log("File too large: %s", fname);
        return 1;
    }

    return 0;
}
