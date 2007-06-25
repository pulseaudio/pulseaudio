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

#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
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
        "channel_map=<channel map>")

#define DEFAULT_DEVICE "/dev/dsp"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_source *source;
    pa_thread *thread;
    pa_asyncmsgq *asyncmsgq;

    char *device_name;
    
    pa_memchunk memchunk;

    uint32_t in_fragment_size, out_fragment_size, sample_size;
    int use_getospace, use_getispace;
    int use_getodelay;

    int use_pcm_volume;
    int use_input_volume;

    int sink_suspended, source_suspended;

    int fd;
    int mode;

    int nfrags, frag_size;
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
    NULL
};

static int suspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->fd >= 0);

    /* Let's suspend */
    ioctl(u->fd, SNDCTL_DSP_SYNC, NULL);
    close(u->fd);
    u->fd = -1;
    
    return 0;
}

static int unsuspend(struct userdata *u) {
    int m;
    pa_sample_spec ss, *ss_original;
    int frag_size, in_frag_size, out_frag_size;
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

    if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) >= 0)
        in_frag_size = info.fragsize;

    if (ioctl(u->fd, SNDCTL_DSP_GETOSPACE, &info) >= 0)
        out_frag_size = info.fragsize;

    if ((u->source && in_frag_size != (int) u->in_fragment_size) || (u->sink && out_frag_size != (int) u->out_fragment_size)) {
        pa_log_warn("Resume failed, fragment settings don't match.");
        goto fail;
    }

    /*
     * Some crappy drivers do not start the recording until we read something.
     * Without this snippet, poll will never register the fd as ready.
     */
    if (u->source) {
        uint8_t *buf = pa_xnew(uint8_t, u->sample_size);
        pa_read(u->fd, buf, u->sample_size, NULL);
        pa_xfree(buf);
    }

    return 0;

fail:
    close(u->fd);
    u->fd = -1;
    return -1;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (u->fd >= 0) {
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
            }

            if (u->memchunk.memblock)
                r += pa_bytes_to_usec(u->memchunk.length, &u->sink->sample_spec);

            *((pa_usec_t*) data) = r;

            break;
        }

        case PA_SINK_MESSAGE_SET_STATE:

            if (PA_PTR_TO_UINT(data) == PA_SINK_SUSPENDED) {
                pa_assert(u->sink->thread_info.state != PA_SINK_SUSPENDED);

                if (u->source_suspended)
                    if (suspend(u) < 0)
                        return -1;

                u->sink_suspended = 1;
            }

            if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                pa_assert(PA_PTR_TO_UINT(data) != PA_SINK_SUSPENDED);

                if (u->source_suspended)
                    if (unsuspend(u) < 0)
                        return -1;

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

    return pa_sink_process_msg(o, code, data, chunk);
}

static int source_process_msg(pa_msgobject *o, int code, void *data, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (u->use_getispace && u->fd >= 0) {
                struct audio_buf_info info;

                if (ioctl(u->fd, SNDCTL_DSP_GETISPACE, &info) < 0) {
                    pa_log_info("Device doesn't support SNDCTL_DSP_GETISPACE: %s", pa_cstrerror(errno));
                    u->use_getispace = 0;
                } else
                    r = pa_bytes_to_usec(info.bytes, &u->sink->sample_spec);
            }

            *((pa_usec_t*) data) = r;
            break;
        }

        case PA_SOURCE_MESSAGE_SET_STATE:

            if (PA_PTR_TO_UINT(data) == PA_SOURCE_SUSPENDED) {
                pa_assert(u->source->thread_info.state != PA_SOURCE_SUSPENDED);

                if (u->sink_suspended)
                    if (suspend(u) < 0)
                        return -1;

                u->source_suspended = 1;
            }

            if (u->source->thread_info.state == PA_SOURCE_SUSPENDED) {
                pa_assert(PA_PTR_TO_UINT(data) != PA_SOURCE_SUSPENDED);

                if (u->sink_suspended)
                    if (unsuspend(u) < 0)
                        return -1;

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

    return pa_source_process_msg(o, code, data, chunk);
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

    /*
     * Some crappy drivers do not start the recording until we read something.
     * Without this snippet, poll will never register the fd as ready.
     */
    if (u->source) {
        uint8_t *buf = pa_xnew(uint8_t, u->sample_size);
        pa_read(u->fd, buf, u->sample_size, &read_type);
        pa_xfree(buf);
    }

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

/*         pa_log("loop"); */
        
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
            ssize_t l;
            void *p;
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
                ssize_t t;

                pa_assert(l > 0);

                if (u->memchunk.length <= 0)
                    pa_sink_render(u->sink, l, &u->memchunk);

                pa_assert(u->memchunk.length > 0);

                p = pa_memblock_acquire(u->memchunk.memblock);
                t = pa_write(u->fd, (uint8_t*) p + u->memchunk.index, u->memchunk.length, &write_type);
                pa_memblock_release(u->memchunk.memblock);

/*                 pa_log("wrote %i bytes", t); */
                
                pa_assert(t != 0);

                if (t < 0) {

                    if (errno == EINTR)
                        continue;

                    else if (errno == EAGAIN) {

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

        /* Try to read some data and pass it on to the source driver */

        if (u->source && u->source->thread_info.state != PA_SOURCE_SUSPENDED && ((pollfd[POLLFD_DSP].revents & POLLIN))) {
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

                if (t < 0) {
                    pa_memblock_unref(memchunk.memblock);

                    if (errno == EINTR)
                        continue;

                    else if (errno == EAGAIN) {
                        pollfd[POLLFD_DSP].revents &= ~POLLIN;
                        break;

                    } else {
                        pa_log("Faile to read data from DSP: %s", pa_cstrerror(errno));
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


        if (u->fd >= 0) {
            pollfd[POLLFD_DSP].fd = u->fd;
            pollfd[POLLFD_DSP].events =
                ((u->source && u->source->thread_info.state != PA_SOURCE_SUSPENDED) ? POLLIN : 0) |
                ((u->sink && u->sink->thread_info.state != PA_SINK_SUSPENDED) ? POLLOUT : 0);
        }
            
        /* Hmm, nothing to do. Let's sleep */

        if (pa_asyncmsgq_before_poll(u->asyncmsgq) < 0)
            continue;

/*         pa_log("polling for %i", u->fd >= 0 ? pollfd[POLLFD_DSP].events : -1);    */
        r = poll(pollfd, u->fd >= 0 ? POLLFD_MAX : POLLFD_DSP, -1);
/*         pa_log("polling got dsp=%i amq=%i (%i)", r > 0 ? pollfd[POLLFD_DSP].revents : 0, r > 0 ? pollfd[POLLFD_ASYNCQ].revents : 0, r);    */

        pa_asyncmsgq_after_poll(u->asyncmsgq);

        if (u->fd < 0)
            pollfd[POLLFD_DSP].revents = 0;
        
        if (r < 0) {
            if (errno == EINTR)
                continue;

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
    const char *p;
    int fd = -1;
    int nfrags, frag_size, in_frag_size, out_frag_size;
    int mode;
    int record = 1, playback = 1;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    char hwdesc[64], *t;
    const char *name;
    char *name_buf = NULL;
    int namereg_fail;

    assert(c);
    assert(m);

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

    /* Fix latency to 100ms */
    nfrags = 12;
    frag_size = pa_bytes_per_second(&ss)/128;

    if (pa_modargs_get_value_s32(ma, "fragments", &nfrags) < 0 || pa_modargs_get_value_s32(ma, "fragment_size", &frag_size) < 0) {
        pa_log("Failed to parse fragments arguments");
        goto fail;
    }

    if ((fd = pa_oss_open(p = pa_modargs_get_value(ma, "device", DEFAULT_DEVICE), &mode, NULL)) < 0)
        goto fail;

    if (pa_oss_get_hw_description(p, hwdesc, sizeof(hwdesc)) >= 0)
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
    assert(frag_size);
    in_frag_size = out_frag_size = frag_size;

    u = pa_xnew0(struct userdata, 1);
    u->core = c;
    u->module = m;
    m->userdata = u;
    u->use_getospace = u->use_getispace = 1;
    u->use_getodelay = 1;
    u->use_input_volume = u->use_pcm_volume = 1;
    u->mode = mode;
    u->device_name = pa_xstrdup(p);
    u->nfrags = nfrags;
    u->frag_size = frag_size;
    pa_assert_se(u->asyncmsgq = pa_asyncmsgq_new(0));

    if (ioctl(fd, SNDCTL_DSP_GETISPACE, &info) >= 0) {
        pa_log_info("Input -- %u fragments of size %u.", info.fragstotal, info.fragsize);
        in_frag_size = info.fragsize;
        u->use_getispace = 1;
    }

    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) >= 0) {
        pa_log_info("Output -- %u fragments of size %u.", info.fragstotal, info.fragsize);
        out_frag_size = info.fragsize;
        u->use_getospace = 1;
    }

    if (mode != O_WRONLY) {
        if ((name = pa_modargs_get_value(ma, "source_name", NULL)))
            namereg_fail = 1;
        else {
            name = name_buf = pa_sprintf_malloc("oss_input.%s", pa_path_get_filename(p));
            namereg_fail = 0;
        }

        if (!(u->source = pa_source_new(c, __FILE__, name, namereg_fail, &ss, &map)))
            goto fail;

        u->source->parent.process_msg = source_process_msg;
        u->source->userdata = u;

        pa_source_set_module(u->source, m);
        pa_source_set_asyncmsgq(u->source, u->asyncmsgq);
        pa_source_set_description(u->source, t = pa_sprintf_malloc("OSS PCM on %s%s%s%s",
                                                                 p,
                                                                 hwdesc[0] ? " (" : "",
                                                                 hwdesc[0] ? hwdesc : "",
                                                                 hwdesc[0] ? ")" : ""));
        pa_xfree(t);
        u->source->is_hardware = 1;
        u->source->refresh_volume = 1;
    } else
        u->source = NULL;

    pa_xfree(name_buf);
    name_buf = NULL;

    if (mode != O_RDONLY) {
        if ((name = pa_modargs_get_value(ma, "sink_name", NULL)))
            namereg_fail = 1;
        else {
            name = name_buf = pa_sprintf_malloc("oss_output.%s", pa_path_get_filename(p));
            namereg_fail = 0;
        }

        if (!(u->sink = pa_sink_new(c, __FILE__, name, namereg_fail, &ss, &map)))
            goto fail;

        u->sink->parent.process_msg = sink_process_msg;
        u->sink->userdata = u;
        
        pa_sink_set_module(u->sink, m);
        pa_sink_set_asyncmsgq(u->sink, u->asyncmsgq);
        pa_sink_set_description(u->sink, t = pa_sprintf_malloc("OSS PCM on %s%s%s%s",
                                                           p,
                                                           hwdesc[0] ? " (" : "",
                                                           hwdesc[0] ? hwdesc : "",
                                                           hwdesc[0] ? ")" : ""));
        pa_xfree(t);
        u->sink->is_hardware = 1;
        u->sink->refresh_volume = 1;
    } else
        u->sink = NULL;

    pa_xfree(name_buf);
    name_buf = NULL;

    assert(u->source || u->sink);

    u->fd = fd;

    pa_memchunk_reset(&u->memchunk);
    u->sample_size = pa_frame_size(&ss);

    u->out_fragment_size = out_frag_size;
    u->in_fragment_size = in_frag_size;

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
    if (fd >= 0)
        close(fd);

    if (ma)
        pa_modargs_free(ma);

    pa_xfree(name_buf);

    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;

    assert(c);
    assert(m);

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

    if (u->fd >= 0)
        close(u->fd);

    pa_xfree(u->device_name);
    
    pa_xfree(u);
}
