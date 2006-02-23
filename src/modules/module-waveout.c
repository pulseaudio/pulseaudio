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

#include <windows.h>
#include <mmsystem.h>
#include <assert.h>

#include <polyp/mainloop-api.h>

#include <polypcore/sink.h>
#include <polypcore/source.h>
#include <polypcore/module.h>
#include <polypcore/modargs.h>
#include <polypcore/sample-util.h>
#include <polypcore/util.h>
#include <polypcore/log.h>
#include <polypcore/xmalloc.h>

#include "module-waveout-symdef.h"

PA_MODULE_AUTHOR("Pierre Ossman")
PA_MODULE_DESCRIPTION("Windows waveOut Sink/Source")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("sink_name=<name for the sink> source_name=<name for the source> record=<enable source?> playback=<enable sink?> format=<sample format> channels=<number of channels> rate=<sample rate> fragments=<number of fragments> fragment_size=<fragment size>")

#define DEFAULT_SINK_NAME "wave_output"
#define DEFAULT_SOURCE_NAME "wave_input"

#define WAVEOUT_MAX_VOLUME 0xFFFF

struct userdata {
    pa_sink *sink;
    pa_source *source;
    pa_core *core;
    pa_time_event *event;
    pa_defer_event *defer;
    pa_usec_t poll_timeout;

    uint32_t fragments, fragment_size;

    uint32_t free_ofrags, free_ifrags;

    DWORD written_bytes;

    int cur_ohdr, cur_ihdr;
    unsigned int oremain;
    WAVEHDR *ohdrs, *ihdrs;
    pa_memchunk silence;

    HWAVEOUT hwo;
    HWAVEIN hwi;
    pa_module *module;

    CRITICAL_SECTION crit;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "source_name",
    "record",
    "playback",
    "fragments",
    "fragment_size",
    "format",
    "rate",
    "channels",
    NULL
};

static void update_usage(struct userdata *u) {
   pa_module_set_used(u->module,
                      (u->sink ? pa_idxset_size(u->sink->inputs) : 0) +
                      (u->sink ? pa_idxset_size(u->sink->monitor_source->outputs) : 0) +
                      (u->source ? pa_idxset_size(u->source->outputs) : 0));
}

static void do_write(struct userdata *u)
{
    uint32_t free_frags, remain;
    pa_memchunk memchunk, *cur_chunk;
    WAVEHDR *hdr;
    MMRESULT res;

    if (!u->sink)
        return;

    EnterCriticalSection(&u->crit);

    free_frags = u->free_ofrags;
    u->free_ofrags = 0;

    LeaveCriticalSection(&u->crit);

    while (free_frags) {
        hdr = &u->ohdrs[u->cur_ohdr];
        if (hdr->dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(u->hwo, hdr, sizeof(WAVEHDR));

        remain = u->oremain;
        while (remain) {
            cur_chunk = &memchunk;

            if (pa_sink_render(u->sink, remain, cur_chunk) < 0) {
                /*
                 * Don't fill with silence unless we're getting close to
                 * underflowing.
                 */
                if (free_frags > u->fragments/2)
                    cur_chunk = &u->silence;
                else {
                    EnterCriticalSection(&u->crit);

                    u->free_ofrags += free_frags;

                    LeaveCriticalSection(&u->crit);

                    u->oremain = remain;
                    return;
                }
            }

            assert(cur_chunk->memblock);
            assert(cur_chunk->memblock->data);
            assert(cur_chunk->length);

            memcpy(hdr->lpData + u->fragment_size - remain,
                (char*)cur_chunk->memblock->data + cur_chunk->index,
                (cur_chunk->length < remain)?cur_chunk->length:remain);

            remain -= (cur_chunk->length < remain)?cur_chunk->length:remain;

            if (cur_chunk != &u->silence) {
                pa_memblock_unref(cur_chunk->memblock);
                cur_chunk->memblock = NULL;
            }
        }

        res = waveOutPrepareHeader(u->hwo, hdr, sizeof(WAVEHDR));
        if (res != MMSYSERR_NOERROR) {
            pa_log_error(__FILE__ ": ERROR: Unable to prepare waveOut block: %d",
                res);
        }
        res = waveOutWrite(u->hwo, hdr, sizeof(WAVEHDR));
        if (res != MMSYSERR_NOERROR) {
            pa_log_error(__FILE__ ": ERROR: Unable to write waveOut block: %d",
                res);
        }
        
        u->written_bytes += u->fragment_size;

        free_frags--;
        u->cur_ohdr++;
        u->cur_ohdr %= u->fragments;
        u->oremain = u->fragment_size;
    }
}

static void do_read(struct userdata *u)
{
    uint32_t free_frags;
    pa_memchunk memchunk;
    WAVEHDR *hdr;
    MMRESULT res;

    if (!u->source)
        return;

    EnterCriticalSection(&u->crit);

    free_frags = u->free_ifrags;
    u->free_ifrags = 0;

    LeaveCriticalSection(&u->crit);

    while (free_frags) {
        hdr = &u->ihdrs[u->cur_ihdr];
        if (hdr->dwFlags & WHDR_PREPARED)
            waveInUnprepareHeader(u->hwi, hdr, sizeof(WAVEHDR));

        if (hdr->dwBytesRecorded) {
            memchunk.memblock = pa_memblock_new(hdr->dwBytesRecorded, u->core->memblock_stat);
            assert(memchunk.memblock);

            memcpy((char*)memchunk.memblock->data, hdr->lpData, hdr->dwBytesRecorded);

            memchunk.length = memchunk.memblock->length = hdr->dwBytesRecorded;
            memchunk.index = 0;

            pa_source_post(u->source, &memchunk);
            pa_memblock_unref(memchunk.memblock);
        }

        res = waveInPrepareHeader(u->hwi, hdr, sizeof(WAVEHDR));
        if (res != MMSYSERR_NOERROR) {
            pa_log_error(__FILE__ ": ERROR: Unable to prepare waveIn block: %d",
                res);
        }
        res = waveInAddBuffer(u->hwi, hdr, sizeof(WAVEHDR));
        if (res != MMSYSERR_NOERROR) {
            pa_log_error(__FILE__ ": ERROR: Unable to add waveIn block: %d",
                res);
        }
        
        free_frags--;
        u->cur_ihdr++;
        u->cur_ihdr %= u->fragments;
    }
}

static void poll_cb(pa_mainloop_api*a, pa_time_event *e, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval ntv;

    assert(u);

    update_usage(u);

    do_write(u);
    do_read(u);

    pa_gettimeofday(&ntv);
    pa_timeval_add(&ntv, u->poll_timeout);

    a->time_restart(e, &ntv);
}

static void defer_cb(pa_mainloop_api*a, pa_defer_event *e, void *userdata) {
    struct userdata *u = userdata;

    assert(u);

    a->defer_enable(e, 0);

    do_write(u);
    do_read(u);
}

static void CALLBACK chunk_done_cb(HWAVEOUT hwo, UINT msg, DWORD_PTR inst, DWORD param1, DWORD param2) {
    struct userdata *u = (struct userdata *)inst;

    if (msg != WOM_DONE)
        return;

    EnterCriticalSection(&u->crit);

    u->free_ofrags++;
    assert(u->free_ofrags <= u->fragments);

    LeaveCriticalSection(&u->crit);
}

static void CALLBACK chunk_ready_cb(HWAVEIN hwi, UINT msg, DWORD_PTR inst, DWORD param1, DWORD param2) {
    struct userdata *u = (struct userdata *)inst;

    if (msg != WIM_DATA)
        return;

    EnterCriticalSection(&u->crit);

    u->free_ifrags++;
    assert(u->free_ifrags <= u->fragments);

    LeaveCriticalSection(&u->crit);
}

static pa_usec_t sink_get_latency_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    uint32_t free_frags;
    MMTIME mmt;
    assert(s && u && u->sink);

    memset(&mmt, 0, sizeof(mmt));
    mmt.wType = TIME_BYTES;
    if (waveOutGetPosition(u->hwo, &mmt, sizeof(mmt)) == MMSYSERR_NOERROR)
        return pa_bytes_to_usec(u->written_bytes - mmt.u.cb, &s->sample_spec);
    else {
        EnterCriticalSection(&u->crit);

        free_frags = u->free_ofrags;

        LeaveCriticalSection(&u->crit);

        return pa_bytes_to_usec((u->fragments - free_frags) * u->fragment_size,
                              &s->sample_spec);
    }
}

static pa_usec_t source_get_latency_cb(pa_source *s) {
    pa_usec_t r = 0;
    struct userdata *u = s->userdata;
    uint32_t free_frags;
    assert(s && u && u->sink);

    EnterCriticalSection(&u->crit);

    free_frags = u->free_ifrags;

    LeaveCriticalSection(&u->crit);

    r += pa_bytes_to_usec((free_frags + 1) * u->fragment_size, &s->sample_spec);

    return r;
}

static void notify_sink_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    assert(u);

    u->core->mainloop->defer_enable(u->defer, 1);
}

static void notify_source_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    assert(u);

    u->core->mainloop->defer_enable(u->defer, 1);
}

static int sink_get_hw_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    DWORD vol;
    pa_volume_t left, right;

    if (waveOutGetVolume(u->hwo, &vol) != MMSYSERR_NOERROR)
        return -1;

    left = (vol & 0xFFFF) * PA_VOLUME_NORM / WAVEOUT_MAX_VOLUME;
    right = ((vol >> 16) & 0xFFFF) * PA_VOLUME_NORM / WAVEOUT_MAX_VOLUME;

    /* Windows supports > 2 channels, except for volume control */
    if (s->hw_volume.channels > 2)
        pa_cvolume_set(&s->hw_volume, s->hw_volume.channels, (left + right)/2);

    s->hw_volume.values[0] = left;
    if (s->hw_volume.channels > 1)
        s->hw_volume.values[1] = right;

    return 0;
}

static int sink_set_hw_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    DWORD vol;

    vol = s->hw_volume.values[0] * WAVEOUT_MAX_VOLUME / PA_VOLUME_NORM;
    if (s->hw_volume.channels > 1)
        vol |= (s->hw_volume.values[0] * WAVEOUT_MAX_VOLUME / PA_VOLUME_NORM) << 16;

    if (waveOutSetVolume(u->hwo, vol) != MMSYSERR_NOERROR)
        return -1;

    return 0;
}

static int ss_to_waveformat(pa_sample_spec *ss, LPWAVEFORMATEX wf) {
    wf->wFormatTag = WAVE_FORMAT_PCM;

    if (ss->channels > 2) {
        pa_log_error(__FILE__": ERROR: More than two channels not supported.");
        return -1;
    }

    wf->nChannels = ss->channels;

    switch (ss->rate) {
    case 8000:
    case 11025:
    case 22005:
    case 44100:
        break;
    default:
        pa_log_error(__FILE__": ERROR: Unsupported sample rate.");
        return -1;
    }

    wf->nSamplesPerSec = ss->rate;

    if (ss->format == PA_SAMPLE_U8)
        wf->wBitsPerSample = 8;
    else if (ss->format == PA_SAMPLE_S16NE)
        wf->wBitsPerSample = 16;
    else {
        pa_log_error(__FILE__": ERROR: Unsupported sample format.");
        return -1;
    }

    wf->nBlockAlign = wf->nChannels * wf->wBitsPerSample/8;
    wf->nAvgBytesPerSec = wf->nSamplesPerSec * wf->nBlockAlign;

    wf->cbSize = 0;

    return 0;
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u = NULL;
    HWAVEOUT hwo = INVALID_HANDLE_VALUE;
    HWAVEIN hwi = INVALID_HANDLE_VALUE;
    WAVEFORMATEX wf;
    int nfrags, frag_size;
    int record = 1, playback = 1;
    pa_sample_spec ss;
    pa_modargs *ma = NULL;
    unsigned int i;
    struct timeval tv;

    assert(c && m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "record", &record) < 0 || pa_modargs_get_value_boolean(ma, "playback", &playback) < 0) {
        pa_log(__FILE__": record= and playback= expect boolean argument.");
        goto fail;
    }

    if (!playback && !record) {
        pa_log(__FILE__": neither playback nor record enabled for device.");
        goto fail;
    }

    nfrags = 20;
    frag_size = 1024;
    if (pa_modargs_get_value_s32(ma, "fragments", &nfrags) < 0 || pa_modargs_get_value_s32(ma, "fragment_size", &frag_size) < 0) {
        pa_log(__FILE__": failed to parse fragments arguments");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": failed to parse sample specification");
        goto fail;
    }

    if (ss_to_waveformat(&ss, &wf) < 0)
        goto fail;

    u = pa_xmalloc(sizeof(struct userdata));

    if (record) {
        if (waveInOpen(&hwi, WAVE_MAPPER, &wf, (DWORD_PTR)chunk_ready_cb, (DWORD_PTR)u, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
            goto fail;
        if (waveInStart(hwi) != MMSYSERR_NOERROR)
            goto fail;
        pa_log_debug(__FILE__": Opened waveIn subsystem.");
    }

    if (playback) {
        if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, (DWORD_PTR)chunk_done_cb, (DWORD_PTR)u, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
            goto fail;
        pa_log_debug(__FILE__": Opened waveOut subsystem.");
    }

    InitializeCriticalSection(&u->crit);

    if (hwi != INVALID_HANDLE_VALUE) {
        u->source = pa_source_new(c, __FILE__, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss, NULL);
        assert(u->source);
        u->source->userdata = u;
        u->source->notify = notify_source_cb;
        u->source->get_latency = source_get_latency_cb;
        pa_source_set_owner(u->source, m);
        u->source->description = pa_sprintf_malloc("Windows waveIn PCM");
    } else
        u->source = NULL;

    if (hwo != INVALID_HANDLE_VALUE) {
        u->sink = pa_sink_new(c, __FILE__, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME), 0, &ss, NULL);
        assert(u->sink);
        u->sink->notify = notify_sink_cb;
        u->sink->get_latency = sink_get_latency_cb;
        u->sink->get_hw_volume = sink_get_hw_volume_cb;
        u->sink->set_hw_volume = sink_set_hw_volume_cb;
        u->sink->userdata = u;
        pa_sink_set_owner(u->sink, m);
        u->sink->description = pa_sprintf_malloc("Windows waveOut PCM");
    } else
        u->sink = NULL;

    assert(u->source || u->sink);

    u->core = c;
    u->hwi = hwi;
    u->hwo = hwo;

    u->fragments = nfrags;
    u->free_ifrags = u->fragments;
    u->free_ofrags = u->fragments;
    u->fragment_size = frag_size - (frag_size % pa_frame_size(&ss));

    u->written_bytes = 0;

    u->oremain = u->fragment_size;

    u->poll_timeout = pa_bytes_to_usec(u->fragments * u->fragment_size / 3, &ss);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, u->poll_timeout);

    u->event = c->mainloop->time_new(c->mainloop, &tv, poll_cb, u);
    assert(u->event);

    u->defer = c->mainloop->defer_new(c->mainloop, defer_cb, u);
    assert(u->defer);
    c->mainloop->defer_enable(u->defer, 0);

    u->cur_ihdr = 0;
    u->cur_ohdr = 0;
    u->ihdrs = pa_xmalloc0(sizeof(WAVEHDR) * u->fragments);
    assert(u->ihdrs);
    u->ohdrs = pa_xmalloc0(sizeof(WAVEHDR) * u->fragments);
    assert(u->ohdrs);
    for (i = 0;i < u->fragments;i++) {
        u->ihdrs[i].dwBufferLength = u->fragment_size;
        u->ohdrs[i].dwBufferLength = u->fragment_size;
        u->ihdrs[i].lpData = pa_xmalloc(u->fragment_size);
        assert(u->ihdrs);
        u->ohdrs[i].lpData = pa_xmalloc(u->fragment_size);
        assert(u->ohdrs);
    }
    
    u->silence.length = u->fragment_size;
    u->silence.memblock = pa_memblock_new(u->silence.length, u->core->memblock_stat);
    assert(u->silence.memblock);
    pa_silence_memblock(u->silence.memblock, &ss);
    u->silence.index = 0;

    u->module = m;
    m->userdata = u;

    pa_modargs_free(ma);

    return 0;

fail:
   if (hwi != INVALID_HANDLE_VALUE)
        waveInClose(hwi);

   if (hwo != INVALID_HANDLE_VALUE)
        waveOutClose(hwo);

    if (u)
        pa_xfree(u);

    if (ma)
        pa_modargs_free(ma);
    
    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    unsigned int i;

    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    if (u->event)
        c->mainloop->time_free(u->event);

    if (u->defer)
        c->mainloop->defer_free(u->defer);

    if (u->sink) {
        pa_sink_disconnect(u->sink);
        pa_sink_unref(u->sink);
    }
    
    if (u->source) {
        pa_source_disconnect(u->source);
        pa_source_unref(u->source);
    }
    
    if (u->hwi != INVALID_HANDLE_VALUE) {
        waveInReset(u->hwi);
        waveInClose(u->hwi);
    }

    if (u->hwo != INVALID_HANDLE_VALUE) {
        waveOutReset(u->hwo);
        waveOutClose(u->hwo);
    }

    for (i = 0;i < u->fragments;i++) {
        pa_xfree(u->ihdrs[i].lpData);
        pa_xfree(u->ohdrs[i].lpData);
    }

    pa_xfree(u->ihdrs);
    pa_xfree(u->ohdrs);

    DeleteCriticalSection(&u->crit);
    
    pa_xfree(u);
}
