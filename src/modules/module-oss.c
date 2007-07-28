/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

/* General power management rules:
 *
 *   When SUSPENDED we close the audio device.
 *
 *   We make no difference between IDLE and RUNNING in our handling.
 *
 *   As long as we are in RUNNING/IDLE state we will *always* write data to
 *   the device. If none is avilable from the inputs, we write silence
 *   instead.
 *
 *   If power should be saved on IDLE this should be implemented in a
 *   special suspend-on-idle module that will put us into SUSPEND mode
 *   as soon and we're idle for too long.
 *
 */

/* TODO: handle restoring of volume after suspend */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#else
#include "poll.h"
#endif

#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/core-error.h>
#include <pulsecore/thread.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "oss-util.h"
#include "module-oss-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("OSS Sink/Source")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "source_name=<name for the source> "
        "device=<OSS device> "
        "record=<enable source?> "
        "playback=<enable sink?> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "channel_map=<channel map> "
        "mmap=<enable memory mapping?>")

#define DEFAULT_DEVICE "/dev/dsp"

#define DEFAULT_NFRAGS 4
#define DEFAULT_FRAGSIZE_MSEC 25

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_source *source;
    pa_thread *thread;
    pa_asyncmsgq *asyncmsgq;

    char *device_name;
    
    pa_memchunk memchunk;

    uint32_t in_fragment_size, out_fragment_size, in_nfrags, out_nfrags, in_hwbuf_size, out_hwbuf_size;
    int use_getospace, use_getispace;
    int use_getodelay;

    int use_pcm_volume;
    int use_input_volume;

    int sink_suspended, source_suspended;

    int fd;
    int mode;

    int nfrags, frag_size;

    int use_mmap;
    unsigned out_mmap_current, in_mmap_current;
    void *in_mmap, *out_mmap;
    pa_memblock **in_mmap_memblocks, **out_mmap_memblocks;

    int in_mmap_saved_nfrags, out_mmap_saved_nfrags;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "source_name",
    "device",
    "record",
    "playback",
    "fragments",
    "fragment_size",
    "format",
    "rate",
    "channels",
    "channel_map",
    "mmap",
    NULL
};

static void trigger(struct userdata *u, int quick) {
    int enable_bits = 0, zero = 0;

/*     pa_log_debug("trigger"); */

    if (u->source && u->source->thread_info.state != PA_SOURCE_SUSPENDED)
        enable_bits |= PCM_ENABLE_INPUT;
    
    if (u->sink && u->sink->thread_info.state != PA_SINK_SUSPENDED)
        enable_bits |= PCM_ENABLE_OUTPUT;
    
    if (u->use_mmap) {

        if (!quick)
            /* First, let's stop all playback, capturing */
            ioctl(u->fd, SNDCTL_DSP_SETTRIGGER, &zero);

#ifdef SNDCTL_DSP_HALT
        if (enable_bits == 0)
            if (ioctl(u->fd, SNDCTL_DSP_HALT, NULL) < 0)
                pa_log_warn("SNDCTL_DSP_HALT: %s", pa_cstrerror(errno));
#endif
        
        if (ioctl(u->fd, SNDCTL_DSP_SETTRIGGER, &enable_bits) < 0)
            pa_log_warn("SNDCTL_DSP_SETTRIGGER: %s", pa_cstrerror(errno));
        
        if (u->sink && !(enable_bits & PCM_ENABLE_OUTPUT)) {
            pa_log_debug("clearing playback buffer");
            pa_silence_memory(u->out_mmap, u->out_hwbuf_size, &u->sink->sample_spec);
        }
        
    } else {

        if (enable_bits)
            if (ioctl(u->fd, SNDCTL_DSP_POST, NULL) < 0)
                pa_log_warn("SNDCTL_DSP_POST: %s", pa_cstrerror(errno));
        
        if (!quick) {
            /*
             * Some crappy drivers do not start the recording until we
             * read something.  Without this snippet, poll will never
             * register the fd as ready.
             */
            
            if (u->source && u->source->thread_info.state != PA_SOURCE_SUSPENDED) {
                uint8_t *buf = pa_xnew(uint8_t, u->in_fragment_size);
                pa_read(u->fd, buf, u->in_fragment_size, NULL);
                pa_xfree(buf);
            }
        }
    }
}

static void mmap_fill_memblocks(struct userdata *u, unsigned n) {
    pa_assert(u);
    pa_assert(u->out_mmap_memblocks);

    while (n > 0) {
        pa_memchunk chunk;

        if (u->out_mmap_memblocks[u->out_mmap_current])
            pa_memblock_unref_fixed(u->out_mmap_memblocks[u->out_mmap_current]);

        chunk.memblock = u->out_mmap_memblocks[u->out_mmap_current] =
            pa_memblock_new_fixed(
                    u->core->mempool,
                    (uint8_t*) u->out_mmap + u->out_fragment_size * u->out_mmap_current,
                    u->out_fragment_size,
                    1);

        chunk.length = pa_memblock_get_length(chunk.memblock);
        chunk.index = 0;

        pa_sink_render_into_full(u->sink, &chunk);

        u->out_mmap_current++;
        while (u->out_mmap_current >= u->out_nfrags)
            u->out_mmap_current -= u->out_nfrags;

        n--;
    }
}

static int mmap_write(struct userdata *u) {
    struct count_info info;
    
    
    pa_assert(u);
    pa_assert(u->sink);

    if (ioctl(u->fd, SNDCTL_DSP_GETOPTR, &info) < 0) {
        pa_log("SNDCTL_DSP_GETOPTR: %s", pa_cstrerror(errno));
        return -1;
    }

    info.blocks += u->out_mmap_saved_nfrags;
    u->out_mmap_saved_nfrags = 0;

    if (info.blocks > 0)
        mmap_fill_memblocks(u, info.blocks);
    
    return info.blocks;
}

static void mmap_post_memblocks(struct userdata *u, unsigned n) {
    pa_assert(u);
    pa_assert(u->in_mmap_memblocks);

    while (n > 0) {
        pa_memchunk chunk;

        if (!u->in_mmap_memblocks[u->in_mmap_current]) {
            
            chunk.memblock = u->in_mmap_memblocks[u->in_mmap_current] =
                pa_memblock_new_fixed(
                        u->core->mempool,
                        (uint8_t*) u->in_mmap + u->in_fragment_size*u->in_mmap_current,
                        u->in_fragment_size,
                        1);
            
            chunk.length = pa_memblock_get_length(chunk.memblock);
            chunk.index = 0;

            pa_source_post(u->source, &chunk);
        }

        u->in_mmap_current++;
        while (u->in_mmap_current >= u->in_nfrags)
            u->in_mmap_current -= u->in_nfrags;

        n--;
    }
}

static void mmap_clear_memblocks(struct userdata*u, unsigned n) {
    unsigned i = u->in_mmap_current;
    
    pa_assert(u);
    pa_assert(u->in_mmap_memblocks);

    if (n > u->in_nfrags)
        n = u->in_nfrags;

    while (n > 0) {
        if (u->in_mmap_memblocks[i]) {
            pa_memblock_unref_fixed(u->in_mmap_memblocks[i]);
            u->in_mmap_memblocks[i] = NULL;
        }

        i++;
        while (i >= u->in_nfrags)
            i -= u->in_nfrags;

        n--;
    }
}

static int mmap_read(struct userdata *u) {
    struct count_info info;
    pa_assert(u);
    pa_assert(u->source);

    if (ioctl(u->fd, SNDCTL_DSP_GETIPTR, &info) < 0) {
        pa_log("SNDCTL_DSP_GETIPTR: %s", pa_cstrerror(errno));
        return -1;
    }

    info.blocks += u->in_mmap_saved_nfrags;
    u->in_mmap_saved_nfrags = 0;

    if (info.blocks > 0) {
        mmap_post_memblocks(u, info.blocks);
        mmap_clear_memblocks(u, u->in_nfrags/2);
    }
    
    return info.blocks;
}

static pa_usec_t mmap_sink_get_latency(struct userdata *u) {
    struct count_info info;
    size_t bpos, n;
    
    pa_assert(u);

    if (ioctl(u->fd, SNDCTL_DSP_GETOPTR, &info) < 0) {
        pa_log("SNDCTL_DSP_GETOPTR: %s", pa_cstrerror(errno));
        return 0;
    }

    u->out_mmap_saved_nfrags += info.blocks;

    bpos = ((u->out_mmap_current + u->out_mmap_saved_nfrags) * u->out_fragment_size) % u->out_hwbuf_size;

    if (bpos <= (size_t) info.ptr)
        n = u->out_hwbuf_size - (info.ptr - bpos);
    else
        n = bpos - info.ptr;

/*     pa_log("n = %u, bpos = %u, ptr = %u, total=%u, fragsize = %u, n_frags = %u\n", n, bpos, (unsigned) info.ptr, total, u->out_fragment_size, u->out_fragments); */

    return pa_bytes_to_usec(n, &u->sink->sample_spec);
}

static pa_usec_t mmap_source_get_latency(struct userdata *u) {
    struct count_info info;
    size_t bpos, n;

    pa_assert(u);

    if (ioctl(u->fd, SNDCTL_DSP_GETIPTR, &info) < 0) {
        pa_log("SNDCTL_DSP_GETIPTR: %s", pa_cstrerror(errno));
        return 0;
    }

    u->in_mmap_saved_nfrags += info.blocks;
    bpos = ((u->in_mmap_current + u->in_mmap_saved_nfrags) * u->in_fragment_size) % u->in_hwbuf_size;

    if (bpos <= (size_t) info.ptr)
        n = info.ptr - bpos;
    else
        n = u->in_hwbuf_size - bpos + info.ptr;

/*     pa_log("n = %u, bpos = %u, ptr = %u, total=%u, fragsize = %u, n_frags = %u\n", n, bpos, (unsigned) info.ptr, total, u->in_fragment_size, u->in_fragments);  */

    return pa_bytes_to_usec(n, &u->source->sample_spec);
}

static pa_usec_t io_sink_get_latency(struct userdata *u) {
    pa_usec_t r = 0;
    
    pa_assert(u);
    
    if (u->use_getodelay) {
        int arg;
        
        if (ioctl(u->fd, SNDCTL_DSP_GETODELAY, &arg) < 0) {
            pa_log_info("Device doesn't support SNDCTL_DSP_GETODELAY: %s", pa_cstrerror(errno));
            u->use_getodelay = 0;
        } else
            r = pa_bytes_to_usec(arg, &u->sink->sample_spec);
        
    }
    
    if (!u->use_getodelay && u->use_getospace) {
        struct audio_buf_info info;
        
        if (ioctl(u->fd, SNDCTL_DSP_GETOSPACE, &info) < 0) {
            pa_log_info("Device doesn't support SNDCTL_DSP_GETOSPACE: %s", pa_cstrerror(errno));
            u->use_getospace = 0;
        } else
            r = pa_bytes_to_usec(info.bytes, &u->sink->sample_spec);
    }

    if (u->memchunk.memblock)
        r += pa_bytes_to_usec(u->memchunk.length, &u->sink->sample_spec);

    return r;
}


static pa_usec_t io_source_get_latency(struct userdata *u) {
    pa_usec_t r = 0;
    
    pa_assert(u);
    
    if (u->use_getispace) {
        struct audio_buf_info info;

        if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) < 0) {
            pa_log_info("Device doesn't support SNDCTL_DSP_GETISPACE: %s", pa_cstrerror(errno));
            u->use_getispace = 0;
        } else
            r = pa_bytes_to_usec(info.bytes, &u->source->sample_spec);
    }

    return r;
}

static int suspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->fd >= 0);

    if (u->out_mmap_memblocks) {
        unsigned i;
        for (i = 0; i < u->out_nfrags; i++)
            if (u->out_mmap_memblocks[i]) {
                pa_memblock_unref_fixed(u->out_mmap_memblocks[i]);
                u->out_mmap_memblocks[i] = NULL;
            }
    }

    if (u->in_mmap_memblocks) {
        unsigned i;
        for (i = 0; i < u->in_nfrags; i++)
            if (u->in_mmap_memblocks[i]) {
                pa_memblock_unref_fixed(u->in_mmap_memblocks[i]);
                u->in_mmap_memblocks[i] = NULL;
            }
    }
    
    if (u->in_mmap && u->in_mmap != MAP_FAILED) {
        munmap(u->in_mmap, u->in_hwbuf_size);
        u->in_mmap = NULL;
    }
        
    if (u->out_mmap && u->out_mmap != MAP_FAILED) {
        munmap(u->out_mmap, u->out_hwbuf_size);
        u->out_mmap = NULL;
    }

    /* Let's suspend */
    ioctl(u->fd, SNDCTL_DSP_SYNC, NULL);
    close(u->fd);
    u->fd = -1;

    pa_log_debug("Device suspended...");
    
    return 0;
}

static int unsuspend(struct userdata *u) {
    int m;
    pa_sample_spec ss, *ss_original;
    int frag_size, in_frag_size, out_frag_size;
    int in_nfrags, out_nfrags;
    struct audio_buf_info info;

    pa_assert(u);
    pa_assert(u->fd < 0);

    m = u->mode;

    pa_log_debug("Trying resume...");

    if ((u->fd = pa_oss_open(u->device_name, &m, NULL)) < 0) {
        pa_log_warn("Resume failed, device busy (%s)", pa_cstrerror(errno));
        return -1;

    if (m != u->mode)
        pa_log_warn("Resume failed, couldn't open device with original access mode.");
        goto fail;
    }

    if (u->nfrags >= 2 && u->frag_size >= 1)
        if (pa_oss_set_fragments(u->fd, u->nfrags, u->frag_size) < 0) {
            pa_log_warn("Resume failed, couldn't set original fragment settings.");
            goto fail;
        }

    ss = *(ss_original = u->sink ? &u->sink->sample_spec : &u->source->sample_spec);
    if (pa_oss_auto_format(u->fd, &ss) < 0 || !pa_sample_spec_equal(&ss, ss_original)) {
        pa_log_warn("Resume failed, couldn't set original sample format settings.");
        goto fail;
    }

    if (ioctl(u->fd, SNDCTL_DSP_GETBLKSIZE, &frag_size) < 0) {
        pa_log_warn("SNDCTL_DSP_GETBLKSIZE: %s", pa_cstrerror(errno));
        goto fail;
    }

    in_frag_size = out_frag_size = frag_size;
    in_nfrags = out_nfrags = u->nfrags;

    if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) >= 0) {
        in_frag_size = info.fragsize;
        in_nfrags = info.fragstotal;
    }

    if (ioctl(u->fd, SNDCTL_DSP_GETOSPACE, &info) >= 0) {
        out_frag_size = info.fragsize;
        out_nfrags = info.fragstotal;
    }

    if ((u->source && (in_frag_size != (int) u->in_fragment_size || in_nfrags != (int) u->in_nfrags)) ||
        (u->sink && (out_frag_size != (int) u->out_fragment_size || out_nfrags != (int) u->out_nfrags))) {
        pa_log_warn("Resume failed, input fragment settings don't match.");
        goto fail;
    }

    if (u->use_mmap) {
        if (u->source) {
            if ((u->in_mmap = mmap(NULL, u->in_hwbuf_size, PROT_READ, MAP_SHARED, u->fd, 0)) == MAP_FAILED) {
                pa_log("Resume failed, mmap(): %s", pa_cstrerror(errno));
                goto fail;
            }
        }

        if (u->sink) {
            if ((u->out_mmap = mmap(NULL, u->out_hwbuf_size, PROT_WRITE, MAP_SHARED, u->fd, 0)) == MAP_FAILED) {
                pa_log("Resume failed, mmap(): %s", pa_cstrerror(errno));
                if (u->in_mmap && u->in_mmap != MAP_FAILED) {
                    munmap(u->in_mmap, u->in_hwbuf_size);
                    u->in_mmap = NULL;
                }

                goto fail;
            }
            
            pa_silence_memory(u->out_mmap, u->out_hwbuf_size, &ss);
        }
    }

    u->out_mmap_current = u->in_mmap_current = 0;
    u->out_mmap_saved_nfrags = u->in_mmap_saved_nfrags = 0;

    if (u->sink)
        pa_sink_get_volume(u->sink);
    if (u->source)
        pa_source_get_volume(u->source);
    
    /* Now, start only what we need */
    trigger(u, 0);

    pa_log_debug("Resumed successfully...");

    return 0;

fail:
    close(u->fd);
    u->fd = -1;
    return -1;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;
    int do_trigger = 0, ret;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (u->fd >= 0) {
                if (u->use_mmap)
                    r = mmap_sink_get_latency(u);
                else
                    r = io_sink_get_latency(u);
            }

            *((pa_usec_t*) data) = r;

            break;
        }

        case PA_SINK_MESSAGE_SET_STATE:

            if (PA_PTR_TO_UINT(data) == PA_SINK_SUSPENDED) {
                pa_assert(u->sink->thread_info.state != PA_SINK_SUSPENDED);

                if (u->source_suspended) {
                    if (suspend(u) < 0)
                        return -1;
                } else
                    do_trigger = 1;

                u->sink_suspended = 1;
                
            } else if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                pa_assert(PA_PTR_TO_UINT(data) != PA_SINK_SUSPENDED);

                if (u->source_suspended) {
                    if (unsuspend(u) < 0) 
                        return -1;
                } else
                    do_trigger = 1;

                u->out_mmap_current = 0;
                u->out_mmap_saved_nfrags = 0;

                u->sink_suspended = 0;
            }
            
            break;

        case PA_SINK_MESSAGE_SET_VOLUME:

            if (u->use_pcm_volume && u->fd >= 0) {

                if (pa_oss_set_pcm_volume(u->fd, &u->sink->sample_spec, ((pa_cvolume*) data)) < 0) {
                    pa_log_info("Device doesn't support setting mixer settings: %s", pa_cstrerror(errno));
                    u->use_pcm_volume = 0;
                } else
                    return 0;
            }

            break;

        case PA_SINK_MESSAGE_GET_VOLUME:

            if (u->use_pcm_volume && u->fd >= 0) {

                if (pa_oss_get_pcm_volume(u->fd, &u->sink->sample_spec, ((pa_cvolume*) data)) < 0) {
                    pa_log_info("Device doesn't support reading mixer settings: %s", pa_cstrerror(errno));
                    u->use_pcm_volume = 0;
                } else
                    return 0;
            }

            break;
    }

    ret = pa_sink_process_msg(o, code, data, chunk);

    if (do_trigger)
        trigger(u, 1);
    
    return ret;
}

static int source_process_msg(pa_msgobject *o, int code, void *data, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;
    int do_trigger = 0, ret;

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (u->fd >= 0) {
                if (u->use_mmap)
                    r = mmap_source_get_latency(u);
                else
                    r = io_source_get_latency(u);
            }
            
            *((pa_usec_t*) data) = r;
            break;
        }

        case PA_SOURCE_MESSAGE_SET_STATE:

            if (PA_PTR_TO_UINT(data) == PA_SOURCE_SUSPENDED) {
                pa_assert(u->source->thread_info.state != PA_SOURCE_SUSPENDED);

                if (u->sink_suspended) {
                    if (suspend(u) < 0) 
                        return -1;
                } else
                    do_trigger = 1;

                u->source_suspended = 1;

            } else if (u->source->thread_info.state == PA_SOURCE_SUSPENDED) {
                pa_assert(PA_PTR_TO_UINT(data) != PA_SOURCE_SUSPENDED);

                if (u->sink_suspended) {
                    if (unsuspend(u) < 0) 
                        return -1;
                } else
                    do_trigger = 1;
                
                u->in_mmap_current = 0;
                u->in_mmap_saved_nfrags = 0;

                u->source_suspended = 0;
            }

            break;

        case PA_SOURCE_MESSAGE_SET_VOLUME:

            if (u->use_input_volume && u->fd >= 0) {

                if (pa_oss_set_input_volume(u->fd, &u->source->sample_spec, ((pa_cvolume*) data)) < 0) {
                    pa_log_info("Device doesn't support setting mixer settings: %s", pa_cstrerror(errno));
                    u->use_input_volume = 0;
                } else
                    return 0;
            }

            break;

        case PA_SOURCE_MESSAGE_GET_VOLUME:

            if (u->use_input_volume && u->fd >= 0) {

                if (pa_oss_get_input_volume(u->fd, &u->source->sample_spec, ((pa_cvolume*) data)) < 0) {
                    pa_log_info("Device doesn't support reading mixer settings: %s", pa_cstrerror(errno));
                    u->use_input_volume = 0;
                } else
                    return 0;
            }

            break;
    }

    ret = pa_source_process_msg(o, code, data, chunk);

    if (do_trigger)
        trigger(u, 1);

    return ret;
}

static void thread_func(void *userdata) {
    enum {
        POLLFD_ASYNCQ,
        POLLFD_DSP,
        POLLFD_MAX,
    };

    struct userdata *u = userdata;
    struct pollfd pollfd[POLLFD_MAX];
    int write_type = 0, read_type = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    trigger(u, 0);
    
    memset(&pollfd, 0, sizeof(pollfd));

    pollfd[POLLFD_ASYNCQ].fd = pa_asyncmsgq_get_fd(u->asyncmsgq);
    pollfd[POLLFD_ASYNCQ].events = POLLIN;
    pollfd[POLLFD_DSP].fd = u->fd;

    for (;;) {
        pa_msgobject *object;
        int code;
        void *data;
        pa_memchunk chunk;
        int r;

/*         pa_log("loop");   */
        
        /* Check whether there is a message for us to process */
        if (pa_asyncmsgq_get(u->asyncmsgq, &object, &code, &data, &chunk, 0) == 0) {
            int ret;

/*             pa_log("processing msg"); */

            if (!object && code == PA_MESSAGE_SHUTDOWN) {
                pa_asyncmsgq_done(u->asyncmsgq, 0);
                goto finish;
            }

            ret = pa_asyncmsgq_dispatch(object, code, data, &chunk);
            pa_asyncmsgq_done(u->asyncmsgq, ret);
            continue;
        } 

/*         pa_log("loop2"); */

        /* Render some data and write it to the dsp */

        if (u->sink && u->sink->thread_info.state != PA_SINK_SUSPENDED && (pollfd[POLLFD_DSP].revents & POLLOUT)) {

            if (u->use_mmap) {
                int ret;

                if ((ret = mmap_write(u)) < 0)
                    goto fail;

                pollfd[POLLFD_DSP].revents &= ~POLLOUT;
                
                if (ret > 0)
                    continue;

            } else {
                ssize_t l;
                int loop = 0;
                
                l = u->out_fragment_size;
                
                if (u->use_getospace) {
                    audio_buf_info info;
                    
                    if (ioctl(u->fd, SNDCTL_DSP_GETOSPACE, &info) < 0) {
                        pa_log_info("Device doesn't support SNDCTL_DSP_GETOSPACE: %s", pa_cstrerror(errno));
                        u->use_getospace = 0;
                    } else {
                        if (info.bytes >= l) {
                            l = (info.bytes/l)*l;
                            loop = 1;
                        }
                    }
                }
                
                do {
                    void *p;
                    ssize_t t;
                    
                    pa_assert(l > 0);
                    
                    if (u->memchunk.length <= 0)
                        pa_sink_render(u->sink, l, &u->memchunk);
                    
                    pa_assert(u->memchunk.length > 0);
                    
                    p = pa_memblock_acquire(u->memchunk.memblock);
                    t = pa_write(u->fd, (uint8_t*) p + u->memchunk.index, u->memchunk.length, &write_type);
                    pa_memblock_release(u->memchunk.memblock);
                    
/*                     pa_log("wrote %i bytes of %u", t, l); */
                    
                    pa_assert(t != 0);
                    
                    if (t < 0) {
                        
                        if (errno == EINTR)
                            continue;
                        
                        else if (errno == EAGAIN) {
                            pa_log_debug("EAGAIN"); 
                            
                            pollfd[POLLFD_DSP].revents &= ~POLLOUT;
                            break;
                            
                        } else {
                            pa_log("Failed to write data to DSP: %s", pa_cstrerror(errno));
                            goto fail;
                        }
                        
                    } else {
                        
                        u->memchunk.index += t;
                        u->memchunk.length -= t;
                        
                        if (u->memchunk.length <= 0) {
                            pa_memblock_unref(u->memchunk.memblock);
                            pa_memchunk_reset(&u->memchunk);
                        }
                        
                        l -= t;
                        
                        pollfd[POLLFD_DSP].revents &= ~POLLOUT;
                    }
                    
                } while (loop && l > 0);
                
                continue;
            }
        }

        /* Try to read some data and pass it on to the source driver */

        if (u->source && u->source->thread_info.state != PA_SOURCE_SUSPENDED && ((pollfd[POLLFD_DSP].revents & POLLIN))) {

            if (u->use_mmap) {
                int ret;

                if ((ret = mmap_read(u)) < 0)
                    goto fail;

                pollfd[POLLFD_DSP].revents &= ~POLLIN;
                
                if (ret > 0)
                    continue;

            } else {

                void *p;
                ssize_t l;
                pa_memchunk memchunk;
                int loop = 0;

                l = u->in_fragment_size;

                if (u->use_getispace) {
                    audio_buf_info info;

                    if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) < 0) {
                        pa_log_info("Device doesn't support SNDCTL_DSP_GETISPACE: %s", pa_cstrerror(errno));
                        u->use_getispace = 0;
                    } else {
                        if (info.bytes >= l) {
                            l = (info.bytes/l)*l;
                            loop = 1;
                        }
                    }
                }

                do {
                    ssize_t t;

                    pa_assert(l > 0);

                    memchunk.memblock = pa_memblock_new(u->core->mempool, l);

                    p = pa_memblock_acquire(memchunk.memblock);
                    t = pa_read(u->fd, p, l, &read_type);
                    pa_memblock_release(memchunk.memblock);

                    pa_assert(t != 0); /* EOF cannot happen */

/*                     pa_log("read %i bytes of %u", t, l); */
                    
                    if (t < 0) {
                        pa_memblock_unref(memchunk.memblock);

                        if (errno == EINTR)
                            continue;

                        else if (errno == EAGAIN) {
                            pa_log_debug("EAGAIN"); 

                            pollfd[POLLFD_DSP].revents &= ~POLLIN;
                            break;

                        } else {
                            pa_log("Failed to read data from DSP: %s", pa_cstrerror(errno));
                            goto fail;
                        }

                    } else {
                        memchunk.index = 0;
                        memchunk.length = t;

                        pa_source_post(u->source, &memchunk);
                        pa_memblock_unref(memchunk.memblock);

                        l -= t;

                        pollfd[POLLFD_DSP].revents &= ~POLLIN;
                    }
                } while (loop && l > 0);

                continue;
            }
        }

        if (u->fd >= 0) {
            pollfd[POLLFD_DSP].fd = u->fd;
            pollfd[POLLFD_DSP].events =
                ((u->source && u->source->thread_info.state != PA_SOURCE_SUSPENDED) ? POLLIN : 0) |
                ((u->sink && u->sink->thread_info.state != PA_SINK_SUSPENDED) ? POLLOUT : 0);
        }
            
        /* Hmm, nothing to do. Let's sleep */

        if (pa_asyncmsgq_before_poll(u->asyncmsgq) < 0)
            continue;

/*         pa_log("polling for %i (legend: %i=POLLIN, %i=POLLOUT)", u->fd >= 0 ? pollfd[POLLFD_DSP].events : -1, POLLIN, POLLOUT); */
        r = poll(pollfd, u->fd >= 0 ? POLLFD_MAX : POLLFD_DSP, -1);
/*         pa_log("polling got dsp=%i amq=%i (%i)", r > 0 ? pollfd[POLLFD_DSP].revents : 0, r > 0 ? pollfd[POLLFD_ASYNCQ].revents : 0, r); */

        pa_asyncmsgq_after_poll(u->asyncmsgq);

        if (u->fd < 0)
            pollfd[POLLFD_DSP].revents = 0;
        
        if (r < 0) {
            if (errno == EINTR) {
                pollfd[POLLFD_ASYNCQ].revents = 0;
                pollfd[POLLFD_DSP].revents = 0;
                continue;
            }

            pa_log("poll() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        pa_assert(r > 0);

        if (pollfd[POLLFD_DSP].revents & ~(POLLOUT|POLLIN)) {
            pa_log("DSP shutdown.");
            goto fail;
        }

        pa_assert((pollfd[POLLFD_ASYNCQ].revents & ~POLLIN) == 0);
    }

fail:
    /* We have to continue processing messages until we receive the
     * SHUTDOWN message */
    pa_asyncmsgq_post(u->core->asyncmsgq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, NULL, NULL);
    pa_asyncmsgq_wait_for(u->asyncmsgq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_core *c, pa_module*m) {
    
    struct audio_buf_info info;
    struct userdata *u = NULL;
    const char *dev;
    int fd = -1;
    int nfrags, frag_size;
    int mode, caps;
    int record = 1, playback = 1, use_mmap = 1;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    char hwdesc[64], *t;
    const char *name;
    int namereg_fail;

    pa_assert(c);
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "record", &record) < 0 || pa_modargs_get_value_boolean(ma, "playback", &playback) < 0) {
        pa_log("record= and playback= expect numeric argument.");
        goto fail;
    }

    if (!playback && !record) {
        pa_log("Neither playback nor record enabled for device.");
        goto fail;
    }

    mode = (playback && record) ? O_RDWR : (playback ? O_WRONLY : (record ? O_RDONLY : 0));

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_OSS) < 0) {
        pa_log("Failed to parse sample specification or channel map");
        goto fail;
    }

    nfrags = DEFAULT_NFRAGS;
    frag_size = pa_usec_to_bytes(DEFAULT_FRAGSIZE_MSEC*1000, &ss);
    if (frag_size <= 0)
        frag_size = pa_frame_size(&ss);

    if (pa_modargs_get_value_s32(ma, "fragments", &nfrags) < 0 || pa_modargs_get_value_s32(ma, "fragment_size", &frag_size) < 0) {
        pa_log("Failed to parse fragments arguments");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "mmap", &use_mmap) < 0) {
        pa_log("Failed to parse mmap argument.");
        goto fail;
    }
    
    if ((fd = pa_oss_open(dev = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), &mode, &caps)) < 0)
        goto fail;

    if (use_mmap && (!(caps & DSP_CAP_MMAP) || !(caps & DSP_CAP_TRIGGER))) {
        pa_log_info("OSS device not mmap capable, falling back to UNIX read/write mode.");
        use_mmap = 0;
    }
    
    if (use_mmap && mode == O_WRONLY) {
        pa_log_info("Device opened for write only, cannot do memory mapping, falling back to UNIX read/write mode.");
        use_mmap = 0;
    }

    if (pa_oss_get_hw_description(dev, hwdesc, sizeof(hwdesc)) >= 0)
        pa_log_info("Hardware name is '%s'.", hwdesc);
    else
        hwdesc[0] = 0;

    pa_log_info("Device opened in %s mode.", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));

    if (nfrags >= 2 && frag_size >= 1)
        if (pa_oss_set_fragments(fd, nfrags, frag_size) < 0)
            goto fail;

    if (pa_oss_auto_format(fd, &ss) < 0)
        goto fail;

    if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &frag_size) < 0) {
        pa_log("SNDCTL_DSP_GETBLKSIZE: %s", pa_cstrerror(errno));
        goto fail;
    }
    pa_assert(frag_size > 0);

    u = pa_xnew0(struct userdata, 1);
    u->core = c;
    u->module = m;
    m->userdata = u;
    u->fd = fd;
    u->use_getospace = u->use_getispace = 1;
    u->use_getodelay = 1;
    u->use_input_volume = u->use_pcm_volume = 1;
    u->mode = mode;
    u->device_name = pa_xstrdup(dev);
    u->in_nfrags = u->out_nfrags = u->nfrags = nfrags;
    u->out_fragment_size = u->in_fragment_size = u->frag_size = frag_size;
    u->use_mmap = use_mmap;
    pa_assert_se(u->asyncmsgq = pa_asyncmsgq_new(0));

    if (ioctl(fd, SNDCTL_DSP_GETISPACE, &info) >= 0) {
        pa_log_info("Input -- %u fragments of size %u.", info.fragstotal, info.fragsize);
        u->in_fragment_size = info.fragsize;
        u->in_nfrags = info.fragstotal;
        u->use_getispace = 1;
    }

    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) >= 0) {
        pa_log_info("Output -- %u fragments of size %u.", info.fragstotal, info.fragsize);
        u->out_fragment_size = info.fragsize;
        u->out_nfrags = info.fragstotal;
        u->use_getospace = 1;
    }

    u->in_hwbuf_size = u->in_nfrags * u->in_fragment_size;
    u->out_hwbuf_size = u->out_nfrags * u->out_fragment_size;
    
    if (mode != O_WRONLY) {
        char *name_buf = NULL;

        if (use_mmap) {
            if ((u->in_mmap = mmap(NULL, u->in_hwbuf_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
                if (mode == O_RDWR) {
                    pa_log_debug("mmap() failed for input. Changing to O_WRONLY mode.");
                    mode = O_WRONLY;
                    goto try_write;
                } else {
                    pa_log("mmap(): %s", pa_cstrerror(errno));
                    goto fail;
                }
            }

            pa_log_debug("Successfully mmap()ed input buffer.");
        }

        if ((name = pa_modargs_get_value(ma, "source_name", NULL)))
            namereg_fail = 1;
        else {
            name = name_buf = pa_sprintf_malloc("oss_input.%s", pa_path_get_filename(dev));
            namereg_fail = 0;
        }

        u->source = pa_source_new(c, __FILE__, name, namereg_fail, &ss, &map);
        pa_xfree(name_buf);
        if (!u->source) {
            pa_log("Failed to create source object");
            goto fail;
        }

        u->source->parent.process_msg = source_process_msg;
        u->source->userdata = u;

        pa_source_set_module(u->source, m);
        pa_source_set_asyncmsgq(u->source, u->asyncmsgq);
        pa_source_set_description(u->source, t = pa_sprintf_malloc(
                                          "OSS PCM on %s%s%s%s",
                                          dev,
                                          hwdesc[0] ? " (" : "",
                                          hwdesc[0] ? hwdesc : "",
                                          hwdesc[0] ? ")" : ""));
        pa_xfree(t);
        u->source->is_hardware = 1;
        u->source->refresh_volume = 1;

        if (use_mmap)
            u->in_mmap_memblocks = pa_xnew0(pa_memblock*, u->in_nfrags);
    }

try_write:
    
    if (mode != O_RDONLY) {
        char *name_buf = NULL;

        if (use_mmap) {
            if ((u->out_mmap = mmap(NULL, u->out_hwbuf_size, PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
                if (mode == O_RDWR) {
                    pa_log_debug("mmap() failed for input. Changing to O_WRONLY mode.");
                    mode = O_WRONLY;
                    goto go_on;
                } else {
                    pa_log("mmap(): %s", pa_cstrerror(errno));
                    goto fail;
                }
            }

            pa_log_debug("Successfully mmap()ed output buffer.");
            pa_silence_memory(u->out_mmap, u->out_hwbuf_size, &ss);
        }
        
        if ((name = pa_modargs_get_value(ma, "sink_name", NULL)))
            namereg_fail = 1;
        else {
            name = name_buf = pa_sprintf_malloc("oss_output.%s", pa_path_get_filename(dev));
            namereg_fail = 0;
        }

        u->sink = pa_sink_new(c, __FILE__, name, namereg_fail, &ss, &map);
        pa_xfree(name_buf);
        if (!u->sink) {
            pa_log("Failed to create sink object");
            goto fail;
        }

        u->sink->parent.process_msg = sink_process_msg;
        u->sink->userdata = u;
        
        pa_sink_set_module(u->sink, m);
        pa_sink_set_asyncmsgq(u->sink, u->asyncmsgq);
        pa_sink_set_description(u->sink, t = pa_sprintf_malloc(
                                        "OSS PCM on %s%s%s%s",
                                        dev,
                                        hwdesc[0] ? " (" : "",
                                        hwdesc[0] ? hwdesc : "",
                                        hwdesc[0] ? ")" : ""));
        pa_xfree(t);
        u->sink->is_hardware = 1;
        u->sink->refresh_volume = 1;

        if (use_mmap)
            u->out_mmap_memblocks = pa_xnew0(pa_memblock*, u->out_nfrags);
    }

go_on:
    
    pa_assert(u->source || u->sink);

    pa_memchunk_reset(&u->memchunk);

    if (!(u->thread = pa_thread_new(thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_modargs_free(ma);

    /* Read mixer settings */
    if (u->source)
        pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->source), PA_SOURCE_MESSAGE_GET_VOLUME, &u->source->volume, NULL, NULL);
    if (u->sink)
        pa_asyncmsgq_post(u->asyncmsgq, PA_MSGOBJECT(u->sink), PA_SINK_MESSAGE_GET_VOLUME, &u->sink->volume, NULL, NULL);

    return 0;

fail:

    if (u)
        pa__done(c, m);
    else if (fd >= 0)
        close(fd);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;

    pa_assert(c);
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_disconnect(u->sink);

    if (u->source)
        pa_source_disconnect(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->asyncmsgq, NULL, PA_MESSAGE_SHUTDOWN, NULL, NULL);
        pa_thread_free(u->thread);
    }

    if (u->asyncmsgq)
        pa_asyncmsgq_free(u->asyncmsgq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->source)
        pa_source_unref(u->source);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->out_mmap_memblocks) {
        unsigned i;
        for (i = 0; i < u->out_nfrags; i++)
            if (u->out_mmap_memblocks[i])
                pa_memblock_unref_fixed(u->out_mmap_memblocks[i]);
        pa_xfree(u->out_mmap_memblocks);
    }

    if (u->in_mmap_memblocks) {
        unsigned i;
        for (i = 0; i < u->in_nfrags; i++)
            if (u->in_mmap_memblocks[i])
                pa_memblock_unref_fixed(u->in_mmap_memblocks[i]);
        pa_xfree(u->in_mmap_memblocks);
    }

    if (u->in_mmap && u->in_mmap != MAP_FAILED)
        munmap(u->in_mmap, u->in_hwbuf_size);

    if (u->out_mmap && u->out_mmap != MAP_FAILED)
        munmap(u->out_mmap, u->out_hwbuf_size);
    
    if (u->fd >= 0)
        close(u->fd);

    pa_xfree(u->device_name);
    
    pa_xfree(u);
}
