/***
  This file is part of PulseAudio.

  Copyright 2004-2009 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <sys/types.h>
#include <limits.h>
#include <asoundlib.h>

#include <pulse/sample.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/i18n.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/atomic.h>
#include <pulsecore/core-error.h>
#include <pulsecore/once.h>

#include "alsa-util.h"

#ifdef HAVE_HAL
#include "hal-util.h"
#endif

#ifdef HAVE_UDEV
#include "udev-util.h"
#endif

struct pa_alsa_fdlist {
    unsigned num_fds;
    struct pollfd *fds;
    /* This is a temporary buffer used to avoid lots of mallocs */
    struct pollfd *work_fds;

    snd_mixer_t *mixer;

    pa_mainloop_api *m;
    pa_defer_event *defer;
    pa_io_event **ios;

    pa_bool_t polled;

    void (*cb)(void *userdata);
    void *userdata;
};

static void io_cb(pa_mainloop_api*a, pa_io_event* e, int fd, pa_io_event_flags_t events, void *userdata) {

    struct pa_alsa_fdlist *fdl = userdata;
    int err;
    unsigned i;
    unsigned short revents;

    pa_assert(a);
    pa_assert(fdl);
    pa_assert(fdl->mixer);
    pa_assert(fdl->fds);
    pa_assert(fdl->work_fds);

    if (fdl->polled)
        return;

    fdl->polled = TRUE;

    memcpy(fdl->work_fds, fdl->fds, sizeof(struct pollfd) * fdl->num_fds);

    for (i = 0; i < fdl->num_fds; i++) {
        if (e == fdl->ios[i]) {
            if (events & PA_IO_EVENT_INPUT)
                fdl->work_fds[i].revents |= POLLIN;
            if (events & PA_IO_EVENT_OUTPUT)
                fdl->work_fds[i].revents |= POLLOUT;
            if (events & PA_IO_EVENT_ERROR)
                fdl->work_fds[i].revents |= POLLERR;
            if (events & PA_IO_EVENT_HANGUP)
                fdl->work_fds[i].revents |= POLLHUP;
            break;
        }
    }

    pa_assert(i != fdl->num_fds);

    if ((err = snd_mixer_poll_descriptors_revents(fdl->mixer, fdl->work_fds, fdl->num_fds, &revents)) < 0) {
        pa_log_error("Unable to get poll revent: %s", snd_strerror(err));
        return;
    }

    a->defer_enable(fdl->defer, 1);

    if (revents)
        snd_mixer_handle_events(fdl->mixer);
}

static void defer_cb(pa_mainloop_api*a, pa_defer_event* e, void *userdata) {
    struct pa_alsa_fdlist *fdl = userdata;
    unsigned num_fds, i;
    int err, n;
    struct pollfd *temp;

    pa_assert(a);
    pa_assert(fdl);
    pa_assert(fdl->mixer);

    a->defer_enable(fdl->defer, 0);

    if ((n = snd_mixer_poll_descriptors_count(fdl->mixer)) < 0) {
        pa_log("snd_mixer_poll_descriptors_count() failed: %s", snd_strerror(n));
        return;
    }
    num_fds = (unsigned) n;

    if (num_fds != fdl->num_fds) {
        if (fdl->fds)
            pa_xfree(fdl->fds);
        if (fdl->work_fds)
            pa_xfree(fdl->work_fds);
        fdl->fds = pa_xnew0(struct pollfd, num_fds);
        fdl->work_fds = pa_xnew(struct pollfd, num_fds);
    }

    memset(fdl->work_fds, 0, sizeof(struct pollfd) * num_fds);

    if ((err = snd_mixer_poll_descriptors(fdl->mixer, fdl->work_fds, num_fds)) < 0) {
        pa_log_error("Unable to get poll descriptors: %s", snd_strerror(err));
        return;
    }

    fdl->polled = FALSE;

    if (memcmp(fdl->fds, fdl->work_fds, sizeof(struct pollfd) * num_fds) == 0)
        return;

    if (fdl->ios) {
        for (i = 0; i < fdl->num_fds; i++)
            a->io_free(fdl->ios[i]);

        if (num_fds != fdl->num_fds) {
            pa_xfree(fdl->ios);
            fdl->ios = NULL;
        }
    }

    if (!fdl->ios)
        fdl->ios = pa_xnew(pa_io_event*, num_fds);

    /* Swap pointers */
    temp = fdl->work_fds;
    fdl->work_fds = fdl->fds;
    fdl->fds = temp;

    fdl->num_fds = num_fds;

    for (i = 0;i < num_fds;i++)
        fdl->ios[i] = a->io_new(a, fdl->fds[i].fd,
            ((fdl->fds[i].events & POLLIN) ? PA_IO_EVENT_INPUT : 0) |
            ((fdl->fds[i].events & POLLOUT) ? PA_IO_EVENT_OUTPUT : 0),
            io_cb, fdl);
}

struct pa_alsa_fdlist *pa_alsa_fdlist_new(void) {
    struct pa_alsa_fdlist *fdl;

    fdl = pa_xnew0(struct pa_alsa_fdlist, 1);

    fdl->num_fds = 0;
    fdl->fds = NULL;
    fdl->work_fds = NULL;
    fdl->mixer = NULL;
    fdl->m = NULL;
    fdl->defer = NULL;
    fdl->ios = NULL;
    fdl->polled = FALSE;

    return fdl;
}

void pa_alsa_fdlist_free(struct pa_alsa_fdlist *fdl) {
    pa_assert(fdl);

    if (fdl->defer) {
        pa_assert(fdl->m);
        fdl->m->defer_free(fdl->defer);
    }

    if (fdl->ios) {
        unsigned i;
        pa_assert(fdl->m);
        for (i = 0; i < fdl->num_fds; i++)
            fdl->m->io_free(fdl->ios[i]);
        pa_xfree(fdl->ios);
    }

    if (fdl->fds)
        pa_xfree(fdl->fds);
    if (fdl->work_fds)
        pa_xfree(fdl->work_fds);

    pa_xfree(fdl);
}

int pa_alsa_fdlist_set_mixer(struct pa_alsa_fdlist *fdl, snd_mixer_t *mixer_handle, pa_mainloop_api* m) {
    pa_assert(fdl);
    pa_assert(mixer_handle);
    pa_assert(m);
    pa_assert(!fdl->m);

    fdl->mixer = mixer_handle;
    fdl->m = m;
    fdl->defer = m->defer_new(m, defer_cb, fdl);

    return 0;
}

static int set_format(snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *hwparams, pa_sample_format_t *f) {

    static const snd_pcm_format_t format_trans[] = {
        [PA_SAMPLE_U8] = SND_PCM_FORMAT_U8,
        [PA_SAMPLE_ALAW] = SND_PCM_FORMAT_A_LAW,
        [PA_SAMPLE_ULAW] = SND_PCM_FORMAT_MU_LAW,
        [PA_SAMPLE_S16LE] = SND_PCM_FORMAT_S16_LE,
        [PA_SAMPLE_S16BE] = SND_PCM_FORMAT_S16_BE,
        [PA_SAMPLE_FLOAT32LE] = SND_PCM_FORMAT_FLOAT_LE,
        [PA_SAMPLE_FLOAT32BE] = SND_PCM_FORMAT_FLOAT_BE,
        [PA_SAMPLE_S32LE] = SND_PCM_FORMAT_S32_LE,
        [PA_SAMPLE_S32BE] = SND_PCM_FORMAT_S32_BE,
        [PA_SAMPLE_S24LE] = SND_PCM_FORMAT_S24_3LE,
        [PA_SAMPLE_S24BE] = SND_PCM_FORMAT_S24_3BE,
        [PA_SAMPLE_S24_32LE] = SND_PCM_FORMAT_S24_LE,
        [PA_SAMPLE_S24_32BE] = SND_PCM_FORMAT_S24_BE,
    };

    static const pa_sample_format_t try_order[] = {
        PA_SAMPLE_FLOAT32NE,
        PA_SAMPLE_FLOAT32RE,
        PA_SAMPLE_S32NE,
        PA_SAMPLE_S32RE,
        PA_SAMPLE_S24_32NE,
        PA_SAMPLE_S24_32RE,
        PA_SAMPLE_S24NE,
        PA_SAMPLE_S24RE,
        PA_SAMPLE_S16NE,
        PA_SAMPLE_S16RE,
        PA_SAMPLE_ALAW,
        PA_SAMPLE_ULAW,
        PA_SAMPLE_U8,
        PA_SAMPLE_INVALID
    };

    int i, ret;

    pa_assert(pcm_handle);
    pa_assert(f);

    if ((ret = snd_pcm_hw_params_set_format(pcm_handle, hwparams, format_trans[*f])) >= 0)
        return ret;

    if (*f == PA_SAMPLE_FLOAT32BE)
        *f = PA_SAMPLE_FLOAT32LE;
    else if (*f == PA_SAMPLE_FLOAT32LE)
        *f = PA_SAMPLE_FLOAT32BE;
    else if (*f == PA_SAMPLE_S24BE)
        *f = PA_SAMPLE_S24LE;
    else if (*f == PA_SAMPLE_S24LE)
        *f = PA_SAMPLE_S24BE;
    else if (*f == PA_SAMPLE_S24_32BE)
        *f = PA_SAMPLE_S24_32LE;
    else if (*f == PA_SAMPLE_S24_32LE)
        *f = PA_SAMPLE_S24_32BE;
    else if (*f == PA_SAMPLE_S16BE)
        *f = PA_SAMPLE_S16LE;
    else if (*f == PA_SAMPLE_S16LE)
        *f = PA_SAMPLE_S16BE;
    else if (*f == PA_SAMPLE_S32BE)
        *f = PA_SAMPLE_S32LE;
    else if (*f == PA_SAMPLE_S32LE)
        *f = PA_SAMPLE_S32BE;
    else
        goto try_auto;

    if ((ret = snd_pcm_hw_params_set_format(pcm_handle, hwparams, format_trans[*f])) >= 0)
        return ret;

try_auto:

    for (i = 0; try_order[i] != PA_SAMPLE_INVALID; i++) {
        *f = try_order[i];

        if ((ret = snd_pcm_hw_params_set_format(pcm_handle, hwparams, format_trans[*f])) >= 0)
            return ret;
    }

    return -1;
}

/* Set the hardware parameters of the given ALSA device. Returns the
 * selected fragment settings in *period and *period_size */
int pa_alsa_set_hw_params(
        snd_pcm_t *pcm_handle,
        pa_sample_spec *ss,
        uint32_t *periods,
        snd_pcm_uframes_t *period_size,
        snd_pcm_uframes_t tsched_size,
        pa_bool_t *use_mmap,
        pa_bool_t *use_tsched,
        pa_bool_t require_exact_channel_number) {

    int ret = -1;
    snd_pcm_uframes_t _period_size = period_size ? *period_size : 0;
    unsigned int _periods = periods ? *periods : 0;
    snd_pcm_uframes_t buffer_size;
    unsigned int r = ss->rate;
    unsigned int c = ss->channels;
    pa_sample_format_t f = ss->format;
    snd_pcm_hw_params_t *hwparams;
    pa_bool_t _use_mmap = use_mmap && *use_mmap;
    pa_bool_t _use_tsched = use_tsched && *use_tsched;
    int dir;

    pa_assert(pcm_handle);
    pa_assert(ss);

    snd_pcm_hw_params_alloca(&hwparams);

    if ((ret = snd_pcm_hw_params_any(pcm_handle, hwparams)) < 0)
        goto finish;

    if ((ret = snd_pcm_hw_params_set_rate_resample(pcm_handle, hwparams, 0)) < 0)
        goto finish;

    if (_use_mmap) {

        if (snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0) {

            /* mmap() didn't work, fall back to interleaved */

            if ((ret = snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
                goto finish;

            _use_mmap = FALSE;
        }

    } else if ((ret = snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        goto finish;

    if (!_use_mmap)
        _use_tsched = FALSE;

    if ((ret = set_format(pcm_handle, hwparams, &f)) < 0)
        goto finish;

    if ((ret = snd_pcm_hw_params_set_rate_near(pcm_handle, hwparams, &r, NULL)) < 0)
        goto finish;

    if (require_exact_channel_number) {
        if ((ret = snd_pcm_hw_params_set_channels(pcm_handle, hwparams, c)) < 0)
            goto finish;
    } else {
        if ((ret = snd_pcm_hw_params_set_channels_near(pcm_handle, hwparams, &c)) < 0)
            goto finish;
    }

    if ((ret = snd_pcm_hw_params_set_periods_integer(pcm_handle, hwparams)) < 0)
        goto finish;

    if (_period_size && tsched_size && _periods) {
        /* Adjust the buffer sizes, if we didn't get the rate we were asking for */
        _period_size = (snd_pcm_uframes_t) (((uint64_t) _period_size * r) / ss->rate);
        tsched_size = (snd_pcm_uframes_t) (((uint64_t) tsched_size * r) / ss->rate);

        if (_use_tsched) {
            _period_size = tsched_size;
            _periods = 1;

            pa_assert_se(snd_pcm_hw_params_get_buffer_size_max(hwparams, &buffer_size) == 0);
            pa_log_debug("Maximum hw buffer size is %u ms", (unsigned) buffer_size * 1000 / r);
        }

        buffer_size = _periods * _period_size;

        if (_periods > 0) {

            /* First we pass 0 as direction to get exactly what we asked
             * for. That this is necessary is presumably a bug in ALSA */

            dir = 0;
            if ((ret = snd_pcm_hw_params_set_periods_near(pcm_handle, hwparams, &_periods, &dir)) < 0) {
                dir = 1;
                if ((ret = snd_pcm_hw_params_set_periods_near(pcm_handle, hwparams, &_periods, &dir)) < 0) {
                    dir = -1;
                    if ((ret = snd_pcm_hw_params_set_periods_near(pcm_handle, hwparams, &_periods, &dir)) < 0)
                        goto finish;
                }
            }
        }

        if (_period_size > 0)
            if ((ret = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hwparams, &buffer_size)) < 0)
                goto finish;
    }

    if  ((ret = snd_pcm_hw_params(pcm_handle, hwparams)) < 0)
        goto finish;

    if (ss->rate != r)
        pa_log_info("Device %s doesn't support %u Hz, changed to %u Hz.", snd_pcm_name(pcm_handle), ss->rate, r);

    if (ss->channels != c)
        pa_log_info("Device %s doesn't support %u channels, changed to %u.", snd_pcm_name(pcm_handle), ss->channels, c);

    if (ss->format != f)
        pa_log_info("Device %s doesn't support sample format %s, changed to %s.", snd_pcm_name(pcm_handle), pa_sample_format_to_string(ss->format), pa_sample_format_to_string(f));

    if ((ret = snd_pcm_prepare(pcm_handle)) < 0)
        goto finish;

    if ((ret = snd_pcm_hw_params_get_period_size(hwparams, &_period_size, &dir)) < 0 ||
        (ret = snd_pcm_hw_params_get_periods(hwparams, &_periods, &dir)) < 0)
        goto finish;

    /* If the sample rate deviates too much, we need to resample */
    if (r < ss->rate*.95 || r > ss->rate*1.05)
        ss->rate = r;
    ss->channels = (uint8_t) c;
    ss->format = f;

    pa_assert(_periods > 0);
    pa_assert(_period_size > 0);

    if (periods)
        *periods = _periods;

    if (period_size)
        *period_size = _period_size;

    if (use_mmap)
        *use_mmap = _use_mmap;

    if (use_tsched)
        *use_tsched = _use_tsched;

    ret = 0;

    snd_pcm_nonblock(pcm_handle, 1);

finish:

    return ret;
}

int pa_alsa_set_sw_params(snd_pcm_t *pcm, snd_pcm_uframes_t avail_min) {
    snd_pcm_sw_params_t *swparams;
    snd_pcm_uframes_t boundary;
    int err;

    pa_assert(pcm);

    snd_pcm_sw_params_alloca(&swparams);

    if ((err = snd_pcm_sw_params_current(pcm, swparams) < 0)) {
        pa_log_warn("Unable to determine current swparams: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_period_event(pcm, swparams, 0)) < 0) {
        pa_log_warn("Unable to disable period event: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_tstamp_mode(pcm, swparams, SND_PCM_TSTAMP_ENABLE)) < 0) {
        pa_log_warn("Unable to enable time stamping: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_get_boundary(swparams, &boundary)) < 0) {
        pa_log_warn("Unable to get boundary: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_stop_threshold(pcm, swparams, boundary)) < 0) {
        pa_log_warn("Unable to set stop threshold: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_start_threshold(pcm, swparams, (snd_pcm_uframes_t) -1)) < 0) {
        pa_log_warn("Unable to set start threshold: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_avail_min(pcm, swparams, avail_min)) < 0) {
        pa_log_error("snd_pcm_sw_params_set_avail_min() failed: %s", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params(pcm, swparams)) < 0) {
        pa_log_warn("Unable to set sw params: %s\n", snd_strerror(err));
        return err;
    }

    return 0;
}

static const struct pa_alsa_profile_info device_table[] = {
    {{ 1, { PA_CHANNEL_POSITION_MONO }},
     "hw",
     N_("Analog Mono"),
     "analog-mono",
     1 },

    {{ 2, { PA_CHANNEL_POSITION_LEFT, PA_CHANNEL_POSITION_RIGHT }},
     "front",
     N_("Analog Stereo"),
     "analog-stereo",
     10 },

    {{ 2, { PA_CHANNEL_POSITION_LEFT, PA_CHANNEL_POSITION_RIGHT }},
     "iec958",
     N_("Digital Stereo (IEC958)"),
     "iec958-stereo",
     5 },

    {{ 2, { PA_CHANNEL_POSITION_LEFT, PA_CHANNEL_POSITION_RIGHT }},
     "hdmi",
     N_("Digital Stereo (HDMI)"),
     "hdmi-stereo",
     4 },

    {{ 4, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT }},
     "surround40",
     N_("Analog Surround 4.0"),
     "analog-surround-40",
     7 },

    {{ 4, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT }},
     "a52",
     N_("Digital Surround 4.0 (IEC958/AC3)"),
     "iec958-ac3-surround-40",
     2 },

    {{ 5, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_LFE }},
     "surround41",
     N_("Analog Surround 4.1"),
     "analog-surround-41",
     7 },

    {{ 5, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_CENTER }},
     "surround50",
     N_("Analog Surround 5.0"),
     "analog-surround-50",
     7 },

    {{ 6, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_CENTER, PA_CHANNEL_POSITION_LFE }},
     "surround51",
     N_("Analog Surround 5.1"),
     "analog-surround-51",
     8 },

    {{ 6, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_CENTER,
            PA_CHANNEL_POSITION_FRONT_RIGHT, PA_CHANNEL_POSITION_REAR_LEFT,
            PA_CHANNEL_POSITION_REAR_RIGHT, PA_CHANNEL_POSITION_LFE}},
     "a52",
     N_("Digital Surround 5.1 (IEC958/AC3)"),
     "iec958-ac3-surround-51",
     3 },

    {{ 8, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_CENTER, PA_CHANNEL_POSITION_LFE,
            PA_CHANNEL_POSITION_SIDE_LEFT, PA_CHANNEL_POSITION_SIDE_RIGHT }},
     "surround71",
     N_("Analog Surround 7.1"),
     "analog-surround-71",
     7 },

    {{ 0, { 0 }}, NULL, NULL, NULL, 0 }
};

snd_pcm_t *pa_alsa_open_by_device_id_auto(
        const char *dev_id,
        char **dev,
        pa_sample_spec *ss,
        pa_channel_map* map,
        int mode,
        uint32_t *nfrags,
        snd_pcm_uframes_t *period_size,
        snd_pcm_uframes_t tsched_size,
        pa_bool_t *use_mmap,
        pa_bool_t *use_tsched,
        const pa_alsa_profile_info **profile) {

    int i;
    int direction = 1;
    char *d;
    snd_pcm_t *pcm_handle;

    pa_assert(dev_id);
    pa_assert(dev);
    pa_assert(ss);
    pa_assert(map);
    pa_assert(nfrags);
    pa_assert(period_size);

    /* First we try to find a device string with a superset of the
     * requested channel map and open it without the plug: prefix. We
     * iterate through our device table from top to bottom and take
     * the first that matches. If we didn't find a working device that
     * way, we iterate backwards, and check all devices that do not
     * provide a superset of the requested channel map.*/

    i = 0;
    for (;;) {

        if ((direction > 0) == pa_channel_map_superset(&device_table[i].map, map)) {
            pa_sample_spec try_ss;

            pa_log_debug("Checking for %s (%s)", device_table[i].name, device_table[i].alsa_name);

            d = pa_sprintf_malloc("%s:%s", device_table[i].alsa_name, dev_id);

            try_ss.channels = device_table[i].map.channels;
            try_ss.rate = ss->rate;
            try_ss.format = ss->format;

            pcm_handle = pa_alsa_open_by_device_string(
                    d,
                    dev,
                    &try_ss,
                    map,
                    mode,
                    nfrags,
                    period_size,
                    tsched_size,
                    use_mmap,
                    use_tsched,
                    TRUE);

            pa_xfree(d);

            if (pcm_handle) {

                *ss = try_ss;
                *map = device_table[i].map;
                pa_assert(map->channels == ss->channels);

                if (profile)
                    *profile = &device_table[i];

                return pcm_handle;
            }
        }

        if (direction > 0) {
            if (!device_table[i+1].alsa_name) {
                /* OK, so we are at the end of our list. Let's turn
                 * back. */
                direction = -1;
            } else {
                /* We are not at the end of the list, so let's simply
                 * try the next entry */
                i++;
            }
        }

        if (direction < 0) {

            if (device_table[i+1].alsa_name &&
                device_table[i].map.channels == device_table[i+1].map.channels) {

                /* OK, the next entry has the same number of channels,
                 * let's try it */
                i++;

            } else {
                /* Hmm, so the next entry does not have the same
                 * number of channels, so let's go backwards until we
                 * find the next entry with a differnt number of
                 * channels */

                for (i--; i >= 0; i--)
                    if (device_table[i].map.channels != device_table[i+1].map.channels)
                        break;

                /* Hmm, there is no entry with a different number of
                 * entries, then we're done */
                if (i < 0)
                    break;

                /* OK, now lets find go back as long as we have the same number of channels */
                for (; i > 0; i--)
                    if (device_table[i].map.channels != device_table[i-1].map.channels)
                        break;
            }
        }
    }

    /* OK, we didn't find any good device, so let's try the raw plughw: stuff */

    d = pa_sprintf_malloc("hw:%s", dev_id);
    pa_log_debug("Trying %s as last resort...", d);
    pcm_handle = pa_alsa_open_by_device_string(d, dev, ss, map, mode, nfrags, period_size, tsched_size, use_mmap, use_tsched, FALSE);
    pa_xfree(d);

    if (pcm_handle && profile)
        *profile = NULL;

    return pcm_handle;
}

snd_pcm_t *pa_alsa_open_by_device_id_profile(
        const char *dev_id,
        char **dev,
        pa_sample_spec *ss,
        pa_channel_map* map,
        int mode,
        uint32_t *nfrags,
        snd_pcm_uframes_t *period_size,
        snd_pcm_uframes_t tsched_size,
        pa_bool_t *use_mmap,
        pa_bool_t *use_tsched,
        const pa_alsa_profile_info *profile) {

    char *d;
    snd_pcm_t *pcm_handle;
    pa_sample_spec try_ss;

    pa_assert(dev_id);
    pa_assert(dev);
    pa_assert(ss);
    pa_assert(map);
    pa_assert(nfrags);
    pa_assert(period_size);
    pa_assert(profile);

    d = pa_sprintf_malloc("%s:%s", profile->alsa_name, dev_id);

    try_ss.channels = profile->map.channels;
    try_ss.rate = ss->rate;
    try_ss.format = ss->format;

    pcm_handle = pa_alsa_open_by_device_string(
            d,
            dev,
            &try_ss,
            map,
            mode,
            nfrags,
            period_size,
            tsched_size,
            use_mmap,
            use_tsched,
            TRUE);

    pa_xfree(d);

    if (!pcm_handle)
        return NULL;

    *ss = try_ss;
    *map = profile->map;
    pa_assert(map->channels == ss->channels);

    return pcm_handle;
}

snd_pcm_t *pa_alsa_open_by_device_string(
        const char *device,
        char **dev,
        pa_sample_spec *ss,
        pa_channel_map* map,
        int mode,
        uint32_t *nfrags,
        snd_pcm_uframes_t *period_size,
        snd_pcm_uframes_t tsched_size,
        pa_bool_t *use_mmap,
        pa_bool_t *use_tsched,
        pa_bool_t require_exact_channel_number) {

    int err;
    char *d;
    snd_pcm_t *pcm_handle;
    pa_bool_t reformat = FALSE;

    pa_assert(device);
    pa_assert(ss);
    pa_assert(map);

    d = pa_xstrdup(device);

    for (;;) {
        pa_log_debug("Trying %s %s SND_PCM_NO_AUTO_FORMAT ...", d, reformat ? "without" : "with");

        /* We don't pass SND_PCM_NONBLOCK here, since alsa-lib <=
         * 1.0.17a would then ignore the SND_PCM_NO_xxx flags. Instead
         * we enable nonblock mode afterwards via
         * snd_pcm_nonblock(). Also see
         * http://mailman.alsa-project.org/pipermail/alsa-devel/2008-August/010258.html */

        if ((err = snd_pcm_open(&pcm_handle, d, mode,
                                /*SND_PCM_NONBLOCK|*/
                                SND_PCM_NO_AUTO_RESAMPLE|
                                SND_PCM_NO_AUTO_CHANNELS|
                                (reformat ? 0 : SND_PCM_NO_AUTO_FORMAT))) < 0) {
            pa_log_info("Error opening PCM device %s: %s", d, snd_strerror(err));
            goto fail;
        }

        if ((err = pa_alsa_set_hw_params(pcm_handle, ss, nfrags, period_size, tsched_size, use_mmap, use_tsched, require_exact_channel_number)) < 0) {

            if (!reformat) {
                reformat = TRUE;

                snd_pcm_close(pcm_handle);
                continue;
            }

            /* Hmm, some hw is very exotic, so we retry with plug, if without it didn't work */

            if (!pa_startswith(d, "plug:") && !pa_startswith(d, "plughw:")) {
                char *t;

                t = pa_sprintf_malloc("plug:%s", d);
                pa_xfree(d);
                d = t;

                reformat = FALSE;

                snd_pcm_close(pcm_handle);
                continue;
            }

            pa_log_info("Failed to set hardware parameters on %s: %s", d, snd_strerror(err));
            snd_pcm_close(pcm_handle);

            goto fail;
        }

        if (dev)
            *dev = d;
        else
            pa_xfree(d);

        if (ss->channels != map->channels)
            pa_channel_map_init_extend(map, ss->channels, PA_CHANNEL_MAP_ALSA);

        return pcm_handle;
    }

fail:
    pa_xfree(d);

    return NULL;
}

int pa_alsa_probe_profiles(
        const char *dev_id,
        const pa_sample_spec *ss,
        void (*cb)(const pa_alsa_profile_info *sink, const pa_alsa_profile_info *source, void *userdata),
        void *userdata) {

    const pa_alsa_profile_info *i;

    pa_assert(dev_id);
    pa_assert(ss);
    pa_assert(cb);

    /* We try each combination of playback/capture. We also try to
     * open only for capture resp. only for sink. Don't get confused
     * by the trailing entry in device_table we use for this! */

    for (i = device_table; i < device_table + PA_ELEMENTSOF(device_table); i++) {
        const pa_alsa_profile_info *j;
        snd_pcm_t *pcm_i = NULL;

        if (i->alsa_name) {
            char *id;
            pa_sample_spec try_ss;
            pa_channel_map try_map;

            pa_log_debug("Checking for playback on %s (%s)", i->name, i->alsa_name);
            id = pa_sprintf_malloc("%s:%s", i->alsa_name, dev_id);

            try_ss = *ss;
            try_ss.channels = i->map.channels;
            try_map = i->map;

            pcm_i = pa_alsa_open_by_device_string(
                    id, NULL,
                    &try_ss, &try_map,
                    SND_PCM_STREAM_PLAYBACK,
                    NULL, NULL, 0, NULL, NULL,
                    TRUE);

            pa_xfree(id);

            if (!pcm_i)
                continue;
        }

        for (j = device_table; j < device_table + PA_ELEMENTSOF(device_table); j++) {
            snd_pcm_t *pcm_j = NULL;

            if (j->alsa_name) {
                char *jd;
                pa_sample_spec try_ss;
                pa_channel_map try_map;

                pa_log_debug("Checking for capture on %s (%s)", j->name, j->alsa_name);
                jd = pa_sprintf_malloc("%s:%s", j->alsa_name, dev_id);

                try_ss = *ss;
                try_ss.channels = j->map.channels;
                try_map = j->map;

                pcm_j = pa_alsa_open_by_device_string(
                        jd, NULL,
                        &try_ss, &try_map,
                        SND_PCM_STREAM_CAPTURE,
                        NULL, NULL, 0, NULL, NULL,
                        TRUE);

                pa_xfree(jd);

                if (!pcm_j)
                    continue;
            }

            if (pcm_j)
                snd_pcm_close(pcm_j);

            if (i->alsa_name || j->alsa_name)
                cb(i->alsa_name ? i : NULL,
                   j->alsa_name ? j : NULL, userdata);
        }

        if (pcm_i)
            snd_pcm_close(pcm_i);
    }

    return TRUE;
}

int pa_alsa_prepare_mixer(snd_mixer_t *mixer, const char *dev) {
    int err;

    pa_assert(mixer);
    pa_assert(dev);

    if ((err = snd_mixer_attach(mixer, dev)) < 0) {
        pa_log_info("Unable to attach to mixer %s: %s", dev, snd_strerror(err));
        return -1;
    }

    if ((err = snd_mixer_selem_register(mixer, NULL, NULL)) < 0) {
        pa_log_warn("Unable to register mixer: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_mixer_load(mixer)) < 0) {
        pa_log_warn("Unable to load mixer: %s", snd_strerror(err));
        return -1;
    }

    pa_log_info("Successfully attached to mixer '%s'", dev);

    return 0;
}

static pa_bool_t elem_has_volume(snd_mixer_elem_t *elem, pa_bool_t playback) {
    pa_assert(elem);

    if (playback && snd_mixer_selem_has_playback_volume(elem))
        return TRUE;

    if (!playback && snd_mixer_selem_has_capture_volume(elem))
        return TRUE;

    return FALSE;
}

static pa_bool_t elem_has_switch(snd_mixer_elem_t *elem, pa_bool_t playback) {
    pa_assert(elem);

    if (playback && snd_mixer_selem_has_playback_switch(elem))
        return TRUE;

    if (!playback && snd_mixer_selem_has_capture_switch(elem))
        return TRUE;

    return FALSE;
}

snd_mixer_elem_t *pa_alsa_find_elem(snd_mixer_t *mixer, const char *name, const char *fallback, pa_bool_t playback) {
    snd_mixer_elem_t *elem = NULL, *fallback_elem = NULL;
    snd_mixer_selem_id_t *sid = NULL;

    snd_mixer_selem_id_alloca(&sid);

    pa_assert(mixer);
    pa_assert(name);

    snd_mixer_selem_id_set_name(sid, name);
    snd_mixer_selem_id_set_index(sid, 0);

    if ((elem = snd_mixer_find_selem(mixer, sid))) {

        if (elem_has_volume(elem, playback) &&
            elem_has_switch(elem, playback))
            goto success;

        if (!elem_has_volume(elem, playback) &&
            !elem_has_switch(elem, playback))
            elem = NULL;
    }

    pa_log_info("Cannot find mixer control \"%s\" or mixer control is no combination of switch/volume.", snd_mixer_selem_id_get_name(sid));

    if (fallback) {
        snd_mixer_selem_id_set_name(sid, fallback);
        snd_mixer_selem_id_set_index(sid, 0);

        if ((fallback_elem = snd_mixer_find_selem(mixer, sid))) {

            if (elem_has_volume(fallback_elem, playback) &&
                elem_has_switch(fallback_elem, playback)) {
                elem = fallback_elem;
                goto success;
            }

            if (!elem_has_volume(fallback_elem, playback) &&
                !elem_has_switch(fallback_elem, playback))
                fallback_elem = NULL;
        }

        pa_log_warn("Cannot find fallback mixer control \"%s\" or mixer control is no combination of switch/volume.", snd_mixer_selem_id_get_name(sid));
    }

    if (elem && fallback_elem) {

        /* Hmm, so we have both elements, but neither has both mute
         * and volume. Let's prefer the one with the volume */

        if (elem_has_volume(elem, playback))
            goto success;

        if (elem_has_volume(fallback_elem, playback)) {
            elem = fallback_elem;
            goto success;
        }
    }

    if (!elem && fallback_elem)
        elem = fallback_elem;

success:

    if (elem)
        pa_log_info("Using mixer control \"%s\".", snd_mixer_selem_id_get_name(sid));

    return elem;
}

int pa_alsa_find_mixer_and_elem(
        snd_pcm_t *pcm,
        snd_mixer_t **_m,
        snd_mixer_elem_t **_e) {

    int err;
    snd_mixer_t *m;
    snd_mixer_elem_t *e;
    pa_bool_t found = FALSE;
    const char *dev;

    pa_assert(pcm);
    pa_assert(_m);
    pa_assert(_e);

    if ((err = snd_mixer_open(&m, 0)) < 0) {
        pa_log("Error opening mixer: %s", snd_strerror(err));
        return -1;
    }

    /* First, try by name */
    if ((dev = snd_pcm_name(pcm)))
        if (pa_alsa_prepare_mixer(m, dev) >= 0)
            found = TRUE;

    /* Then, try by card index */
    if (!found) {
        snd_pcm_info_t* info;
        snd_pcm_info_alloca(&info);

        if (snd_pcm_info(pcm, info) >= 0) {
            char *md;
            int card_idx;

            if ((card_idx = snd_pcm_info_get_card(info)) >= 0) {

                md = pa_sprintf_malloc("hw:%i", card_idx);

                if (!dev || !pa_streq(dev, md))
                    if (pa_alsa_prepare_mixer(m, md) >= 0)
                        found = TRUE;

                pa_xfree(md);
            }
        }
    }

    if (!found) {
        snd_mixer_close(m);
        return -1;
    }

    switch (snd_pcm_stream(pcm)) {

        case SND_PCM_STREAM_PLAYBACK:
            e = pa_alsa_find_elem(m, "Master", "PCM", TRUE);
            break;

        case SND_PCM_STREAM_CAPTURE:
            e = pa_alsa_find_elem(m, "Capture", "Mic", FALSE);
            break;

        default:
            pa_assert_not_reached();
    }

    if (!e) {
        snd_mixer_close(m);
        return -1;
    }

    pa_assert(e && m);

    *_m = m;
    *_e = e;

    return 0;
}

static const snd_mixer_selem_channel_id_t alsa_channel_ids[PA_CHANNEL_POSITION_MAX] = {
    [PA_CHANNEL_POSITION_MONO] = SND_MIXER_SCHN_MONO, /* The ALSA name is just an alias! */

    [PA_CHANNEL_POSITION_FRONT_CENTER] = SND_MIXER_SCHN_FRONT_CENTER,
    [PA_CHANNEL_POSITION_FRONT_LEFT] = SND_MIXER_SCHN_FRONT_LEFT,
    [PA_CHANNEL_POSITION_FRONT_RIGHT] = SND_MIXER_SCHN_FRONT_RIGHT,

    [PA_CHANNEL_POSITION_REAR_CENTER] = SND_MIXER_SCHN_REAR_CENTER,
    [PA_CHANNEL_POSITION_REAR_LEFT] = SND_MIXER_SCHN_REAR_LEFT,
    [PA_CHANNEL_POSITION_REAR_RIGHT] = SND_MIXER_SCHN_REAR_RIGHT,

    [PA_CHANNEL_POSITION_LFE] = SND_MIXER_SCHN_WOOFER,

    [PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = SND_MIXER_SCHN_UNKNOWN,

    [PA_CHANNEL_POSITION_SIDE_LEFT] = SND_MIXER_SCHN_SIDE_LEFT,
    [PA_CHANNEL_POSITION_SIDE_RIGHT] = SND_MIXER_SCHN_SIDE_RIGHT,

    [PA_CHANNEL_POSITION_AUX0] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX1] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX2] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX3] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX4] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX5] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX6] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX7] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX8] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX9] =  SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX10] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX11] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX12] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX13] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX14] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX15] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX16] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX17] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX18] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX19] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX20] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX21] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX22] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX23] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX24] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX25] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX26] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX27] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX28] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX29] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX30] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_AUX31] = SND_MIXER_SCHN_UNKNOWN,

    [PA_CHANNEL_POSITION_TOP_CENTER] = SND_MIXER_SCHN_UNKNOWN,

    [PA_CHANNEL_POSITION_TOP_FRONT_CENTER] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_TOP_FRONT_LEFT] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_TOP_FRONT_RIGHT] = SND_MIXER_SCHN_UNKNOWN,

    [PA_CHANNEL_POSITION_TOP_REAR_CENTER] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_TOP_REAR_LEFT] = SND_MIXER_SCHN_UNKNOWN,
    [PA_CHANNEL_POSITION_TOP_REAR_RIGHT] = SND_MIXER_SCHN_UNKNOWN
};


int pa_alsa_calc_mixer_map(snd_mixer_elem_t *elem, const pa_channel_map *channel_map, snd_mixer_selem_channel_id_t mixer_map[], pa_bool_t playback) {
    unsigned i;
    pa_bool_t alsa_channel_used[SND_MIXER_SCHN_LAST];
    pa_bool_t mono_used = FALSE;

    pa_assert(elem);
    pa_assert(channel_map);
    pa_assert(mixer_map);

    memset(&alsa_channel_used, 0, sizeof(alsa_channel_used));

    if (channel_map->channels > 1 &&
        ((playback && snd_mixer_selem_has_playback_volume_joined(elem)) ||
         (!playback && snd_mixer_selem_has_capture_volume_joined(elem)))) {
        pa_log_info("ALSA device lacks independant volume controls for each channel.");
        return -1;
    }

    for (i = 0; i < channel_map->channels; i++) {
        snd_mixer_selem_channel_id_t id;
        pa_bool_t is_mono;

        is_mono = channel_map->map[i] == PA_CHANNEL_POSITION_MONO;
        id = alsa_channel_ids[channel_map->map[i]];

        if (!is_mono && id == SND_MIXER_SCHN_UNKNOWN) {
            pa_log_info("Configured channel map contains channel '%s' that is unknown to the ALSA mixer.", pa_channel_position_to_string(channel_map->map[i]));
            return -1;
        }

        if ((is_mono && mono_used) || (!is_mono && alsa_channel_used[id])) {
            pa_log_info("Channel map has duplicate channel '%s', falling back to software volume control.", pa_channel_position_to_string(channel_map->map[i]));
            return -1;
        }

        if ((playback && (!snd_mixer_selem_has_playback_channel(elem, id) || (is_mono && !snd_mixer_selem_is_playback_mono(elem)))) ||
            (!playback && (!snd_mixer_selem_has_capture_channel(elem, id) || (is_mono && !snd_mixer_selem_is_capture_mono(elem))))) {

            pa_log_info("ALSA device lacks separate volumes control for channel '%s'", pa_channel_position_to_string(channel_map->map[i]));
            return -1;
        }

        if (is_mono) {
            mixer_map[i] = SND_MIXER_SCHN_MONO;
            mono_used = TRUE;
        } else {
            mixer_map[i] = id;
            alsa_channel_used[id] = TRUE;
        }
    }

    pa_log_info("All %u channels can be mapped to mixer channels.", channel_map->channels);

    return 0;
}

void pa_alsa_dump(snd_pcm_t *pcm) {
    int err;
    snd_output_t *out;

    pa_assert(pcm);

    pa_assert_se(snd_output_buffer_open(&out) == 0);

    if ((err = snd_pcm_dump(pcm, out)) < 0)
        pa_log_debug("snd_pcm_dump(): %s", snd_strerror(err));
    else {
        char *s = NULL;
        snd_output_buffer_string(out, &s);
        pa_log_debug("snd_pcm_dump():\n%s", pa_strnull(s));
    }

    pa_assert_se(snd_output_close(out) == 0);
}

void pa_alsa_dump_status(snd_pcm_t *pcm) {
    int err;
    snd_output_t *out;
    snd_pcm_status_t *status;

    pa_assert(pcm);

    snd_pcm_status_alloca(&status);

    pa_assert_se(snd_output_buffer_open(&out) == 0);

    pa_assert_se(snd_pcm_status(pcm, status) == 0);

    if ((err = snd_pcm_status_dump(status, out)) < 0)
        pa_log_debug("snd_pcm_dump(): %s", snd_strerror(err));
    else {
        char *s = NULL;
        snd_output_buffer_string(out, &s);
        pa_log_debug("snd_pcm_dump():\n%s", pa_strnull(s));
    }

    pa_assert_se(snd_output_close(out) == 0);
}

static void alsa_error_handler(const char *file, int line, const char *function, int err, const char *fmt,...) {
    va_list ap;
    char *alsa_file;

    alsa_file = pa_sprintf_malloc("(alsa-lib)%s", file);

    va_start(ap, fmt);

    pa_log_levelv_meta(PA_LOG_INFO, alsa_file, line, function, fmt, ap);

    va_end(ap);

    pa_xfree(alsa_file);
}

static pa_atomic_t n_error_handler_installed = PA_ATOMIC_INIT(0);

void pa_alsa_redirect_errors_inc(void) {
    /* This is not really thread safe, but we do our best */

    if (pa_atomic_inc(&n_error_handler_installed) == 0)
        snd_lib_error_set_handler(alsa_error_handler);
}

void pa_alsa_redirect_errors_dec(void) {
    int r;

    pa_assert_se((r = pa_atomic_dec(&n_error_handler_installed)) >= 1);

    if (r == 1)
        snd_lib_error_set_handler(NULL);
}

pa_bool_t pa_alsa_init_description(pa_proplist *p) {
    const char *s;
    pa_assert(p);

    if (pa_device_init_description(p))
        return TRUE;

    if ((s = pa_proplist_gets(p, "alsa.card_name"))) {
        pa_proplist_sets(p, PA_PROP_DEVICE_DESCRIPTION, s);
        return TRUE;
    }

    if ((s = pa_proplist_gets(p, "alsa.name"))) {
        pa_proplist_sets(p, PA_PROP_DEVICE_DESCRIPTION, s);
        return TRUE;
    }

    return FALSE;
}

void pa_alsa_init_proplist_card(pa_core *c, pa_proplist *p, int card) {
    char *cn, *lcn, *dn;

    pa_assert(p);
    pa_assert(card >= 0);

    pa_proplist_setf(p, "alsa.card", "%i", card);

    if (snd_card_get_name(card, &cn) >= 0) {
        pa_proplist_sets(p, "alsa.card_name", cn);
        free(cn);
    }

    if (snd_card_get_longname(card, &lcn) >= 0) {
        pa_proplist_sets(p, "alsa.long_card_name", lcn);
        free(lcn);
    }

    if ((dn = pa_alsa_get_driver_name(card))) {
        pa_proplist_sets(p, "alsa.driver_name", dn);
        pa_xfree(dn);
    }

#ifdef HAVE_UDEV
    pa_udev_get_info(c, p, card);
#endif

#ifdef HAVE_HAL
    pa_hal_get_info(c, p, card);
#endif
}

void pa_alsa_init_proplist_pcm_info(pa_core *c, pa_proplist *p, snd_pcm_info_t *pcm_info) {

    static const char * const alsa_class_table[SND_PCM_CLASS_LAST+1] = {
        [SND_PCM_CLASS_GENERIC] = "generic",
        [SND_PCM_CLASS_MULTI] = "multi",
        [SND_PCM_CLASS_MODEM] = "modem",
        [SND_PCM_CLASS_DIGITIZER] = "digitizer"
    };
    static const char * const class_table[SND_PCM_CLASS_LAST+1] = {
        [SND_PCM_CLASS_GENERIC] = "sound",
        [SND_PCM_CLASS_MULTI] = NULL,
        [SND_PCM_CLASS_MODEM] = "modem",
        [SND_PCM_CLASS_DIGITIZER] = NULL
    };
    static const char * const alsa_subclass_table[SND_PCM_SUBCLASS_LAST+1] = {
        [SND_PCM_SUBCLASS_GENERIC_MIX] = "generic-mix",
        [SND_PCM_SUBCLASS_MULTI_MIX] = "multi-mix"
    };

    snd_pcm_class_t class;
    snd_pcm_subclass_t subclass;
    const char *n, *id, *sdn;
    int card;

    pa_assert(p);
    pa_assert(pcm_info);

    pa_proplist_sets(p, PA_PROP_DEVICE_API, "alsa");

    class = snd_pcm_info_get_class(pcm_info);
    if (class <= SND_PCM_CLASS_LAST) {
        if (class_table[class])
            pa_proplist_sets(p, PA_PROP_DEVICE_CLASS, class_table[class]);
        if (alsa_class_table[class])
            pa_proplist_sets(p, "alsa.class", alsa_class_table[class]);
    }

    subclass = snd_pcm_info_get_subclass(pcm_info);
    if (subclass <= SND_PCM_SUBCLASS_LAST)
        if (alsa_subclass_table[subclass])
            pa_proplist_sets(p, "alsa.subclass", alsa_subclass_table[subclass]);

    if ((n = snd_pcm_info_get_name(pcm_info)))
        pa_proplist_sets(p, "alsa.name", n);

    if ((id = snd_pcm_info_get_id(pcm_info)))
        pa_proplist_sets(p, "alsa.id", id);

    pa_proplist_setf(p, "alsa.subdevice", "%u", snd_pcm_info_get_subdevice(pcm_info));
    if ((sdn = snd_pcm_info_get_subdevice_name(pcm_info)))
        pa_proplist_sets(p, "alsa.subdevice_name", sdn);

    pa_proplist_setf(p, "alsa.device", "%u", snd_pcm_info_get_device(pcm_info));

    if ((card = snd_pcm_info_get_card(pcm_info)) >= 0)
        pa_alsa_init_proplist_card(c, p, card);
}

void pa_alsa_init_proplist_pcm(pa_core *c, pa_proplist *p, snd_pcm_t *pcm) {
    snd_pcm_hw_params_t *hwparams;
    snd_pcm_info_t *info;
    int bits, err;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_info_alloca(&info);

    if ((err = snd_pcm_hw_params_current(pcm, hwparams)) < 0)
        pa_log_warn("Error fetching hardware parameter info: %s", snd_strerror(err));
    else {

        if ((bits = snd_pcm_hw_params_get_sbits(hwparams)) >= 0)
            pa_proplist_setf(p, "alsa.resolution_bits", "%i", bits);
    }

    if ((err = snd_pcm_info(pcm, info)) < 0)
        pa_log_warn("Error fetching PCM info: %s", snd_strerror(err));
    else
        pa_alsa_init_proplist_pcm_info(c, p, info);
}

int pa_alsa_recover_from_poll(snd_pcm_t *pcm, int revents) {
    snd_pcm_state_t state;
    int err;

    pa_assert(pcm);

    if (revents & POLLERR)
        pa_log_debug("Got POLLERR from ALSA");
    if (revents & POLLNVAL)
        pa_log_warn("Got POLLNVAL from ALSA");
    if (revents & POLLHUP)
        pa_log_warn("Got POLLHUP from ALSA");
    if (revents & POLLPRI)
        pa_log_warn("Got POLLPRI from ALSA");
    if (revents & POLLIN)
        pa_log_debug("Got POLLIN from ALSA");
    if (revents & POLLOUT)
        pa_log_debug("Got POLLOUT from ALSA");

    state = snd_pcm_state(pcm);
    pa_log_debug("PCM state is %s", snd_pcm_state_name(state));

    /* Try to recover from this error */

    switch (state) {

        case SND_PCM_STATE_XRUN:
            if ((err = snd_pcm_recover(pcm, -EPIPE, 1)) != 0) {
                pa_log_warn("Could not recover from POLLERR|POLLNVAL|POLLHUP and XRUN: %s", snd_strerror(err));
                return -1;
            }
            break;

        case SND_PCM_STATE_SUSPENDED:
            if ((err = snd_pcm_recover(pcm, -ESTRPIPE, 1)) != 0) {
                pa_log_warn("Could not recover from POLLERR|POLLNVAL|POLLHUP and SUSPENDED: %s", snd_strerror(err));
                return -1;
            }
            break;

        default:

            snd_pcm_drop(pcm);

            if ((err = snd_pcm_prepare(pcm)) < 0) {
                pa_log_warn("Could not recover from POLLERR|POLLNVAL|POLLHUP with snd_pcm_prepare(): %s", snd_strerror(err));
                return -1;
            }
            break;
    }

    return 0;
}

pa_rtpoll_item* pa_alsa_build_pollfd(snd_pcm_t *pcm, pa_rtpoll *rtpoll) {
    int n, err;
    struct pollfd *pollfd;
    pa_rtpoll_item *item;

    pa_assert(pcm);

    if ((n = snd_pcm_poll_descriptors_count(pcm)) < 0) {
        pa_log("snd_pcm_poll_descriptors_count() failed: %s", snd_strerror(n));
        return NULL;
    }

    item = pa_rtpoll_item_new(rtpoll, PA_RTPOLL_NEVER, (unsigned) n);
    pollfd = pa_rtpoll_item_get_pollfd(item, NULL);

    if ((err = snd_pcm_poll_descriptors(pcm, pollfd, (unsigned) n)) < 0) {
        pa_log("snd_pcm_poll_descriptors() failed: %s", snd_strerror(err));
        pa_rtpoll_item_free(item);
        return NULL;
    }

    return item;
}

snd_pcm_sframes_t pa_alsa_safe_avail(snd_pcm_t *pcm, size_t hwbuf_size, const pa_sample_spec *ss) {
    snd_pcm_sframes_t n;
    size_t k;

    pa_assert(pcm);
    pa_assert(hwbuf_size > 0);
    pa_assert(ss);

    /* Some ALSA driver expose weird bugs, let's inform the user about
     * what is going on */

    n = snd_pcm_avail(pcm);

    if (n <= 0)
        return n;

    k = (size_t) n * pa_frame_size(ss);

    if (k >= hwbuf_size * 5 ||
        k >= pa_bytes_per_second(ss)*10) {

        PA_ONCE_BEGIN {
            char *dn = pa_alsa_get_driver_name_by_pcm(pcm);
            pa_log(_("snd_pcm_avail() returned a value that is exceptionally large: %lu bytes (%lu ms).\n"
                     "Most likely this is a bug in the ALSA driver '%s'. Please report this issue to the ALSA developers."),
                   (unsigned long) k,
                   (unsigned long) (pa_bytes_to_usec(k, ss) / PA_USEC_PER_MSEC),
                   pa_strnull(dn));
            pa_xfree(dn);
        } PA_ONCE_END;

        /* Mhmm, let's try not to fail completely */
        n = (snd_pcm_sframes_t) (hwbuf_size / pa_frame_size(ss));
    }

    return n;
}

int pa_alsa_safe_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay, size_t hwbuf_size, const pa_sample_spec *ss) {
    ssize_t k;
    size_t abs_k;
    int r;

    pa_assert(pcm);
    pa_assert(delay);
    pa_assert(hwbuf_size > 0);
    pa_assert(ss);

    /* Some ALSA driver expose weird bugs, let's inform the user about
     * what is going on */

    if ((r = snd_pcm_delay(pcm, delay)) < 0)
        return r;

    k = (ssize_t) *delay * (ssize_t) pa_frame_size(ss);

    abs_k = k >= 0 ? (size_t) k : (size_t) -k;

    if (abs_k >= hwbuf_size * 5 ||
        abs_k >= pa_bytes_per_second(ss)*10) {

        PA_ONCE_BEGIN {
            char *dn = pa_alsa_get_driver_name_by_pcm(pcm);
            pa_log(_("snd_pcm_delay() returned a value that is exceptionally large: %li bytes (%s%lu ms).\n"
                     "Most likely this is a bug in the ALSA driver '%s'. Please report this issue to the ALSA developers."),
                   (signed long) k,
                   k < 0 ? "-" : "",
                   (unsigned long) (pa_bytes_to_usec(abs_k, ss) / PA_USEC_PER_MSEC),
                   pa_strnull(dn));
            pa_xfree(dn);
        } PA_ONCE_END;

        /* Mhmm, let's try not to fail completely */
        if (k < 0)
            *delay = -(snd_pcm_sframes_t) (hwbuf_size / pa_frame_size(ss));
        else
            *delay = (snd_pcm_sframes_t) (hwbuf_size / pa_frame_size(ss));
    }

    return 0;
}

int pa_alsa_safe_mmap_begin(snd_pcm_t *pcm, const snd_pcm_channel_area_t **areas, snd_pcm_uframes_t *offset, snd_pcm_uframes_t *frames, size_t hwbuf_size, const pa_sample_spec *ss) {
    int r;
    snd_pcm_uframes_t before;
    size_t k;

    pa_assert(pcm);
    pa_assert(areas);
    pa_assert(offset);
    pa_assert(frames);
    pa_assert(hwbuf_size > 0);
    pa_assert(ss);

    before = *frames;

    r = snd_pcm_mmap_begin(pcm, areas, offset, frames);

    if (r < 0)
        return r;

    k = (size_t) *frames * pa_frame_size(ss);

    if (*frames > before ||
        k >= hwbuf_size * 3 ||
        k >= pa_bytes_per_second(ss)*10)

        PA_ONCE_BEGIN {
            char *dn = pa_alsa_get_driver_name_by_pcm(pcm);
            pa_log(_("snd_pcm_mmap_begin() returned a value that is exceptionally large: %lu bytes (%lu ms).\n"
                     "Most likely this is a bug in the ALSA driver '%s'. Please report this issue to the ALSA developers."),
                   (unsigned long) k,
                   (unsigned long) (pa_bytes_to_usec(k, ss) / PA_USEC_PER_MSEC),
                   pa_strnull(dn));
            pa_xfree(dn);
        } PA_ONCE_END;

    return r;
}

char *pa_alsa_get_driver_name(int card) {
    char *t, *m, *n;

    pa_assert(card >= 0);

    t = pa_sprintf_malloc("/sys/class/sound/card%i/device/driver/module", card);
    m = pa_readlink(t);
    pa_xfree(t);

    if (!m)
        return NULL;

    n = pa_xstrdup(pa_path_get_filename(m));
    pa_xfree(m);

    return n;
}

char *pa_alsa_get_driver_name_by_pcm(snd_pcm_t *pcm) {
    int card;

    snd_pcm_info_t* info;
    snd_pcm_info_alloca(&info);

    if (snd_pcm_info(pcm, info) < 0)
        return NULL;

    if ((card = snd_pcm_info_get_card(info)) < 0)
        return NULL;

    return pa_alsa_get_driver_name(card);
}

char *pa_alsa_get_reserve_name(const char *device) {
    const char *t;
    int i;

    pa_assert(device);

    if ((t = strchr(device, ':')))
        device = t+1;

    if ((i = snd_card_get_index(device)) < 0) {
        int32_t k;

        if (pa_atoi(device, &k) < 0)
            return NULL;

        i = (int) k;
    }

    return pa_sprintf_malloc("Audio%i", i);
}
