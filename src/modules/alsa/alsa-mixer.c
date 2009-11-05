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

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/sample.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/i18n.h>
#include <pulse/utf8.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/atomic.h>
#include <pulsecore/core-error.h>
#include <pulsecore/once.h>
#include <pulsecore/thread.h>
#include <pulsecore/conf-parser.h>
#include <pulsecore/strbuf.h>

#include "alsa-mixer.h"
#include "alsa-util.h"

struct description_map {
    const char *name;
    const char *description;
};

static const char *lookup_description(const char *name, const struct description_map dm[], unsigned n) {
    unsigned i;

    for (i = 0; i < n; i++)
        if (pa_streq(dm[i].name, name))
            return dm[i].description;

    return NULL;
}

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
        pa_log_error("Unable to get poll revent: %s", pa_alsa_strerror(err));
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
        pa_log("snd_mixer_poll_descriptors_count() failed: %s", pa_alsa_strerror(n));
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
        pa_log_error("Unable to get poll descriptors: %s", pa_alsa_strerror(err));
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

static int prepare_mixer(snd_mixer_t *mixer, const char *dev) {
    int err;

    pa_assert(mixer);
    pa_assert(dev);

    if ((err = snd_mixer_attach(mixer, dev)) < 0) {
        pa_log_info("Unable to attach to mixer %s: %s", dev, pa_alsa_strerror(err));
        return -1;
    }

    if ((err = snd_mixer_selem_register(mixer, NULL, NULL)) < 0) {
        pa_log_warn("Unable to register mixer: %s", pa_alsa_strerror(err));
        return -1;
    }

    if ((err = snd_mixer_load(mixer)) < 0) {
        pa_log_warn("Unable to load mixer: %s", pa_alsa_strerror(err));
        return -1;
    }

    pa_log_info("Successfully attached to mixer '%s'", dev);
    return 0;
}

snd_mixer_t *pa_alsa_open_mixer_for_pcm(snd_pcm_t *pcm, char **ctl_device) {
    int err;
    snd_mixer_t *m;
    const char *dev;
    snd_pcm_info_t* info;
    snd_pcm_info_alloca(&info);

    pa_assert(pcm);

    if ((err = snd_mixer_open(&m, 0)) < 0) {
        pa_log("Error opening mixer: %s", pa_alsa_strerror(err));
        return NULL;
    }

    /* First, try by name */
    if ((dev = snd_pcm_name(pcm)))
        if (prepare_mixer(m, dev) >= 0) {
            if (ctl_device)
                *ctl_device = pa_xstrdup(dev);

            return m;
        }

    /* Then, try by card index */
    if (snd_pcm_info(pcm, info) >= 0) {
        char *md;
        int card_idx;

        if ((card_idx = snd_pcm_info_get_card(info)) >= 0) {

            md = pa_sprintf_malloc("hw:%i", card_idx);

            if (!dev || !pa_streq(dev, md))
                if (prepare_mixer(m, md) >= 0) {

                    if (ctl_device)
                        *ctl_device = md;
                    else
                        pa_xfree(md);

                    return m;
                }

            pa_xfree(md);
        }
    }

    snd_mixer_close(m);
    return NULL;
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

static void setting_free(pa_alsa_setting *s) {
    pa_assert(s);

    if (s->options)
        pa_idxset_free(s->options, NULL, NULL);

    pa_xfree(s->name);
    pa_xfree(s->description);
    pa_xfree(s);
}

static void option_free(pa_alsa_option *o) {
    pa_assert(o);

    pa_xfree(o->alsa_name);
    pa_xfree(o->name);
    pa_xfree(o->description);
    pa_xfree(o);
}

static void element_free(pa_alsa_element *e) {
    pa_alsa_option *o;
    pa_assert(e);

    while ((o = e->options)) {
        PA_LLIST_REMOVE(pa_alsa_option, e->options, o);
        option_free(o);
    }

    pa_xfree(e->alsa_name);
    pa_xfree(e);
}

void pa_alsa_path_free(pa_alsa_path *p) {
    pa_alsa_element *e;
    pa_alsa_setting *s;

    pa_assert(p);

    while ((e = p->elements)) {
        PA_LLIST_REMOVE(pa_alsa_element, p->elements, e);
        element_free(e);
    }

    while ((s = p->settings)) {
        PA_LLIST_REMOVE(pa_alsa_setting, p->settings, s);
        setting_free(s);
    }

    pa_xfree(p->name);
    pa_xfree(p->description);
    pa_xfree(p);
}

void pa_alsa_path_set_free(pa_alsa_path_set *ps) {
    pa_alsa_path *p;
    pa_assert(ps);

    while ((p = ps->paths)) {
        PA_LLIST_REMOVE(pa_alsa_path, ps->paths, p);
        pa_alsa_path_free(p);
    }

    pa_xfree(ps);
}

static long to_alsa_dB(pa_volume_t v) {
    return (long) (pa_sw_volume_to_dB(v) * 100.0);
}

static pa_volume_t from_alsa_dB(long v) {
    return pa_sw_volume_from_dB((double) v / 100.0);
}

static long to_alsa_volume(pa_volume_t v, long min, long max) {
    long w;

    w = (long) round(((double) v * (double) (max - min)) / PA_VOLUME_NORM) + min;
    return PA_CLAMP_UNLIKELY(w, min, max);
}

static pa_volume_t from_alsa_volume(long v, long min, long max) {
    return (pa_volume_t) round(((double) (v - min) * PA_VOLUME_NORM) / (double) (max - min));
}

#define SELEM_INIT(sid, name)                           \
    do {                                                \
        snd_mixer_selem_id_alloca(&(sid));              \
        snd_mixer_selem_id_set_name((sid), (name));     \
        snd_mixer_selem_id_set_index((sid), 0);         \
    } while(FALSE)

static int element_get_volume(pa_alsa_element *e, snd_mixer_t *m, const pa_channel_map *cm, pa_cvolume *v) {
    snd_mixer_selem_id_t *sid;
    snd_mixer_elem_t *me;
    snd_mixer_selem_channel_id_t c;
    pa_channel_position_mask_t mask = 0;
    unsigned k;

    pa_assert(m);
    pa_assert(e);
    pa_assert(cm);
    pa_assert(v);

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return -1;
    }

    pa_cvolume_mute(v, cm->channels);

    /* We take the highest volume of all channels that match */

    for (c = 0; c <= SND_MIXER_SCHN_LAST; c++) {
        int r;
        pa_volume_t f;

        if (e->has_dB) {
            long value = 0;

            if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {
                if (snd_mixer_selem_has_playback_channel(me, c))
                    r = snd_mixer_selem_get_playback_dB(me, c, &value);
                else
                    r = -1;
            } else {
                if (snd_mixer_selem_has_capture_channel(me, c))
                    r = snd_mixer_selem_get_capture_dB(me, c, &value);
                else
                    r = -1;
            }

            if (r < 0)
                continue;

#ifdef HAVE_VALGRIND_MEMCHECK_H
                VALGRIND_MAKE_MEM_DEFINED(&value, sizeof(value));
#endif

            f = from_alsa_dB(value);

        } else {
            long value = 0;

            if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {
                if (snd_mixer_selem_has_playback_channel(me, c))
                    r = snd_mixer_selem_get_playback_volume(me, c, &value);
                else
                    r = -1;
            } else {
                if (snd_mixer_selem_has_capture_channel(me, c))
                    r = snd_mixer_selem_get_capture_volume(me, c, &value);
                else
                    r = -1;
            }

            if (r < 0)
                continue;

            f = from_alsa_volume(value, e->min_volume, e->max_volume);
        }

        for (k = 0; k < cm->channels; k++)
            if (e->masks[c][e->n_channels-1] & PA_CHANNEL_POSITION_MASK(cm->map[k]))
                if (v->values[k] < f)
                    v->values[k] = f;

        mask |= e->masks[c][e->n_channels-1];
    }

    for (k = 0; k < cm->channels; k++)
        if (!(mask & PA_CHANNEL_POSITION_MASK(cm->map[k])))
            v->values[k] = PA_VOLUME_NORM;

    return 0;
}

int pa_alsa_path_get_volume(pa_alsa_path *p, snd_mixer_t *m, const pa_channel_map *cm, pa_cvolume *v) {
    pa_alsa_element *e;

    pa_assert(m);
    pa_assert(p);
    pa_assert(cm);
    pa_assert(v);

    if (!p->has_volume)
        return -1;

    pa_cvolume_reset(v, cm->channels);

    PA_LLIST_FOREACH(e, p->elements) {
        pa_cvolume ev;

        if (e->volume_use != PA_ALSA_VOLUME_MERGE)
            continue;

        pa_assert(!p->has_dB || e->has_dB);

        if (element_get_volume(e, m, cm, &ev) < 0)
            return -1;

        /* If we have no dB information all we can do is take the first element and leave */
        if (!p->has_dB) {
            *v = ev;
            return 0;
        }

        pa_sw_cvolume_multiply(v, v, &ev);
    }

    return 0;
}

static int element_get_switch(pa_alsa_element *e, snd_mixer_t *m, pa_bool_t *b) {
    snd_mixer_selem_id_t *sid;
    snd_mixer_elem_t *me;
    snd_mixer_selem_channel_id_t c;

    pa_assert(m);
    pa_assert(e);
    pa_assert(b);

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return -1;
    }

    /* We return muted if at least one channel is muted */

    for (c = 0; c <= SND_MIXER_SCHN_LAST; c++) {
        int r;
        int value = 0;

        if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {
            if (snd_mixer_selem_has_playback_channel(me, c))
                r = snd_mixer_selem_get_playback_switch(me, c, &value);
            else
                r = -1;
        } else {
            if (snd_mixer_selem_has_capture_channel(me, c))
                r = snd_mixer_selem_get_capture_switch(me, c, &value);
            else
                r = -1;
        }

        if (r < 0)
            continue;

        if (!value) {
            *b = FALSE;
            return 0;
        }
    }

    *b = TRUE;
    return 0;
}

int pa_alsa_path_get_mute(pa_alsa_path *p, snd_mixer_t *m, pa_bool_t *muted) {
    pa_alsa_element *e;

    pa_assert(m);
    pa_assert(p);
    pa_assert(muted);

    if (!p->has_mute)
        return -1;

    PA_LLIST_FOREACH(e, p->elements) {
        pa_bool_t b;

        if (e->switch_use != PA_ALSA_SWITCH_MUTE)
            continue;

        if (element_get_switch(e, m, &b) < 0)
            return -1;

        if (!b) {
            *muted = TRUE;
            return 0;
        }
    }

    *muted = FALSE;
    return 0;
}

static int element_set_volume(pa_alsa_element *e, snd_mixer_t *m, const pa_channel_map *cm, pa_cvolume *v) {
    snd_mixer_selem_id_t *sid;
    pa_cvolume rv;
    snd_mixer_elem_t *me;
    snd_mixer_selem_channel_id_t c;
    pa_channel_position_mask_t mask = 0;
    unsigned k;

    pa_assert(m);
    pa_assert(e);
    pa_assert(cm);
    pa_assert(v);
    pa_assert(pa_cvolume_compatible_with_channel_map(v, cm));

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return -1;
    }

    pa_cvolume_mute(&rv, cm->channels);

    for (c = 0; c <= SND_MIXER_SCHN_LAST; c++) {
        int r;
        pa_volume_t f = PA_VOLUME_MUTED;
        pa_bool_t found = FALSE;

        for (k = 0; k < cm->channels; k++)
            if (e->masks[c][e->n_channels-1] & PA_CHANNEL_POSITION_MASK(cm->map[k])) {
                found = TRUE;
                if (v->values[k] > f)
                    f = v->values[k];
            }

        if (!found) {
            /* Hmm, so this channel does not exist in the volume
             * struct, so let's bind it to the overall max of the
             * volume. */
            f = pa_cvolume_max(v);
        }

        if (e->has_dB) {
            long value = to_alsa_dB(f);

            if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {
                /* If we call set_play_volume() without checking first
                 * if the channel is available, ALSA behaves ver
                 * strangely and doesn't fail the call */
                if (snd_mixer_selem_has_playback_channel(me, c)) {
                    if ((r = snd_mixer_selem_set_playback_dB(me, c, value, +1)) >= 0)
                        r = snd_mixer_selem_get_playback_dB(me, c, &value);
                } else
                    r = -1;
            } else {
                if (snd_mixer_selem_has_capture_channel(me, c)) {
                    if ((r = snd_mixer_selem_set_capture_dB(me, c, value, +1)) >= 0)
                        r = snd_mixer_selem_get_capture_dB(me, c, &value);
                } else
                    r = -1;
            }

            if (r < 0)
                continue;

#ifdef HAVE_VALGRIND_MEMCHECK_H
            VALGRIND_MAKE_MEM_DEFINED(&value, sizeof(value));
#endif

            f = from_alsa_dB(value);

        } else {
            long value;

            value = to_alsa_volume(f, e->min_volume, e->max_volume);

            if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {
                if (snd_mixer_selem_has_playback_channel(me, c)) {
                    if ((r = snd_mixer_selem_set_playback_volume(me, c, value)) >= 0)
                        r = snd_mixer_selem_get_playback_volume(me, c, &value);
                } else
                    r = -1;
            } else {
                if (snd_mixer_selem_has_capture_channel(me, c)) {
                    if ((r = snd_mixer_selem_set_capture_volume(me, c, value)) >= 0)
                        r = snd_mixer_selem_get_capture_volume(me, c, &value);
                } else
                    r = -1;
            }

            if (r < 0)
                continue;

            f = from_alsa_volume(value, e->min_volume, e->max_volume);
        }

        for (k = 0; k < cm->channels; k++)
            if (e->masks[c][e->n_channels-1] & PA_CHANNEL_POSITION_MASK(cm->map[k]))
                if (rv.values[k] < f)
                    rv.values[k] = f;

        mask |= e->masks[c][e->n_channels-1];
    }

    for (k = 0; k < cm->channels; k++)
        if (!(mask & PA_CHANNEL_POSITION_MASK(cm->map[k])))
            rv.values[k] = PA_VOLUME_NORM;

    *v = rv;
    return 0;
}

int pa_alsa_path_set_volume(pa_alsa_path *p, snd_mixer_t *m, const pa_channel_map *cm, pa_cvolume *v) {
    pa_alsa_element *e;
    pa_cvolume rv;

    pa_assert(m);
    pa_assert(p);
    pa_assert(cm);
    pa_assert(v);
    pa_assert(pa_cvolume_compatible_with_channel_map(v, cm));

    if (!p->has_volume)
        return -1;

    rv = *v; /* Remaining adjustment */
    pa_cvolume_reset(v, cm->channels); /* Adjustment done */

    PA_LLIST_FOREACH(e, p->elements) {
        pa_cvolume ev;

        if (e->volume_use != PA_ALSA_VOLUME_MERGE)
            continue;

        pa_assert(!p->has_dB || e->has_dB);

        ev = rv;
        if (element_set_volume(e, m, cm, &ev) < 0)
            return -1;

        if (!p->has_dB) {
            *v = ev;
            return 0;
        }

        pa_sw_cvolume_multiply(v, v, &ev);
        pa_sw_cvolume_divide(&rv, &rv, &ev);
    }

    return 0;
}

static int element_set_switch(pa_alsa_element *e, snd_mixer_t *m, pa_bool_t b) {
    snd_mixer_elem_t *me;
    snd_mixer_selem_id_t *sid;
    int r;

    pa_assert(m);
    pa_assert(e);

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return -1;
    }

    if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
        r = snd_mixer_selem_set_playback_switch_all(me, b);
    else
        r = snd_mixer_selem_set_capture_switch_all(me, b);

    if (r < 0)
        pa_log_warn("Failed to set switch of %s: %s", e->alsa_name, pa_alsa_strerror(errno));

    return r;
}

int pa_alsa_path_set_mute(pa_alsa_path *p, snd_mixer_t *m, pa_bool_t muted) {
    pa_alsa_element *e;

    pa_assert(m);
    pa_assert(p);

    if (!p->has_mute)
        return -1;

    PA_LLIST_FOREACH(e, p->elements) {

        if (e->switch_use != PA_ALSA_SWITCH_MUTE)
            continue;

        if (element_set_switch(e, m, !muted) < 0)
            return -1;
    }

    return 0;
}

static int element_mute_volume(pa_alsa_element *e, snd_mixer_t *m) {
    snd_mixer_elem_t *me;
    snd_mixer_selem_id_t *sid;
    int r;

    pa_assert(m);
    pa_assert(e);

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return -1;
    }

    if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
        r = snd_mixer_selem_set_playback_volume_all(me, e->min_volume);
    else
        r = snd_mixer_selem_set_capture_volume_all(me, e->min_volume);

    if (r < 0)
        pa_log_warn("Faile to set volume to muted of %s: %s", e->alsa_name, pa_alsa_strerror(errno));

    return r;
}

/* The volume to 0dB */
static int element_zero_volume(pa_alsa_element *e, snd_mixer_t *m) {
    snd_mixer_elem_t *me;
    snd_mixer_selem_id_t *sid;
    int r;

    pa_assert(m);
    pa_assert(e);

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return -1;
    }

    if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
        r = snd_mixer_selem_set_playback_dB_all(me, 0, +1);
    else
        r = snd_mixer_selem_set_capture_dB_all(me, 0, +1);

    if (r < 0)
        pa_log_warn("Faile to set volume to 0dB of %s: %s", e->alsa_name, pa_alsa_strerror(errno));

    return r;
}

int pa_alsa_path_select(pa_alsa_path *p, snd_mixer_t *m) {
    pa_alsa_element *e;
    int r = 0;

    pa_assert(m);
    pa_assert(p);

    pa_log_debug("Activating path %s", p->name);
    pa_alsa_path_dump(p);

    PA_LLIST_FOREACH(e, p->elements) {

        switch (e->switch_use) {
            case PA_ALSA_SWITCH_OFF:
                r = element_set_switch(e, m, FALSE);
                break;

            case PA_ALSA_SWITCH_ON:
                r = element_set_switch(e, m, TRUE);
                break;

            case PA_ALSA_SWITCH_MUTE:
            case PA_ALSA_SWITCH_IGNORE:
            case PA_ALSA_SWITCH_SELECT:
                r = 0;
                break;
        }

        if (r < 0)
            return -1;

        switch (e->volume_use) {
            case PA_ALSA_VOLUME_OFF:
                r = element_mute_volume(e, m);
                break;

            case PA_ALSA_VOLUME_ZERO:
                r = element_zero_volume(e, m);
                break;

            case PA_ALSA_VOLUME_MERGE:
            case PA_ALSA_VOLUME_IGNORE:
                r = 0;
                break;
        }

        if (r < 0)
            return -1;
    }

    return 0;
}

static int check_required(pa_alsa_element *e, snd_mixer_elem_t *me) {
    pa_bool_t has_switch;
    pa_bool_t has_enumeration;
    pa_bool_t has_volume;

    pa_assert(e);
    pa_assert(me);

    if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {
        has_switch =
            snd_mixer_selem_has_playback_switch(me) ||
            (e->direction_try_other && snd_mixer_selem_has_capture_switch(me));
    } else {
        has_switch =
            snd_mixer_selem_has_capture_switch(me) ||
            (e->direction_try_other && snd_mixer_selem_has_playback_switch(me));
    }

    if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {
        has_volume =
            snd_mixer_selem_has_playback_volume(me) ||
            (e->direction_try_other && snd_mixer_selem_has_capture_volume(me));
    } else {
        has_volume =
            snd_mixer_selem_has_capture_volume(me) ||
            (e->direction_try_other && snd_mixer_selem_has_playback_volume(me));
    }

    has_enumeration = snd_mixer_selem_is_enumerated(me);

    if ((e->required == PA_ALSA_REQUIRED_SWITCH && !has_switch) ||
        (e->required == PA_ALSA_REQUIRED_VOLUME && !has_volume) ||
        (e->required == PA_ALSA_REQUIRED_ENUMERATION && !has_enumeration))
        return -1;

    if (e->required == PA_ALSA_REQUIRED_ANY && !(has_switch || has_volume || has_enumeration))
        return -1;

    if ((e->required_absent == PA_ALSA_REQUIRED_SWITCH && has_switch) ||
        (e->required_absent == PA_ALSA_REQUIRED_VOLUME && has_volume) ||
        (e->required_absent == PA_ALSA_REQUIRED_ENUMERATION && has_enumeration))
        return -1;

    if (e->required_absent == PA_ALSA_REQUIRED_ANY && (has_switch || has_volume || has_enumeration))
        return -1;

    return 0;
}

static int element_probe(pa_alsa_element *e, snd_mixer_t *m) {
    snd_mixer_selem_id_t *sid;
    snd_mixer_elem_t *me;

    pa_assert(m);
    pa_assert(e);

    SELEM_INIT(sid, e->alsa_name);

    if (!(me = snd_mixer_find_selem(m, sid))) {

        if (e->required != PA_ALSA_REQUIRED_IGNORE)
            return -1;

        e->switch_use = PA_ALSA_SWITCH_IGNORE;
        e->volume_use = PA_ALSA_VOLUME_IGNORE;
        e->enumeration_use = PA_ALSA_VOLUME_IGNORE;

        return 0;
    }

    if (e->switch_use != PA_ALSA_SWITCH_IGNORE) {
        if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {

            if (!snd_mixer_selem_has_playback_switch(me)) {
                if (e->direction_try_other && snd_mixer_selem_has_capture_switch(me))
                    e->direction = PA_ALSA_DIRECTION_INPUT;
                else
                    e->switch_use = PA_ALSA_SWITCH_IGNORE;
            }

        } else {

            if (!snd_mixer_selem_has_capture_switch(me)) {
                if (e->direction_try_other && snd_mixer_selem_has_playback_switch(me))
                    e->direction = PA_ALSA_DIRECTION_OUTPUT;
                else
                    e->switch_use = PA_ALSA_SWITCH_IGNORE;
            }
        }

        if (e->switch_use != PA_ALSA_SWITCH_IGNORE)
            e->direction_try_other = FALSE;
    }

    if (e->volume_use != PA_ALSA_VOLUME_IGNORE) {

        if (e->direction == PA_ALSA_DIRECTION_OUTPUT) {

            if (!snd_mixer_selem_has_playback_volume(me)) {
                if (e->direction_try_other && snd_mixer_selem_has_capture_volume(me))
                    e->direction = PA_ALSA_DIRECTION_INPUT;
                else
                    e->volume_use = PA_ALSA_VOLUME_IGNORE;
            }

        } else {

            if (!snd_mixer_selem_has_capture_volume(me)) {
                if (e->direction_try_other && snd_mixer_selem_has_playback_volume(me))
                    e->direction = PA_ALSA_DIRECTION_OUTPUT;
                else
                    e->volume_use = PA_ALSA_VOLUME_IGNORE;
            }
        }

        if (e->volume_use != PA_ALSA_VOLUME_IGNORE) {
            long min_dB = 0, max_dB = 0;
            int r;

            e->direction_try_other = FALSE;

            if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
                e->has_dB = snd_mixer_selem_get_playback_dB_range(me, &min_dB, &max_dB) >= 0;
            else
                e->has_dB = snd_mixer_selem_get_capture_dB_range(me, &min_dB, &max_dB) >= 0;

            if (e->has_dB) {
#ifdef HAVE_VALGRIND_MEMCHECK_H
                VALGRIND_MAKE_MEM_DEFINED(&min_dB, sizeof(min_dB));
                VALGRIND_MAKE_MEM_DEFINED(&max_dB, sizeof(max_dB));
#endif

                e->min_dB = ((double) min_dB) / 100.0;
                e->max_dB = ((double) max_dB) / 100.0;

                if (min_dB >= max_dB) {
                    pa_log_warn("Your kernel driver is broken: it reports a volume range from %0.2f dB to %0.2f dB which makes no sense.", e->min_dB, e->max_dB);
                    e->has_dB = FALSE;
                }
            }

            if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
                r = snd_mixer_selem_get_playback_volume_range(me, &e->min_volume, &e->max_volume);
            else
                r = snd_mixer_selem_get_capture_volume_range(me, &e->min_volume, &e->max_volume);

            if (r < 0) {
                pa_log_warn("Failed to get volume range of %s: %s", e->alsa_name, pa_alsa_strerror(r));
                return -1;
            }


            if (e->min_volume >= e->max_volume) {
                pa_log_warn("Your kernel driver is broken: it reports a volume range from %li to %li which makes no sense.", e->min_volume, e->max_volume);
                e->volume_use = PA_ALSA_VOLUME_IGNORE;

            } else {
                pa_bool_t is_mono;
                pa_channel_position_t p;

                if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
                    is_mono = snd_mixer_selem_is_playback_mono(me) > 0;
                else
                    is_mono = snd_mixer_selem_is_capture_mono(me) > 0;

                if (is_mono) {
                    e->n_channels = 1;

                    if (!e->override_map) {
                        for (p = PA_CHANNEL_POSITION_FRONT_LEFT; p < PA_CHANNEL_POSITION_MAX; p++)
                            e->masks[alsa_channel_ids[p]][e->n_channels-1] = 0;
                        e->masks[SND_MIXER_SCHN_MONO][e->n_channels-1] = PA_CHANNEL_POSITION_MASK_ALL;
                    }

                    e->merged_mask = e->masks[SND_MIXER_SCHN_MONO][e->n_channels-1];
                } else {
                    e->n_channels = 0;
                    for (p = PA_CHANNEL_POSITION_FRONT_LEFT; p < PA_CHANNEL_POSITION_MAX; p++) {

                        if (alsa_channel_ids[p] == SND_MIXER_SCHN_UNKNOWN)
                            continue;

                        if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
                            e->n_channels += snd_mixer_selem_has_playback_channel(me, alsa_channel_ids[p]) > 0;
                        else
                            e->n_channels += snd_mixer_selem_has_capture_channel(me, alsa_channel_ids[p]) > 0;
                    }

                    if (e->n_channels <= 0) {
                        pa_log_warn("Volume element %s with no channels?", e->alsa_name);
                        return -1;
                    }

                    if (!e->override_map) {
                        for (p = PA_CHANNEL_POSITION_FRONT_LEFT; p < PA_CHANNEL_POSITION_MAX; p++) {
                            pa_bool_t has_channel;

                            if (alsa_channel_ids[p] == SND_MIXER_SCHN_UNKNOWN)
                                continue;

                            if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
                                has_channel = snd_mixer_selem_has_playback_channel(me, alsa_channel_ids[p]) > 0;
                            else
                                has_channel = snd_mixer_selem_has_capture_channel(me, alsa_channel_ids[p]) > 0;

                            e->masks[alsa_channel_ids[p]][e->n_channels-1] = has_channel ? PA_CHANNEL_POSITION_MASK(p) : 0;
                        }
                    }

                    e->merged_mask = 0;
                    for (p = PA_CHANNEL_POSITION_FRONT_LEFT; p < PA_CHANNEL_POSITION_MAX; p++)
                        e->merged_mask |= e->masks[alsa_channel_ids[p]][e->n_channels-1];
                }
            }
        }

    }

    if (check_required(e, me) < 0)
        return -1;

    if (e->switch_use == PA_ALSA_SWITCH_SELECT) {
        pa_alsa_option *o;

        PA_LLIST_FOREACH(o, e->options)
            o->alsa_idx = pa_streq(o->alsa_name, "on") ? 1 : 0;
    } else if (e->enumeration_use == PA_ALSA_ENUMERATION_SELECT) {
        int n;
        pa_alsa_option *o;

        if ((n = snd_mixer_selem_get_enum_items(me)) < 0) {
            pa_log("snd_mixer_selem_get_enum_items() failed: %s", pa_alsa_strerror(n));
            return -1;
        }

        PA_LLIST_FOREACH(o, e->options) {
            int i;

            for (i = 0; i < n; i++) {
                char buf[128];

                if (snd_mixer_selem_get_enum_item_name(me, i, sizeof(buf), buf) < 0)
                    continue;

                if (!pa_streq(buf, o->alsa_name))
                    continue;

                o->alsa_idx = i;
            }
        }
    }

    return 0;
}

static pa_alsa_element* element_get(pa_alsa_path *p, const char *section, pa_bool_t prefixed) {
    pa_alsa_element *e;

    pa_assert(p);
    pa_assert(section);

    if (prefixed) {
        if (!pa_startswith(section, "Element "))
            return NULL;

        section += 8;
    }

    /* This is not an element section, but an enum section? */
    if (strchr(section, ':'))
        return NULL;

    if (p->last_element && pa_streq(p->last_element->alsa_name, section))
        return p->last_element;

    PA_LLIST_FOREACH(e, p->elements)
        if (pa_streq(e->alsa_name, section))
            goto finish;

    e = pa_xnew0(pa_alsa_element, 1);
    e->path = p;
    e->alsa_name = pa_xstrdup(section);
    e->direction = p->direction;

    PA_LLIST_INSERT_AFTER(pa_alsa_element, p->elements, p->last_element, e);

finish:
    p->last_element = e;
    return e;
}

static pa_alsa_option* option_get(pa_alsa_path *p, const char *section) {
    char *en;
    const char *on;
    pa_alsa_option *o;
    pa_alsa_element *e;

    if (!pa_startswith(section, "Option "))
        return NULL;

    section += 7;

    /* This is not an enum section, but an element section? */
    if (!(on = strchr(section, ':')))
        return NULL;

    en = pa_xstrndup(section, on - section);
    on++;

    if (p->last_option &&
        pa_streq(p->last_option->element->alsa_name, en) &&
        pa_streq(p->last_option->alsa_name, on)) {
        pa_xfree(en);
        return p->last_option;
    }

    pa_assert_se(e = element_get(p, en, FALSE));
    pa_xfree(en);

    PA_LLIST_FOREACH(o, e->options)
        if (pa_streq(o->alsa_name, on))
            goto finish;

    o = pa_xnew0(pa_alsa_option, 1);
    o->element = e;
    o->alsa_name = pa_xstrdup(on);
    o->alsa_idx = -1;

    if (p->last_option && p->last_option->element == e)
        PA_LLIST_INSERT_AFTER(pa_alsa_option, e->options, p->last_option, o);
    else
        PA_LLIST_PREPEND(pa_alsa_option, e->options, o);

finish:
    p->last_option = o;
    return o;
}

static int element_parse_switch(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_element *e;

    pa_assert(p);

    if (!(e = element_get(p, section, TRUE))) {
        pa_log("[%s:%u] Switch makes no sense in '%s'", filename, line, section);
        return -1;
    }

    if (pa_streq(rvalue, "ignore"))
        e->switch_use = PA_ALSA_SWITCH_IGNORE;
    else if (pa_streq(rvalue, "mute"))
        e->switch_use = PA_ALSA_SWITCH_MUTE;
    else if (pa_streq(rvalue, "off"))
        e->switch_use = PA_ALSA_SWITCH_OFF;
    else if (pa_streq(rvalue, "on"))
        e->switch_use = PA_ALSA_SWITCH_ON;
    else if (pa_streq(rvalue, "select"))
        e->switch_use = PA_ALSA_SWITCH_SELECT;
    else {
        pa_log("[%s:%u] Switch invalid of '%s'", filename, line, section);
        return -1;
    }

    return 0;
}

static int element_parse_volume(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_element *e;

    pa_assert(p);

    if (!(e = element_get(p, section, TRUE))) {
        pa_log("[%s:%u] Volume makes no sense in '%s'", filename, line, section);
        return -1;
    }

    if (pa_streq(rvalue, "ignore"))
        e->volume_use = PA_ALSA_VOLUME_IGNORE;
    else if (pa_streq(rvalue, "merge"))
        e->volume_use = PA_ALSA_VOLUME_MERGE;
    else if (pa_streq(rvalue, "off"))
        e->volume_use = PA_ALSA_VOLUME_OFF;
    else if (pa_streq(rvalue, "zero"))
        e->volume_use = PA_ALSA_VOLUME_ZERO;
    else {
        pa_log("[%s:%u] Volume invalid of '%s'", filename, line, section);
        return -1;
    }

    return 0;
}

static int element_parse_enumeration(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_element *e;

    pa_assert(p);

    if (!(e = element_get(p, section, TRUE))) {
        pa_log("[%s:%u] Enumeration makes no sense in '%s'", filename, line, section);
        return -1;
    }

    if (pa_streq(rvalue, "ignore"))
        e->enumeration_use = PA_ALSA_ENUMERATION_IGNORE;
    else if (pa_streq(rvalue, "select"))
        e->enumeration_use = PA_ALSA_ENUMERATION_SELECT;
    else {
        pa_log("[%s:%u] Enumeration invalid of '%s'", filename, line, section);
        return -1;
    }

    return 0;
}

static int option_parse_priority(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_option *o;
    uint32_t prio;

    pa_assert(p);

    if (!(o = option_get(p, section))) {
        pa_log("[%s:%u] Priority makes no sense in '%s'", filename, line, section);
        return -1;
    }

    if (pa_atou(rvalue, &prio) < 0) {
        pa_log("[%s:%u] Priority invalid of '%s'", filename, line, section);
        return -1;
    }

    o->priority = prio;
    return 0;
}

static int option_parse_name(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_option *o;

    pa_assert(p);

    if (!(o = option_get(p, section))) {
        pa_log("[%s:%u] Name makes no sense in '%s'", filename, line, section);
        return -1;
    }

    pa_xfree(o->name);
    o->name = pa_xstrdup(rvalue);

    return 0;
}

static int element_parse_required(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_element *e;
    pa_alsa_required_t req;

    pa_assert(p);

    if (!(e = element_get(p, section, TRUE))) {
        pa_log("[%s:%u] Required makes no sense in '%s'", filename, line, section);
        return -1;
    }

    if (pa_streq(rvalue, "ignore"))
        req = PA_ALSA_REQUIRED_IGNORE;
    else if (pa_streq(rvalue, "switch"))
        req = PA_ALSA_REQUIRED_SWITCH;
    else if (pa_streq(rvalue, "volume"))
        req = PA_ALSA_REQUIRED_VOLUME;
    else if (pa_streq(rvalue, "enumeration"))
        req = PA_ALSA_REQUIRED_ENUMERATION;
    else if (pa_streq(rvalue, "any"))
        req = PA_ALSA_REQUIRED_ANY;
    else {
        pa_log("[%s:%u] Required invalid of '%s'", filename, line, section);
        return -1;
    }

    if (pa_streq(lvalue, "required-absent"))
        e->required_absent = req;
    else
        e->required = req;

    return 0;
}

static int element_parse_direction(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_element *e;

    pa_assert(p);

    if (!(e = element_get(p, section, TRUE))) {
        pa_log("[%s:%u] Direction makes no sense in '%s'", filename, line, section);
        return -1;
    }

    if (pa_streq(rvalue, "playback"))
        e->direction = PA_ALSA_DIRECTION_OUTPUT;
    else if (pa_streq(rvalue, "capture"))
        e->direction = PA_ALSA_DIRECTION_INPUT;
    else {
        pa_log("[%s:%u] Direction invalid of '%s'", filename, line, section);
        return -1;
    }

    return 0;
}

static int element_parse_direction_try_other(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_element *e;
    int yes;

    if (!(e = element_get(p, section, TRUE))) {
        pa_log("[%s:%u] Direction makes no sense in '%s'", filename, line, section);
        return -1;
    }

    if ((yes = pa_parse_boolean(rvalue)) < 0) {
        pa_log("[%s:%u] Direction invalid of '%s'", filename, line, section);
        return -1;
    }

    e->direction_try_other = !!yes;
    return 0;
}

static pa_channel_position_mask_t parse_mask(const char *m) {
    pa_channel_position_mask_t v;

    if (pa_streq(m, "all-left"))
        v = PA_CHANNEL_POSITION_MASK_LEFT;
    else if (pa_streq(m, "all-right"))
        v = PA_CHANNEL_POSITION_MASK_RIGHT;
    else if (pa_streq(m, "all-center"))
        v = PA_CHANNEL_POSITION_MASK_CENTER;
    else if (pa_streq(m, "all-front"))
        v = PA_CHANNEL_POSITION_MASK_FRONT;
    else if (pa_streq(m, "all-rear"))
        v = PA_CHANNEL_POSITION_MASK_REAR;
    else if (pa_streq(m, "all-side"))
        v = PA_CHANNEL_POSITION_MASK_SIDE_OR_TOP_CENTER;
    else if (pa_streq(m, "all-top"))
        v = PA_CHANNEL_POSITION_MASK_TOP;
    else if (pa_streq(m, "all-no-lfe"))
        v = PA_CHANNEL_POSITION_MASK_ALL ^ PA_CHANNEL_POSITION_MASK(PA_CHANNEL_POSITION_LFE);
    else if (pa_streq(m, "all"))
        v = PA_CHANNEL_POSITION_MASK_ALL;
    else {
        pa_channel_position_t p;

        if ((p = pa_channel_position_from_string(m)) == PA_CHANNEL_POSITION_INVALID)
            return 0;

        v = PA_CHANNEL_POSITION_MASK(p);
    }

    return v;
}

static int element_parse_override_map(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_path *p = userdata;
    pa_alsa_element *e;
    const char *state = NULL;
    unsigned i = 0;
    char *n;

    if (!(e = element_get(p, section, TRUE))) {
        pa_log("[%s:%u] Override map makes no sense in '%s'", filename, line, section);
        return -1;
    }

    while ((n = pa_split(rvalue, ",", &state))) {
        pa_channel_position_mask_t m;

        if (!*n)
            m = 0;
        else {
            if ((m = parse_mask(n)) == 0) {
                pa_log("[%s:%u] Override map '%s' invalid in '%s'", filename, line, n, section);
                pa_xfree(n);
                return -1;
            }
        }

        if (pa_streq(lvalue, "override-map.1"))
            e->masks[i++][0] = m;
        else
            e->masks[i++][1] = m;

        /* Later on we might add override-map.3 and so on here ... */

        pa_xfree(n);
    }

    e->override_map = TRUE;

    return 0;
}

static int element_set_option(pa_alsa_element *e, snd_mixer_t *m, int alsa_idx) {
    snd_mixer_selem_id_t *sid;
    snd_mixer_elem_t *me;
    int r;

    pa_assert(e);
    pa_assert(m);

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return -1;
    }

    if (e->switch_use == PA_ALSA_SWITCH_SELECT) {

        if (e->direction == PA_ALSA_DIRECTION_OUTPUT)
            r = snd_mixer_selem_set_playback_switch_all(me, alsa_idx);
        else
            r = snd_mixer_selem_set_capture_switch_all(me, alsa_idx);

        if (r < 0)
            pa_log_warn("Faile to set switch of %s: %s", e->alsa_name, pa_alsa_strerror(errno));

    } else {
        pa_assert(e->enumeration_use == PA_ALSA_ENUMERATION_SELECT);

        if ((r = snd_mixer_selem_set_enum_item(me, 0, alsa_idx)) < 0)
            pa_log_warn("Faile to set enumeration of %s: %s", e->alsa_name, pa_alsa_strerror(errno));
    }

    return r;
}

int pa_alsa_setting_select(pa_alsa_setting *s, snd_mixer_t *m) {
    pa_alsa_option *o;
    uint32_t idx;

    pa_assert(s);
    pa_assert(m);

    PA_IDXSET_FOREACH(o, s->options, idx)
        element_set_option(o->element, m, o->alsa_idx);

    return 0;
}

static int option_verify(pa_alsa_option *o) {
    static const struct description_map well_known_descriptions[] = {
        { "input",                     N_("Input") },
        { "input-docking",             N_("Docking Station Input") },
        { "input-docking-microphone",  N_("Docking Station Microphone") },
        { "input-linein",              N_("Line-In") },
        { "input-microphone",          N_("Microphone") },
        { "input-microphone-external", N_("External Microphone") },
        { "input-microphone-internal", N_("Internal Microphone") },
        { "input-radio",               N_("Radio") },
        { "input-video",               N_("Video") },
        { "input-agc-on",              N_("Automatic Gain Control") },
        { "input-agc-off",             N_("No Automatic Gain Control") },
        { "input-boost-on",            N_("Boost") },
        { "input-boost-off",           N_("No Boost") },
        { "output-amplifier-on",       N_("Amplifier") },
        { "output-amplifier-off",      N_("No Amplifier") },
        { "output-speaker",            N_("Speaker") },
        { "output-headphones",         N_("Headphones") }
    };

    pa_assert(o);

    if (!o->name) {
        pa_log("No name set for option %s", o->alsa_name);
        return -1;
    }

    if (o->element->enumeration_use != PA_ALSA_ENUMERATION_SELECT &&
        o->element->switch_use != PA_ALSA_SWITCH_SELECT) {
        pa_log("Element %s of option %s not set for select.", o->element->alsa_name, o->name);
        return -1;
    }

    if (o->element->switch_use == PA_ALSA_SWITCH_SELECT &&
        !pa_streq(o->alsa_name, "on") &&
        !pa_streq(o->alsa_name, "off")) {
        pa_log("Switch %s options need be named off or on ", o->element->alsa_name);
        return -1;
    }

    if (!o->description)
        o->description = pa_xstrdup(lookup_description(o->name,
                                                       well_known_descriptions,
                                                       PA_ELEMENTSOF(well_known_descriptions)));
    if (!o->description)
        o->description = pa_xstrdup(o->name);

    return 0;
}

static int element_verify(pa_alsa_element *e) {
    pa_alsa_option *o;

    pa_assert(e);

    if ((e->required != PA_ALSA_REQUIRED_IGNORE && e->required == e->required_absent) ||
        (e->required_absent == PA_ALSA_REQUIRED_ANY && e->required != PA_ALSA_REQUIRED_IGNORE)) {
        pa_log("Element %s cannot be required and absent at the same time.", e->alsa_name);
        return -1;
    }

    if (e->switch_use == PA_ALSA_SWITCH_SELECT && e->enumeration_use == PA_ALSA_ENUMERATION_SELECT) {
        pa_log("Element %s cannot set select for both switch and enumeration.", e->alsa_name);
        return -1;
    }

    PA_LLIST_FOREACH(o, e->options)
        if (option_verify(o) < 0)
            return -1;

    return 0;
}

static int path_verify(pa_alsa_path *p) {
    static const struct description_map well_known_descriptions[] = {
        { "analog-input",               N_("Analog Input") },
        { "analog-input-microphone",    N_("Analog Microphone") },
        { "analog-input-linein",        N_("Analog Line-In") },
        { "analog-input-radio",         N_("Analog Radio") },
        { "analog-input-video",         N_("Analog Video") },
        { "analog-output",              N_("Analog Output") },
        { "analog-output-headphones",   N_("Analog Headphones") },
        { "analog-output-lfe-on-mono",  N_("Analog Output (LFE)") },
        { "analog-output-mono",         N_("Analog Mono Output") },
        { "analog-output-headphones-2", N_("Analog Headphones 2") },
        { "analog-output-speaker",      N_("Analog Speaker") }
    };

    pa_alsa_element *e;

    pa_assert(p);

    PA_LLIST_FOREACH(e, p->elements)
        if (element_verify(e) < 0)
            return -1;

    if (!p->description)
        p->description = pa_xstrdup(lookup_description(p->name,
                                                       well_known_descriptions,
                                                       PA_ELEMENTSOF(well_known_descriptions)));

    if (!p->description)
        p->description = pa_xstrdup(p->name);

    return 0;
}

pa_alsa_path* pa_alsa_path_new(const char *fname, pa_alsa_direction_t direction) {
    pa_alsa_path *p;
    char *fn;
    int r;
    const char *n;

    pa_config_item items[] = {
        /* [General] */
        { "priority",            pa_config_parse_unsigned,          NULL, "General" },
        { "description",         pa_config_parse_string,            NULL, "General" },
        { "name",                pa_config_parse_string,            NULL, "General" },

        /* [Option ...] */
        { "priority",            option_parse_priority,             NULL, NULL },
        { "name",                option_parse_name,                 NULL, NULL },

        /* [Element ...] */
        { "switch",              element_parse_switch,              NULL, NULL },
        { "volume",              element_parse_volume,              NULL, NULL },
        { "enumeration",         element_parse_enumeration,         NULL, NULL },
        { "override-map.1",      element_parse_override_map,        NULL, NULL },
        { "override-map.2",      element_parse_override_map,        NULL, NULL },
        /* ... later on we might add override-map.3 and so on here ... */
        { "required",            element_parse_required,            NULL, NULL },
        { "required-absent",     element_parse_required,            NULL, NULL },
        { "direction",           element_parse_direction,           NULL, NULL },
        { "direction-try-other", element_parse_direction_try_other, NULL, NULL },
        { NULL, NULL, NULL, NULL }
    };

    pa_assert(fname);

    p = pa_xnew0(pa_alsa_path, 1);
    n = pa_path_get_filename(fname);
    p->name = pa_xstrndup(n, strcspn(n, "."));
    p->direction = direction;

    items[0].data = &p->priority;
    items[1].data = &p->description;
    items[2].data = &p->name;

    fn = pa_maybe_prefix_path(fname,
#if defined(__linux__) && !defined(__OPTIMIZE__)
                              pa_run_from_build_tree() ? PA_BUILDDIR "/modules/alsa/mixer/paths/" :
#endif
                              PA_ALSA_PATHS_DIR);

    r = pa_config_parse(fn, NULL, items, p);
    pa_xfree(fn);

    if (r < 0)
        goto fail;

    if (path_verify(p) < 0)
        goto fail;

    return p;

fail:
    pa_alsa_path_free(p);
    return NULL;
}

pa_alsa_path* pa_alsa_path_synthesize(const char*element, pa_alsa_direction_t direction) {
    pa_alsa_path *p;
    pa_alsa_element *e;

    pa_assert(element);

    p = pa_xnew0(pa_alsa_path, 1);
    p->name = pa_xstrdup(element);
    p->direction = direction;

    e = pa_xnew0(pa_alsa_element, 1);
    e->path = p;
    e->alsa_name = pa_xstrdup(element);
    e->direction = direction;

    e->switch_use = PA_ALSA_SWITCH_MUTE;
    e->volume_use = PA_ALSA_VOLUME_MERGE;

    PA_LLIST_PREPEND(pa_alsa_element, p->elements, e);
    return p;
}

static pa_bool_t element_drop_unsupported(pa_alsa_element *e) {
    pa_alsa_option *o, *n;

    pa_assert(e);

    for (o = e->options; o; o = n) {
        n = o->next;

        if (o->alsa_idx < 0) {
            PA_LLIST_REMOVE(pa_alsa_option, e->options, o);
            option_free(o);
        }
    }

    return
        e->switch_use != PA_ALSA_SWITCH_IGNORE ||
        e->volume_use != PA_ALSA_VOLUME_IGNORE ||
        e->enumeration_use != PA_ALSA_ENUMERATION_IGNORE;
}

static void path_drop_unsupported(pa_alsa_path *p) {
    pa_alsa_element *e, *n;

    pa_assert(p);

    for (e = p->elements; e; e = n) {
        n = e->next;

        if (!element_drop_unsupported(e)) {
            PA_LLIST_REMOVE(pa_alsa_element, p->elements, e);
            element_free(e);
        }
    }
}

static void path_make_options_unique(pa_alsa_path *p) {
    pa_alsa_element *e;
    pa_alsa_option *o, *u;

    PA_LLIST_FOREACH(e, p->elements) {
        PA_LLIST_FOREACH(o, e->options) {
            unsigned i;
            char *m;

            for (u = o->next; u; u = u->next)
                if (pa_streq(u->name, o->name))
                    break;

            if (!u)
                continue;

            m = pa_xstrdup(o->name);

            /* OK, this name is not unique, hence let's rename */
            for (i = 1, u = o; u; u = u->next) {
                char *nn, *nd;

                if (!pa_streq(u->name, m))
                    continue;

                nn = pa_sprintf_malloc("%s-%u", m, i);
                pa_xfree(u->name);
                u->name = nn;

                nd = pa_sprintf_malloc("%s %u", u->description, i);
                pa_xfree(u->description);
                u->description = nd;

                i++;
            }

            pa_xfree(m);
        }
    }
}

static pa_bool_t element_create_settings(pa_alsa_element *e, pa_alsa_setting *template) {
    pa_alsa_option *o;

    for (; e; e = e->next)
        if (e->switch_use == PA_ALSA_SWITCH_SELECT ||
            e->enumeration_use == PA_ALSA_ENUMERATION_SELECT)
            break;

    if (!e)
        return FALSE;

    for (o = e->options; o; o = o->next) {
        pa_alsa_setting *s;

        if (template) {
            s = pa_xnewdup(pa_alsa_setting, template, 1);
            s->options = pa_idxset_copy(template->options);
            s->name = pa_sprintf_malloc(_("%s+%s"), template->name, o->name);
            s->description =
                (template->description[0] && o->description[0])
                ? pa_sprintf_malloc(_("%s / %s"), template->description, o->description)
                : (template->description[0]
                   ? pa_xstrdup(template->description)
                   : pa_xstrdup(o->description));

            s->priority = PA_MAX(template->priority, o->priority);
        } else {
            s = pa_xnew0(pa_alsa_setting, 1);
            s->options = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
            s->name = pa_xstrdup(o->name);
            s->description = pa_xstrdup(o->description);
            s->priority = o->priority;
        }

        pa_idxset_put(s->options, o, NULL);

        if (element_create_settings(e->next, s))
            /* This is not a leaf, so let's get rid of it */
            setting_free(s);
        else {
            /* This is a leaf, so let's add it */
            PA_LLIST_INSERT_AFTER(pa_alsa_setting, e->path->settings, e->path->last_setting, s);

            e->path->last_setting = s;
        }
    }

    return TRUE;
}

static void path_create_settings(pa_alsa_path *p) {
    pa_assert(p);

    element_create_settings(p->elements, NULL);
}

int pa_alsa_path_probe(pa_alsa_path *p, snd_mixer_t *m, pa_bool_t ignore_dB) {
    pa_alsa_element *e;
    double min_dB[PA_CHANNEL_POSITION_MAX], max_dB[PA_CHANNEL_POSITION_MAX];
    pa_channel_position_t t;

    pa_assert(p);
    pa_assert(m);

    if (p->probed)
        return 0;

    pa_zero(min_dB);
    pa_zero(max_dB);

    pa_log_debug("Probing path '%s'", p->name);

    PA_LLIST_FOREACH(e, p->elements) {
        if (element_probe(e, m) < 0) {
            p->supported = FALSE;
            pa_log_debug("Probe of element '%s' failed.", e->alsa_name);
            return -1;
        }

        if (ignore_dB)
            e->has_dB = FALSE;

        if (e->volume_use == PA_ALSA_VOLUME_MERGE) {

            if (!p->has_volume) {
                p->min_volume = e->min_volume;
                p->max_volume = e->max_volume;
            }

            if (e->has_dB) {
                if (!p->has_volume) {
                    for (t = 0; t < PA_CHANNEL_POSITION_MAX; t++)
                        if (PA_CHANNEL_POSITION_MASK(t) & e->merged_mask) {
                            min_dB[t] = e->min_dB;
                            max_dB[t] = e->max_dB;
                        }

                    p->has_dB = TRUE;
                } else {

                    if (p->has_dB) {
                        for (t = 0; t < PA_CHANNEL_POSITION_MAX; t++)
                            if (PA_CHANNEL_POSITION_MASK(t) & e->merged_mask) {
                                min_dB[t] += e->min_dB;
                                max_dB[t] += e->max_dB;
                            }
                    } else
                        /* Hmm, there's another element before us
                         * which cannot do dB volumes, so we we need
                         * to 'neutralize' this slider */
                        e->volume_use = PA_ALSA_VOLUME_ZERO;
                }
            } else if (p->has_volume)
                /* We can't use this volume, so let's ignore it */
                e->volume_use = PA_ALSA_VOLUME_IGNORE;

            p->has_volume = TRUE;
        }

        if (e->switch_use == PA_ALSA_SWITCH_MUTE)
            p->has_mute = TRUE;
    }

    path_drop_unsupported(p);
    path_make_options_unique(p);
    path_create_settings(p);

    p->supported = TRUE;
    p->probed = TRUE;

    p->min_dB = INFINITY;
    p->max_dB = -INFINITY;

    for (t = 0; t < PA_CHANNEL_POSITION_MAX; t++) {
        if (p->min_dB > min_dB[t])
            p->min_dB = min_dB[t];

        if (p->max_dB < max_dB[t])
            p->max_dB = max_dB[t];
    }

    return 0;
}

void pa_alsa_setting_dump(pa_alsa_setting *s) {
    pa_assert(s);

    pa_log_debug("Setting %s (%s) priority=%u",
                 s->name,
                 pa_strnull(s->description),
                 s->priority);
}

void pa_alsa_option_dump(pa_alsa_option *o) {
    pa_assert(o);

    pa_log_debug("Option %s (%s/%s) index=%i, priority=%u",
                 o->alsa_name,
                 pa_strnull(o->name),
                 pa_strnull(o->description),
                 o->alsa_idx,
                 o->priority);
}

void pa_alsa_element_dump(pa_alsa_element *e) {
    pa_alsa_option *o;
    pa_assert(e);

    pa_log_debug("Element %s, direction=%i, switch=%i, volume=%i, enumeration=%i, required=%i, required_absent=%i, mask=0x%llx, n_channels=%u, override_map=%s",
                 e->alsa_name,
                 e->direction,
                 e->switch_use,
                 e->volume_use,
                 e->enumeration_use,
                 e->required,
                 e->required_absent,
                 (long long unsigned) e->merged_mask,
                 e->n_channels,
                 pa_yes_no(e->override_map));

    PA_LLIST_FOREACH(o, e->options)
        pa_alsa_option_dump(o);
}

void pa_alsa_path_dump(pa_alsa_path *p) {
    pa_alsa_element *e;
    pa_alsa_setting *s;
    pa_assert(p);

    pa_log_debug("Path %s (%s), direction=%i, priority=%u, probed=%s, supported=%s, has_mute=%s, has_volume=%s, "
                 "has_dB=%s, min_volume=%li, max_volume=%li, min_dB=%g, max_dB=%g",
                 p->name,
                 pa_strnull(p->description),
                 p->direction,
                 p->priority,
                 pa_yes_no(p->probed),
                 pa_yes_no(p->supported),
                 pa_yes_no(p->has_mute),
                 pa_yes_no(p->has_volume),
                 pa_yes_no(p->has_dB),
                 p->min_volume, p->max_volume,
                 p->min_dB, p->max_dB);

    PA_LLIST_FOREACH(e, p->elements)
        pa_alsa_element_dump(e);

    PA_LLIST_FOREACH(s, p->settings)
        pa_alsa_setting_dump(s);
}

static void element_set_callback(pa_alsa_element *e, snd_mixer_t *m, snd_mixer_elem_callback_t cb, void *userdata) {
    snd_mixer_selem_id_t *sid;
    snd_mixer_elem_t *me;

    pa_assert(e);
    pa_assert(m);
    pa_assert(cb);

    SELEM_INIT(sid, e->alsa_name);
    if (!(me = snd_mixer_find_selem(m, sid))) {
        pa_log_warn("Element %s seems to have disappeared.", e->alsa_name);
        return;
    }

    snd_mixer_elem_set_callback(me, cb);
    snd_mixer_elem_set_callback_private(me, userdata);
}

void pa_alsa_path_set_callback(pa_alsa_path *p, snd_mixer_t *m, snd_mixer_elem_callback_t cb, void *userdata) {
    pa_alsa_element *e;

    pa_assert(p);
    pa_assert(m);
    pa_assert(cb);

    PA_LLIST_FOREACH(e, p->elements)
        element_set_callback(e, m, cb, userdata);
}

void pa_alsa_path_set_set_callback(pa_alsa_path_set *ps, snd_mixer_t *m, snd_mixer_elem_callback_t cb, void *userdata) {
    pa_alsa_path *p;

    pa_assert(ps);
    pa_assert(m);
    pa_assert(cb);

    PA_LLIST_FOREACH(p, ps->paths)
        pa_alsa_path_set_callback(p, m, cb, userdata);
}

pa_alsa_path_set *pa_alsa_path_set_new(pa_alsa_mapping *m, pa_alsa_direction_t direction) {
    pa_alsa_path_set *ps;
    char **pn = NULL, **en = NULL, **ie;

    pa_assert(m);
    pa_assert(direction == PA_ALSA_DIRECTION_OUTPUT || direction == PA_ALSA_DIRECTION_INPUT);

    if (m->direction != PA_ALSA_DIRECTION_ANY && m->direction != direction)
        return NULL;

    ps = pa_xnew0(pa_alsa_path_set, 1);
    ps->direction = direction;

    if (direction == PA_ALSA_DIRECTION_OUTPUT)
        pn = m->output_path_names;
    else if (direction == PA_ALSA_DIRECTION_INPUT)
        pn = m->input_path_names;

    if (pn) {
        char **in;

        for (in = pn; *in; in++) {
            pa_alsa_path *p;
            pa_bool_t duplicate = FALSE;
            char **kn, *fn;

            for (kn = pn; kn != in; kn++)
                if (pa_streq(*kn, *in)) {
                    duplicate = TRUE;
                    break;
                }

            if (duplicate)
                continue;

            fn = pa_sprintf_malloc("%s.conf", *in);

            if ((p = pa_alsa_path_new(fn, direction))) {
                p->path_set = ps;
                PA_LLIST_INSERT_AFTER(pa_alsa_path, ps->paths, ps->last_path, p);
                ps->last_path = p;
            }

            pa_xfree(fn);
        }

        return ps;
    }

    if (direction == PA_ALSA_DIRECTION_OUTPUT)
        en = m->output_element;
    else if (direction == PA_ALSA_DIRECTION_INPUT)
        en = m->input_element;

    if (!en) {
        pa_alsa_path_set_free(ps);
        return NULL;
    }

    for (ie = en; *ie; ie++) {
        char **je;
        pa_alsa_path *p;

        p = pa_alsa_path_synthesize(*ie, direction);
        p->path_set = ps;

        /* Mark all other passed elements for require-absent */
        for (je = en; *je; je++) {
            pa_alsa_element *e;
            e = pa_xnew0(pa_alsa_element, 1);
            e->path = p;
            e->alsa_name = pa_xstrdup(*je);
            e->direction = direction;
            e->required_absent = PA_ALSA_REQUIRED_ANY;

            PA_LLIST_INSERT_AFTER(pa_alsa_element, p->elements, p->last_element, e);
            p->last_element = e;
        }

        PA_LLIST_INSERT_AFTER(pa_alsa_path, ps->paths, ps->last_path, p);
        ps->last_path = p;
    }

    return ps;
}

void pa_alsa_path_set_dump(pa_alsa_path_set *ps) {
    pa_alsa_path *p;
    pa_assert(ps);

    pa_log_debug("Path Set %p, direction=%i, probed=%s",
                 (void*) ps,
                 ps->direction,
                 pa_yes_no(ps->probed));

    PA_LLIST_FOREACH(p, ps->paths)
        pa_alsa_path_dump(p);
}

static void path_set_unify(pa_alsa_path_set *ps) {
    pa_alsa_path *p;
    pa_bool_t has_dB = TRUE, has_volume = TRUE, has_mute = TRUE;
    pa_assert(ps);

    /* We have issues dealing with paths that vary too wildly. That
     * means for now we have to have all paths support volume/mute/dB
     * or none. */

    PA_LLIST_FOREACH(p, ps->paths) {
        pa_assert(p->probed);

        if (!p->has_volume)
            has_volume = FALSE;
        else if (!p->has_dB)
            has_dB = FALSE;

        if (!p->has_mute)
            has_mute = FALSE;
    }

    if (!has_volume || !has_dB || !has_mute) {

        if (!has_volume)
            pa_log_debug("Some paths of the device lack hardware volume control, disabling hardware control altogether.");
        else if (!has_dB)
            pa_log_debug("Some paths of the device lack dB information, disabling dB logic altogether.");

        if (!has_mute)
            pa_log_debug("Some paths of the device lack hardware mute control, disabling hardware control altogether.");

        PA_LLIST_FOREACH(p, ps->paths) {
            if (!has_volume)
                p->has_volume = FALSE;
            else if (!has_dB)
                p->has_dB = FALSE;

            if (!has_mute)
                p->has_mute = FALSE;
        }
    }
}

static void path_set_make_paths_unique(pa_alsa_path_set *ps) {
    pa_alsa_path *p, *q;

    PA_LLIST_FOREACH(p, ps->paths) {
        unsigned i;
        char *m;

        for (q = p->next; q; q = q->next)
            if (pa_streq(q->name, p->name))
                break;

        if (!q)
            continue;

        m = pa_xstrdup(p->name);

        /* OK, this name is not unique, hence let's rename */
        for (i = 1, q = p; q; q = q->next) {
            char *nn, *nd;

            if (!pa_streq(q->name, m))
                continue;

            nn = pa_sprintf_malloc("%s-%u", m, i);
            pa_xfree(q->name);
            q->name = nn;

            nd = pa_sprintf_malloc("%s %u", q->description, i);
            pa_xfree(q->description);
            q->description = nd;

            i++;
        }

        pa_xfree(m);
    }
}

void pa_alsa_path_set_probe(pa_alsa_path_set *ps, snd_mixer_t *m, pa_bool_t ignore_dB) {
    pa_alsa_path *p, *n;

    pa_assert(ps);

    if (ps->probed)
        return;

    for (p = ps->paths; p; p = n) {
        n = p->next;

        if (pa_alsa_path_probe(p, m, ignore_dB) < 0) {
            PA_LLIST_REMOVE(pa_alsa_path, ps->paths, p);
            pa_alsa_path_free(p);
        }
    }

    path_set_unify(ps);
    path_set_make_paths_unique(ps);
    ps->probed = TRUE;
}

static void mapping_free(pa_alsa_mapping *m) {
    pa_assert(m);

    pa_xfree(m->name);
    pa_xfree(m->description);

    pa_xstrfreev(m->device_strings);
    pa_xstrfreev(m->input_path_names);
    pa_xstrfreev(m->output_path_names);
    pa_xstrfreev(m->input_element);
    pa_xstrfreev(m->output_element);

    pa_assert(!m->input_pcm);
    pa_assert(!m->output_pcm);

    pa_xfree(m);
}

static void profile_free(pa_alsa_profile *p) {
    pa_assert(p);

    pa_xfree(p->name);
    pa_xfree(p->description);

    pa_xstrfreev(p->input_mapping_names);
    pa_xstrfreev(p->output_mapping_names);

    if (p->input_mappings)
        pa_idxset_free(p->input_mappings, NULL, NULL);

    if (p->output_mappings)
        pa_idxset_free(p->output_mappings, NULL, NULL);

    pa_xfree(p);
}

void pa_alsa_profile_set_free(pa_alsa_profile_set *ps) {
    pa_assert(ps);

    if (ps->profiles) {
        pa_alsa_profile *p;

        while ((p = pa_hashmap_steal_first(ps->profiles)))
            profile_free(p);

        pa_hashmap_free(ps->profiles, NULL, NULL);
    }

    if (ps->mappings) {
        pa_alsa_mapping *m;

        while ((m = pa_hashmap_steal_first(ps->mappings)))
            mapping_free(m);

        pa_hashmap_free(ps->mappings, NULL, NULL);
    }

    pa_xfree(ps);
}

static pa_alsa_mapping *mapping_get(pa_alsa_profile_set *ps, const char *name) {
    pa_alsa_mapping *m;

    if (!pa_startswith(name, "Mapping "))
        return NULL;

    name += 8;

    if ((m = pa_hashmap_get(ps->mappings, name)))
        return m;

    m = pa_xnew0(pa_alsa_mapping, 1);
    m->profile_set = ps;
    m->name = pa_xstrdup(name);
    pa_channel_map_init(&m->channel_map);

    pa_hashmap_put(ps->mappings, m->name, m);

    return m;
}

static pa_alsa_profile *profile_get(pa_alsa_profile_set *ps, const char *name) {
    pa_alsa_profile *p;

    if (!pa_startswith(name, "Profile "))
        return NULL;

    name += 8;

    if ((p = pa_hashmap_get(ps->profiles, name)))
        return p;

    p = pa_xnew0(pa_alsa_profile, 1);
    p->profile_set = ps;
    p->name = pa_xstrdup(name);

    pa_hashmap_put(ps->profiles, p->name, p);

    return p;
}

static int mapping_parse_device_strings(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_mapping *m;

    pa_assert(ps);

    if (!(m = mapping_get(ps, section))) {
        pa_log("[%s:%u] %s invalid in section %s", filename, line, lvalue, section);
        return -1;
    }

    pa_xstrfreev(m->device_strings);
    if (!(m->device_strings = pa_split_spaces_strv(rvalue))) {
        pa_log("[%s:%u] Device string list empty of '%s'", filename, line, section);
        return -1;
    }

    return 0;
}

static int mapping_parse_channel_map(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_mapping *m;

    pa_assert(ps);

    if (!(m = mapping_get(ps, section))) {
        pa_log("[%s:%u] %s invalid in section %s", filename, line, lvalue, section);
        return -1;
    }

    if (!(pa_channel_map_parse(&m->channel_map, rvalue))) {
        pa_log("[%s:%u] Channel map invalid of '%s'", filename, line, section);
        return -1;
    }

    return 0;
}

static int mapping_parse_paths(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_mapping *m;

    pa_assert(ps);

    if (!(m = mapping_get(ps, section))) {
        pa_log("[%s:%u] %s invalid in section %s", filename, line, lvalue, section);
        return -1;
    }

    if (pa_streq(lvalue, "paths-input")) {
        pa_xstrfreev(m->input_path_names);
        m->input_path_names = pa_split_spaces_strv(rvalue);
    } else {
        pa_xstrfreev(m->output_path_names);
        m->output_path_names = pa_split_spaces_strv(rvalue);
    }

    return 0;
}

static int mapping_parse_element(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_mapping *m;

    pa_assert(ps);

    if (!(m = mapping_get(ps, section))) {
        pa_log("[%s:%u] %s invalid in section %s", filename, line, lvalue, section);
        return -1;
    }

    if (pa_streq(lvalue, "element-input")) {
        pa_xstrfreev(m->input_element);
        m->input_element = pa_split_spaces_strv(rvalue);
    } else {
        pa_xstrfreev(m->output_element);
        m->output_element = pa_split_spaces_strv(rvalue);
    }

    return 0;
}

static int mapping_parse_direction(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_mapping *m;

    pa_assert(ps);

    if (!(m = mapping_get(ps, section))) {
        pa_log("[%s:%u] Section name %s invalid.", filename, line, section);
        return -1;
    }

    if (pa_streq(rvalue, "input"))
        m->direction = PA_ALSA_DIRECTION_INPUT;
    else if (pa_streq(rvalue, "output"))
        m->direction = PA_ALSA_DIRECTION_OUTPUT;
    else if (pa_streq(rvalue, "any"))
        m->direction = PA_ALSA_DIRECTION_ANY;
    else {
        pa_log("[%s:%u] Direction %s invalid.", filename, line, rvalue);
        return -1;
    }

    return 0;
}

static int mapping_parse_description(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_profile *p;
    pa_alsa_mapping *m;

    pa_assert(ps);

    if ((m = mapping_get(ps, section))) {
        pa_xstrdup(m->description);
        m->description = pa_xstrdup(rvalue);
    } else if ((p = profile_get(ps, section))) {
        pa_xfree(p->description);
        p->description = pa_xstrdup(rvalue);
    } else {
        pa_log("[%s:%u] Section name %s invalid.", filename, line, section);
        return -1;
    }

    return 0;
}

static int mapping_parse_priority(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_profile *p;
    pa_alsa_mapping *m;
    uint32_t prio;

    pa_assert(ps);

    if (pa_atou(rvalue, &prio) < 0) {
        pa_log("[%s:%u] Priority invalid of '%s'", filename, line, section);
        return -1;
    }

    if ((m = mapping_get(ps, section)))
        m->priority = prio;
    else if ((p = profile_get(ps, section)))
        p->priority = prio;
    else {
        pa_log("[%s:%u] Section name %s invalid.", filename, line, section);
        return -1;
    }

    return 0;
}

static int profile_parse_mappings(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_profile *p;

    pa_assert(ps);

    if (!(p = profile_get(ps, section))) {
        pa_log("[%s:%u] %s invalid in section %s", filename, line, lvalue, section);
        return -1;
    }

    if (pa_streq(lvalue, "input-mappings")) {
        pa_xstrfreev(p->input_mapping_names);
        p->input_mapping_names = pa_split_spaces_strv(rvalue);
    } else {
        pa_xstrfreev(p->output_mapping_names);
        p->output_mapping_names = pa_split_spaces_strv(rvalue);
    }

    return 0;
}

static int profile_parse_skip_probe(
        const char *filename,
        unsigned line,
        const char *section,
        const char *lvalue,
        const char *rvalue,
        void *data,
        void *userdata) {

    pa_alsa_profile_set *ps = userdata;
    pa_alsa_profile *p;
    int b;

    pa_assert(ps);

    if (!(p = profile_get(ps, section))) {
        pa_log("[%s:%u] %s invalid in section %s", filename, line, lvalue, section);
        return -1;
    }

    if ((b = pa_parse_boolean(rvalue)) < 0) {
        pa_log("[%s:%u] Skip probe invalid of '%s'", filename, line, section);
        return -1;
    }

    p->supported = b;

    return 0;
}

static int mapping_verify(pa_alsa_mapping *m, const pa_channel_map *bonus) {

    static const struct description_map well_known_descriptions[] = {
        { "analog-mono",            N_("Analog Mono") },
        { "analog-stereo",          N_("Analog Stereo") },
        { "analog-surround-21",     N_("Analog Surround 2.1") },
        { "analog-surround-30",     N_("Analog Surround 3.0") },
        { "analog-surround-31",     N_("Analog Surround 3.1") },
        { "analog-surround-40",     N_("Analog Surround 4.0") },
        { "analog-surround-41",     N_("Analog Surround 4.1") },
        { "analog-surround-50",     N_("Analog Surround 5.0") },
        { "analog-surround-51",     N_("Analog Surround 5.1") },
        { "analog-surround-61",     N_("Analog Surround 6.0") },
        { "analog-surround-61",     N_("Analog Surround 6.1") },
        { "analog-surround-70",     N_("Analog Surround 7.0") },
        { "analog-surround-71",     N_("Analog Surround 7.1") },
        { "iec958-stereo",          N_("Digital Stereo (IEC958)") },
        { "iec958-surround-40",     N_("Digital Surround 4.0 (IEC958)") },
        { "iec958-ac3-surround-40", N_("Digital Surround 4.0 (IEC958/AC3)") },
        { "iec958-ac3-surround-51", N_("Digital Surround 5.1 (IEC958/AC3)") },
        { "hdmi-stereo",            N_("Digital Stereo (HDMI)") }
    };

    pa_assert(m);

    if (!pa_channel_map_valid(&m->channel_map)) {
        pa_log("Mapping %s is missing channel map.", m->name);
        return -1;
    }

    if (!m->device_strings) {
        pa_log("Mapping %s is missing device strings.", m->name);
        return -1;
    }

    if ((m->input_path_names && m->input_element) ||
        (m->output_path_names && m->output_element)) {
        pa_log("Mapping %s must have either mixer path or mixer elment, not both.", m->name);
        return -1;
    }

    if (!m->description)
        m->description = pa_xstrdup(lookup_description(m->name,
                                                       well_known_descriptions,
                                                       PA_ELEMENTSOF(well_known_descriptions)));

    if (!m->description)
        m->description = pa_xstrdup(m->name);

    if (bonus) {
        if (pa_channel_map_equal(&m->channel_map, bonus))
            m->priority += 50;
        else if (m->channel_map.channels == bonus->channels)
            m->priority += 30;
    }

    return 0;
}

void pa_alsa_mapping_dump(pa_alsa_mapping *m) {
    char cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    pa_assert(m);

    pa_log_debug("Mapping %s (%s), priority=%u, channel_map=%s, supported=%s, direction=%i",
                 m->name,
                 pa_strnull(m->description),
                 m->priority,
                 pa_channel_map_snprint(cm, sizeof(cm), &m->channel_map),
                 pa_yes_no(m->supported),
                 m->direction);
}

static void profile_set_add_auto_pair(
        pa_alsa_profile_set *ps,
        pa_alsa_mapping *m, /* output */
        pa_alsa_mapping *n  /* input */) {

    char *name;
    pa_alsa_profile *p;

    pa_assert(ps);
    pa_assert(m || n);

    if (m && m->direction == PA_ALSA_DIRECTION_INPUT)
        return;

    if (n && n->direction == PA_ALSA_DIRECTION_OUTPUT)
        return;

    if (m && n)
        name = pa_sprintf_malloc("output:%s+input:%s", m->name, n->name);
    else if (m)
        name = pa_sprintf_malloc("output:%s", m->name);
    else
        name = pa_sprintf_malloc("input:%s", n->name);

    if (pa_hashmap_get(ps->profiles, name)) {
        pa_xfree(name);
        return;
    }

    p = pa_xnew0(pa_alsa_profile, 1);
    p->profile_set = ps;
    p->name = name;

    if (m) {
        p->output_mappings = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
        pa_idxset_put(p->output_mappings, m, NULL);
        p->priority += m->priority * 100;
    }

    if (n) {
        p->input_mappings = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
        pa_idxset_put(p->input_mappings, n, NULL);
        p->priority += n->priority;
    }

    pa_hashmap_put(ps->profiles, p->name, p);
}

static void profile_set_add_auto(pa_alsa_profile_set *ps) {
    pa_alsa_mapping *m, *n;
    void *m_state, *n_state;

    pa_assert(ps);

    PA_HASHMAP_FOREACH(m, ps->mappings, m_state) {
        profile_set_add_auto_pair(ps, m, NULL);

        PA_HASHMAP_FOREACH(n, ps->mappings, n_state)
            profile_set_add_auto_pair(ps, m, n);
    }

    PA_HASHMAP_FOREACH(n, ps->mappings, n_state)
        profile_set_add_auto_pair(ps, NULL, n);
}

static int profile_verify(pa_alsa_profile *p) {

    static const struct description_map well_known_descriptions[] = {
        { "output:analog-mono+input:analog-mono",     N_("Analog Mono Duplex") },
        { "output:analog-stereo+input:analog-stereo", N_("Analog Stereo Duplex") },
        { "output:iec958-stereo",                     N_("Digital Stereo Duplex (IEC958)") },
        { "off",                                      N_("Off") }
    };

    pa_assert(p);

    /* Replace the output mapping names by the actual mappings */
    if (p->output_mapping_names) {
        char **name;

        pa_assert(!p->output_mappings);
        p->output_mappings = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

        for (name = p->output_mapping_names; *name; name++) {
            pa_alsa_mapping *m;
            char **in;
            pa_bool_t duplicate = FALSE;

            for (in = name + 1; *in; in++)
                if (pa_streq(*name, *in)) {
                    duplicate = TRUE;
                    break;
                }

            if (duplicate)
                continue;

            if (!(m = pa_hashmap_get(p->profile_set->mappings, *name)) || m->direction == PA_ALSA_DIRECTION_INPUT) {
                pa_log("Profile '%s' refers to unexistant mapping '%s'.", p->name, *name);
                return -1;
            }

            pa_idxset_put(p->output_mappings, m, NULL);

            if (p->supported)
                m->supported++;
        }

        pa_xstrfreev(p->output_mapping_names);
        p->output_mapping_names = NULL;
    }

    /* Replace the input mapping names by the actual mappings */
    if (p->input_mapping_names) {
        char **name;

        pa_assert(!p->input_mappings);
        p->input_mappings = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

        for (name = p->input_mapping_names; *name; name++) {
            pa_alsa_mapping *m;
            char **in;
            pa_bool_t duplicate = FALSE;

            for (in = name + 1; *in; in++)
                if (pa_streq(*name, *in)) {
                    duplicate = TRUE;
                    break;
                }

            if (duplicate)
                continue;

            if (!(m = pa_hashmap_get(p->profile_set->mappings, *name)) || m->direction == PA_ALSA_DIRECTION_OUTPUT) {
                pa_log("Profile '%s' refers to unexistant mapping '%s'.", p->name, *name);
                return -1;
            }

            pa_idxset_put(p->input_mappings, m, NULL);

            if (p->supported)
                m->supported++;
        }

        pa_xstrfreev(p->input_mapping_names);
        p->input_mapping_names = NULL;
    }

    if (!p->input_mappings && !p->output_mappings) {
        pa_log("Profile '%s' lacks mappings.", p->name);
        return -1;
    }

    if (!p->description)
        p->description = pa_xstrdup(lookup_description(p->name,
                                                       well_known_descriptions,
                                                       PA_ELEMENTSOF(well_known_descriptions)));

    if (!p->description) {
        pa_strbuf *sb;
        uint32_t idx;
        pa_alsa_mapping *m;

        sb = pa_strbuf_new();

        if (p->output_mappings)
            PA_IDXSET_FOREACH(m, p->output_mappings, idx) {
                if (!pa_strbuf_isempty(sb))
                    pa_strbuf_puts(sb, " + ");

                pa_strbuf_printf(sb, "%s Output", m->description);
            }

        if (p->input_mappings)
            PA_IDXSET_FOREACH(m, p->input_mappings, idx) {
                if (!pa_strbuf_isempty(sb))
                    pa_strbuf_puts(sb, " + ");

                pa_strbuf_printf(sb, "%s Input", m->description);
            }

        p->description = pa_strbuf_tostring_free(sb);
    }

    return 0;
}

void pa_alsa_profile_dump(pa_alsa_profile *p) {
    uint32_t idx;
    pa_alsa_mapping *m;
    pa_assert(p);

    pa_log_debug("Profile %s (%s), priority=%u, supported=%s n_input_mappings=%u, n_output_mappings=%u",
                 p->name,
                 pa_strnull(p->description),
                 p->priority,
                 pa_yes_no(p->supported),
                 p->input_mappings ? pa_idxset_size(p->input_mappings) : 0,
                 p->output_mappings ? pa_idxset_size(p->output_mappings) : 0);

    if (p->input_mappings)
        PA_IDXSET_FOREACH(m, p->input_mappings, idx)
            pa_log_debug("Input %s", m->name);

    if (p->output_mappings)
        PA_IDXSET_FOREACH(m, p->output_mappings, idx)
            pa_log_debug("Output %s", m->name);
}

pa_alsa_profile_set* pa_alsa_profile_set_new(const char *fname, const pa_channel_map *bonus) {
    pa_alsa_profile_set *ps;
    pa_alsa_profile *p;
    pa_alsa_mapping *m;
    char *fn;
    int r;
    void *state;

    static pa_config_item items[] = {
        /* [General] */
        { "auto-profiles",          pa_config_parse_bool,         NULL, "General" },

        /* [Mapping ...] */
        { "device-strings",         mapping_parse_device_strings, NULL, NULL },
        { "channel-map",            mapping_parse_channel_map,    NULL, NULL },
        { "paths-input",            mapping_parse_paths,          NULL, NULL },
        { "paths-output",           mapping_parse_paths,          NULL, NULL },
        { "element-input",          mapping_parse_element,        NULL, NULL },
        { "element-output",         mapping_parse_element,        NULL, NULL },
        { "direction",              mapping_parse_direction,      NULL, NULL },

        /* Shared by [Mapping ...] and [Profile ...] */
        { "description",            mapping_parse_description,    NULL, NULL },
        { "priority",               mapping_parse_priority,       NULL, NULL },

        /* [Profile ...] */
        { "input-mappings",         profile_parse_mappings,       NULL, NULL },
        { "output-mappings",        profile_parse_mappings,       NULL, NULL },
        { "skip-probe",             profile_parse_skip_probe,     NULL, NULL },
        { NULL, NULL, NULL, NULL }
    };

    ps = pa_xnew0(pa_alsa_profile_set, 1);
    ps->mappings = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    ps->profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    items[0].data = &ps->auto_profiles;

    if (!fname)
        fname = "default.conf";

    fn = pa_maybe_prefix_path(fname,
#if defined(__linux__) && !defined(__OPTIMIZE__)
                              pa_run_from_build_tree() ? PA_BUILDDIR "/modules/alsa/mixer/profile-sets/" :
#endif
                              PA_ALSA_PROFILE_SETS_DIR);

    r = pa_config_parse(fn, NULL, items, ps);
    pa_xfree(fn);

    if (r < 0)
        goto fail;

    PA_HASHMAP_FOREACH(m, ps->mappings, state)
        if (mapping_verify(m, bonus) < 0)
            goto fail;

    if (ps->auto_profiles)
        profile_set_add_auto(ps);

    PA_HASHMAP_FOREACH(p, ps->profiles, state)
        if (profile_verify(p) < 0)
            goto fail;

    return ps;

fail:
    pa_alsa_profile_set_free(ps);
    return NULL;
}

void pa_alsa_profile_set_probe(
        pa_alsa_profile_set *ps,
        const char *dev_id,
        const pa_sample_spec *ss,
        unsigned default_n_fragments,
        unsigned default_fragment_size_msec) {

    void *state;
    pa_alsa_profile *p, *last = NULL;
    pa_alsa_mapping *m;

    pa_assert(ps);
    pa_assert(dev_id);
    pa_assert(ss);

    if (ps->probed)
        return;

    PA_HASHMAP_FOREACH(p, ps->profiles, state) {
        pa_sample_spec try_ss;
        pa_channel_map try_map;
        snd_pcm_uframes_t try_period_size, try_buffer_size;
        uint32_t idx;

        /* Is this already marked that it is supported? (i.e. from the config file) */
        if (p->supported)
            continue;

        pa_log_debug("Looking at profile %s", p->name);

        /* Close PCMs from the last iteration we don't need anymore */
        if (last && last->output_mappings)
            PA_IDXSET_FOREACH(m, last->output_mappings, idx) {

                if (!m->output_pcm)
                    break;

                if (last->supported)
                    m->supported++;

                if (!p->output_mappings || !pa_idxset_get_by_data(p->output_mappings, m, NULL)) {
                    snd_pcm_close(m->output_pcm);
                    m->output_pcm = NULL;
                }
            }

        if (last && last->input_mappings)
            PA_IDXSET_FOREACH(m, last->input_mappings, idx) {

                if (!m->input_pcm)
                    break;

                if (last->supported)
                    m->supported++;

                if (!p->input_mappings || !pa_idxset_get_by_data(p->input_mappings, m, NULL)) {
                    snd_pcm_close(m->input_pcm);
                    m->input_pcm = NULL;
                }
            }

        p->supported = TRUE;

        /* Check if we can open all new ones */
        if (p->output_mappings)
            PA_IDXSET_FOREACH(m, p->output_mappings, idx) {

                if (m->output_pcm)
                    continue;

                pa_log_debug("Checking for playback on %s (%s)", m->description, m->name);
                try_map = m->channel_map;
                try_ss = *ss;
                try_ss.channels = try_map.channels;

                try_period_size =
                    pa_usec_to_bytes(default_fragment_size_msec * PA_USEC_PER_MSEC, &try_ss) /
                    pa_frame_size(&try_ss);
                try_buffer_size = default_n_fragments * try_period_size;

                if (!(m ->output_pcm = pa_alsa_open_by_template(
                              m->device_strings,
                              dev_id,
                              NULL,
                              &try_ss, &try_map,
                              SND_PCM_STREAM_PLAYBACK,
                              &try_period_size, &try_buffer_size, 0, NULL, NULL,
                              TRUE))) {
                    p->supported = FALSE;
                    break;
                }
            }

        if (p->input_mappings && p->supported)
            PA_IDXSET_FOREACH(m, p->input_mappings, idx) {

                if (m->input_pcm)
                    continue;

                pa_log_debug("Checking for recording on %s (%s)", m->description, m->name);
                try_map = m->channel_map;
                try_ss = *ss;
                try_ss.channels = try_map.channels;

                try_period_size =
                    pa_usec_to_bytes(default_fragment_size_msec*PA_USEC_PER_MSEC, &try_ss) /
                    pa_frame_size(&try_ss);
                try_buffer_size = default_n_fragments * try_period_size;

                if (!(m ->input_pcm = pa_alsa_open_by_template(
                              m->device_strings,
                              dev_id,
                              NULL,
                              &try_ss, &try_map,
                              SND_PCM_STREAM_CAPTURE,
                              &try_period_size, &try_buffer_size, 0, NULL, NULL,
                              TRUE))) {
                    p->supported = FALSE;
                    break;
                }
            }

        last = p;

        if (p->supported)
            pa_log_debug("Profile %s supported.", p->name);
    }

    /* Clean up */
    if (last) {
        uint32_t idx;

        if (last->output_mappings)
            PA_IDXSET_FOREACH(m, last->output_mappings, idx)
                if (m->output_pcm) {

                    if (last->supported)
                        m->supported++;

                    snd_pcm_close(m->output_pcm);
                    m->output_pcm = NULL;
                }

        if (last->input_mappings)
            PA_IDXSET_FOREACH(m, last->input_mappings, idx)
                if (m->input_pcm) {

                    if (last->supported)
                        m->supported++;

                    snd_pcm_close(m->input_pcm);
                    m->input_pcm = NULL;
                }
    }

    PA_HASHMAP_FOREACH(p, ps->profiles, state)
        if (!p->supported) {
            pa_hashmap_remove(ps->profiles, p->name);
            profile_free(p);
        }

    PA_HASHMAP_FOREACH(m, ps->mappings, state)
        if (m->supported <= 0) {
            pa_hashmap_remove(ps->mappings, m->name);
            mapping_free(m);
        }

    ps->probed = TRUE;
}

void pa_alsa_profile_set_dump(pa_alsa_profile_set *ps) {
    pa_alsa_profile *p;
    pa_alsa_mapping *m;
    void *state;

    pa_assert(ps);

    pa_log_debug("Profile set %p, auto_profiles=%s, probed=%s, n_mappings=%u, n_profiles=%u",
                 (void*)
                 ps,
                 pa_yes_no(ps->auto_profiles),
                 pa_yes_no(ps->probed),
                 pa_hashmap_size(ps->mappings),
                 pa_hashmap_size(ps->profiles));

    PA_HASHMAP_FOREACH(m, ps->mappings, state)
        pa_alsa_mapping_dump(m);

    PA_HASHMAP_FOREACH(p, ps->profiles, state)
        pa_alsa_profile_dump(p);
}

void pa_alsa_add_ports(pa_hashmap **p, pa_alsa_path_set *ps) {
    pa_alsa_path *path;

    pa_assert(p);
    pa_assert(!*p);
    pa_assert(ps);

    /* if there is no path, we don't want a port list */
    if (!ps->paths)
        return;

    if (!ps->paths->next){
        pa_alsa_setting *s;

        /* If there is only one path, but no or only one setting, then
         * we want a port list either */
        if (!ps->paths->settings || !ps->paths->settings->next)
            return;

        /* Ok, there is only one path, however with multiple settings,
         * so let's create a port for each setting */
        *p = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

        PA_LLIST_FOREACH(s, ps->paths->settings) {
            pa_device_port *port;
            pa_alsa_port_data *data;

            port = pa_device_port_new(s->name, s->description, sizeof(pa_alsa_port_data));
            port->priority = s->priority;

            data = PA_DEVICE_PORT_DATA(port);
            data->path = ps->paths;
            data->setting = s;

            pa_hashmap_put(*p, port->name, port);
        }

    } else {

        /* We have multiple paths, so let's create a port for each
         * one, and each of each settings */
        *p = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

        PA_LLIST_FOREACH(path, ps->paths) {

            if (!path->settings || !path->settings->next) {
                pa_device_port *port;
                pa_alsa_port_data *data;

                /* If there is no or just one setting we only need a
                 * single entry */

                port = pa_device_port_new(path->name, path->description, sizeof(pa_alsa_port_data));
                port->priority = path->priority * 100;


                data = PA_DEVICE_PORT_DATA(port);
                data->path = path;
                data->setting = path->settings;

                pa_hashmap_put(*p, port->name, port);
            } else {
                pa_alsa_setting *s;

                PA_LLIST_FOREACH(s, path->settings) {
                    pa_device_port *port;
                    pa_alsa_port_data *data;
                    char *n, *d;

                    n = pa_sprintf_malloc("%s;%s", path->name, s->name);

                    if (s->description[0])
                        d = pa_sprintf_malloc(_("%s / %s"), path->description, s->description);
                    else
                        d = pa_xstrdup(path->description);

                    port = pa_device_port_new(n, d, sizeof(pa_alsa_port_data));
                    port->priority = path->priority * 100 + s->priority;

                    pa_xfree(n);
                    pa_xfree(d);

                    data = PA_DEVICE_PORT_DATA(port);
                    data->path = path;
                    data->setting = s;

                    pa_hashmap_put(*p, port->name, port);
                }
            }
        }
    }

    pa_log_debug("Added %u ports", pa_hashmap_size(*p));
}
