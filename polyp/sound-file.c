#include <string.h>
#include <assert.h>

#include <sndfile.h>

#include "sound-file.h"
#include "sample.h"

#define MAX_FILE_SIZE (1024*1024)

int pa_sound_file_load(const char *fname, struct pa_sample_spec *ss, struct pa_memchunk *chunk) {
    SNDFILE*sf = NULL;
    SF_INFO sfinfo;
    int ret = -1;
    size_t l;
    assert(fname && ss && chunk);

    memset(&sfinfo, 0, sizeof(sfinfo));

    chunk->memblock = NULL;
    chunk->index = chunk->length = 0;
    
    if (!(sf = sf_open(fname, SFM_READ, &sfinfo))) {
        fprintf(stderr, __FILE__": Failed to open file %s\n", fname);
        goto finish;
    }

    ss->format = PA_SAMPLE_FLOAT32;
    ss->rate = sfinfo.samplerate;
    ss->channels = sfinfo.channels;

    if (!pa_sample_spec_valid(ss)) {
        fprintf(stderr, __FILE__": Unsupported sample format in file %s\n", fname);
        goto finish;
    }
    
    if ((l = pa_frame_size(ss)*sfinfo.frames) > MAX_FILE_SIZE) {
        fprintf(stderr, __FILE__": File to large\n");
        goto finish;
    }

    chunk->memblock = pa_memblock_new(l);
    assert(chunk->memblock);
    chunk->index = 0;
    chunk->length = l;

    if (sf_readf_float(sf, chunk->memblock->data, sfinfo.frames) != sfinfo.frames) {
        fprintf(stderr, __FILE__": Premature file end\n");
        goto finish;
    }

    ret = 0;

finish:

    if (sf)
        sf_close(sf);

    if (ret != 0 && chunk->memblock)
        pa_memblock_unref(chunk->memblock);
    
    return ret;
    
}
