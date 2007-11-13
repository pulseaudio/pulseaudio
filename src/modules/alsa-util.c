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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <asoundlib.h>

#include <pulse/sample.h>
#include <pulse/xmalloc.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>

#include "alsa-util.h"

struct pa_alsa_fdlist {
    int num_fds;
    struct pollfd *fds;
    /* This is a temporary buffer used to avoid lots of mallocs */
    struct pollfd *work_fds;

    snd_mixer_t *mixer;

    pa_mainloop_api *m;
    pa_defer_event *defer;
    pa_io_event **ios;

    int polled;

    void (*cb)(void *userdata);
    void *userdata;
};

static void io_cb(pa_mainloop_api*a, pa_io_event* e, PA_GCC_UNUSED int fd, pa_io_event_flags_t events, void *userdata) {

    struct pa_alsa_fdlist *fdl = userdata;
    int err, i;
    unsigned short revents;

    pa_assert(a);
    pa_assert(fdl);
    pa_assert(fdl->mixer);
    pa_assert(fdl->fds);
    pa_assert(fdl->work_fds);

    if (fdl->polled)
        return;

    fdl->polled = 1;

    memcpy(fdl->work_fds, fdl->fds, sizeof(struct pollfd) * fdl->num_fds);

    for (i = 0;i < fdl->num_fds; i++) {
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

static void defer_cb(pa_mainloop_api*a, PA_GCC_UNUSED pa_defer_event* e, void *userdata) {
    struct pa_alsa_fdlist *fdl = userdata;
    int num_fds, i, err;
    struct pollfd *temp;

    pa_assert(a);
    pa_assert(fdl);
    pa_assert(fdl->mixer);

    a->defer_enable(fdl->defer, 0);

    num_fds = snd_mixer_poll_descriptors_count(fdl->mixer);
    pa_assert(num_fds > 0);

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

    fdl->polled = 0;

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
    fdl->polled = 0;

    return fdl;
}

void pa_alsa_fdlist_free(struct pa_alsa_fdlist *fdl) {
    pa_assert(fdl);

    if (fdl->defer) {
        pa_assert(fdl->m);
        fdl->m->defer_free(fdl->defer);
    }

    if (fdl->ios) {
        int i;
        pa_assert(fdl->m);
        for (i = 0;i < fdl->num_fds;i++)
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
    };

    static const pa_sample_format_t try_order[] = {
        PA_SAMPLE_FLOAT32NE,
        PA_SAMPLE_FLOAT32RE,
        PA_SAMPLE_S32NE,
        PA_SAMPLE_S32RE,
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
        pa_bool_t *use_mmap,
        pa_bool_t require_exact_channel_number) {

    int ret = -1;
    snd_pcm_uframes_t buffer_size;
    unsigned int r = ss->rate;
    unsigned int c = ss->channels;
    pa_sample_format_t f = ss->format;
    snd_pcm_hw_params_t *hwparams;
    pa_bool_t _use_mmap = use_mmap && *use_mmap;

    pa_assert(pcm_handle);
    pa_assert(ss);
    pa_assert(periods);
    pa_assert(period_size);

    snd_pcm_hw_params_alloca(&hwparams);

    buffer_size = *periods * *period_size;

    if ((ret = snd_pcm_hw_params_any(pcm_handle, hwparams)) < 0 ||
        (ret = snd_pcm_hw_params_set_rate_resample(pcm_handle, hwparams, 0)) < 0)
        goto finish;

    if (_use_mmap) {
        if ((ret = snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {

            /* mmap() didn't work, fall back to interleaved */

            if ((ret = snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
                goto finish;

            _use_mmap = FALSE;
        }

    } else if ((ret = snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        goto finish;

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

    if ((*period_size > 0 && (ret = snd_pcm_hw_params_set_period_size_near(pcm_handle, hwparams, period_size, NULL)) < 0) ||
        (*periods > 0 && (ret = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hwparams, &buffer_size)) < 0))
        goto finish;

    if  ((ret = snd_pcm_hw_params(pcm_handle, hwparams)) < 0)
        goto finish;

    if (ss->rate != r)
        pa_log_warn("Device %s doesn't support %u Hz, changed to %u Hz.", snd_pcm_name(pcm_handle), ss->rate, r);

    if (ss->channels != c)
        pa_log_warn("Device %s doesn't support %u channels, changed to %u.", snd_pcm_name(pcm_handle), ss->channels, c);

    if (ss->format != f)
        pa_log_warn("Device %s doesn't support sample format %s, changed to %s.", snd_pcm_name(pcm_handle), pa_sample_format_to_string(ss->format), pa_sample_format_to_string(f));

    if ((ret = snd_pcm_prepare(pcm_handle)) < 0)
        goto finish;

    if ((ret = snd_pcm_hw_params_get_buffer_size(hwparams, &buffer_size)) < 0 ||
        (ret = snd_pcm_hw_params_get_period_size(hwparams, period_size, NULL)) < 0)
        goto finish;

    /* If the sample rate deviates too much, we need to resample */
    if (r < ss->rate*.95 || r > ss->rate*1.05)
        ss->rate = r;
    ss->channels = c;
    ss->format = f;

    pa_assert(buffer_size > 0);
    pa_assert(*period_size > 0);
    *periods = buffer_size / *period_size;
    pa_assert(*periods > 0);

    if (use_mmap)
        *use_mmap = _use_mmap;

    ret = 0;

finish:

    return ret;
}

int pa_alsa_set_sw_params(snd_pcm_t *pcm) {
    snd_pcm_sw_params_t *swparams;
    int err;

    pa_assert(pcm);

    snd_pcm_sw_params_alloca(&swparams);

    if ((err = snd_pcm_sw_params_current(pcm, swparams) < 0)) {
        pa_log_warn("Unable to determine current swparams: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_stop_threshold(pcm, swparams, (snd_pcm_uframes_t) -1)) < 0) {
        pa_log_warn("Unable to set stop threshold: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_start_threshold(pcm, swparams, (snd_pcm_uframes_t) -1)) < 0) {
        pa_log_warn("Unable to set start threshold: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params(pcm, swparams)) < 0) {
        pa_log_warn("Unable to set sw params: %s\n", snd_strerror(err));
        return err;
    }

    return 0;
}

struct device_info {
    pa_channel_map map;
    const char *name;
};

static const struct device_info device_table[] = {
    {{ 2, { PA_CHANNEL_POSITION_LEFT, PA_CHANNEL_POSITION_RIGHT } }, "front" },

    {{ 4, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT }}, "surround40" },

    {{ 5, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_LFE }}, "surround41" },

    {{ 5, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_CENTER }}, "surround50" },

    {{ 6, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_CENTER, PA_CHANNEL_POSITION_LFE }}, "surround51" },

    {{ 8, { PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
            PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
            PA_CHANNEL_POSITION_CENTER, PA_CHANNEL_POSITION_LFE,
            PA_CHANNEL_POSITION_SIDE_LEFT, PA_CHANNEL_POSITION_SIDE_RIGHT }} , "surround71" },

    {{ 0, { 0 }}, NULL }
};

static pa_bool_t channel_map_superset(const pa_channel_map *a, const pa_channel_map *b) {
    pa_bool_t in_a[PA_CHANNEL_POSITION_MAX];
    unsigned i;

    pa_assert(a);
    pa_assert(b);

    memset(in_a, 0, sizeof(in_a));

    for (i = 0; i < a->channels; i++)
        in_a[a->map[i]] = TRUE;

    for (i = 0; i < b->channels; i++)
        if (!in_a[b->map[i]])
            return FALSE;

    return TRUE;
}

snd_pcm_t *pa_alsa_open_by_device_id(
        const char *dev_id,
        char **dev,
        pa_sample_spec *ss,
        pa_channel_map* map,
        int mode,
        uint32_t *nfrags,
        snd_pcm_uframes_t *period_size,
        pa_bool_t *use_mmap) {

    int i;
    int direction = 1;
    int err;
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

    for (i = 0;; i += direction) {
        pa_sample_spec try_ss;

        if (i < 0) {
            pa_assert(direction == -1);

            /* OK, so we iterated backwards, and now are at the
             * beginning of our list. */

            break;

        } else if (!device_table[i].name) {
            pa_assert(direction == 1);

            /* OK, so we are at the end of our list. at iterated
             * forwards. */

            i--;
            direction = -1;
        }

        if ((direction > 0) == !channel_map_superset(&device_table[i].map, map))
            continue;

        d = pa_sprintf_malloc("%s:%s", device_table[i].name, dev_id);
        pa_log_debug("Trying %s...", d);

        if ((err = snd_pcm_open(&pcm_handle, d, mode, SND_PCM_NONBLOCK)) < 0) {
            pa_log_info("Couldn't open PCM device %s: %s", d, snd_strerror(err));
            pa_xfree(d);
            continue;
        }

        try_ss.channels = device_table[i].map.channels;
        try_ss.rate = ss->rate;
        try_ss.format = ss->format;

        if ((err = pa_alsa_set_hw_params(pcm_handle, &try_ss, nfrags, period_size, use_mmap, TRUE)) < 0) {
            pa_log_info("PCM device %s refused our hw parameters: %s", d, snd_strerror(err));
            pa_xfree(d);
            snd_pcm_close(pcm_handle);
            continue;
        }

        *ss = try_ss;
        *map = device_table[i].map;
        pa_assert(map->channels == ss->channels);
        *dev = d;
        return pcm_handle;
    }

    /* OK, we didn't find any good device, so let's try the raw hw: stuff */

    d = pa_sprintf_malloc("hw:%s", dev_id);
    pa_log_debug("Trying %s as last resort...", d);
    pcm_handle = pa_alsa_open_by_device_string(d, dev, ss, map, mode, nfrags, period_size, use_mmap);
    pa_xfree(d);

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
        pa_bool_t *use_mmap) {

    int err;
    char *d;
    snd_pcm_t *pcm_handle;

    pa_assert(device);
    pa_assert(dev);
    pa_assert(ss);
    pa_assert(map);
    pa_assert(nfrags);
    pa_assert(period_size);

    d = pa_xstrdup(device);

    for (;;) {

        if ((err = snd_pcm_open(&pcm_handle, d, mode, SND_PCM_NONBLOCK)) < 0) {
            pa_log("Error opening PCM device %s: %s", d, snd_strerror(err));
            pa_xfree(d);
            return NULL;
        }

        if ((err = pa_alsa_set_hw_params(pcm_handle, ss, nfrags, period_size, use_mmap, FALSE)) < 0) {

            if (err == -EPERM) {
                /* Hmm, some hw is very exotic, so we retry with plug, if without it didn't work */

                if (pa_startswith(d, "hw:")) {
                    char *t = pa_sprintf_malloc("plughw:%s", d+3);
                    pa_log_debug("Opening the device as '%s' didn't work, retrying with '%s'.", d, t);
                    pa_xfree(d);
                    d = t;

                    snd_pcm_close(pcm_handle);
                    continue;
                }

                pa_log("Failed to set hardware parameters on %s: %s", d, snd_strerror(err));
                pa_xfree(d);
                snd_pcm_close(pcm_handle);
                return NULL;
            }
        }

        *dev = d;

        if (ss->channels != map->channels)
            pa_channel_map_init_auto(map, ss->channels, PA_CHANNEL_MAP_ALSA);

        return pcm_handle;
    }
}

int pa_alsa_prepare_mixer(snd_mixer_t *mixer, const char *dev) {
    int err;

    pa_assert(mixer);
    pa_assert(dev);

    if ((err = snd_mixer_attach(mixer, dev)) < 0) {
        pa_log_warn("Unable to attach to mixer %s: %s", dev, snd_strerror(err));
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

    return 0;
}

snd_mixer_elem_t *pa_alsa_find_elem(snd_mixer_t *mixer, const char *name, const char *fallback) {
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid = NULL;

    snd_mixer_selem_id_alloca(&sid);

    pa_assert(mixer);
    pa_assert(name);

    snd_mixer_selem_id_set_name(sid, name);

    if (!(elem = snd_mixer_find_selem(mixer, sid))) {
        pa_log_warn("Cannot find mixer control \"%s\".", snd_mixer_selem_id_get_name(sid));

        if (fallback) {
            snd_mixer_selem_id_set_name(sid, fallback);

            if (!(elem = snd_mixer_find_selem(mixer, sid)))
                pa_log_warn("Cannot find fallback mixer control \"%s\".", snd_mixer_selem_id_get_name(sid));
        }
    }

    if (elem)
        pa_log_info("Using mixer control \"%s\".", snd_mixer_selem_id_get_name(sid));

    return elem;
}
