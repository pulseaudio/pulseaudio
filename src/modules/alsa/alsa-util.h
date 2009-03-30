#ifndef fooalsautilhfoo
#define fooalsautilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
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

#include <asoundlib.h>

#include <pulse/sample.h>
#include <pulse/volume.h>
#include <pulse/mainloop-api.h>
#include <pulse/channelmap.h>
#include <pulse/proplist.h>

#include <pulsecore/rtpoll.h>
#include <pulsecore/core.h>

typedef struct pa_alsa_fdlist pa_alsa_fdlist;

struct pa_alsa_fdlist *pa_alsa_fdlist_new(void);
void pa_alsa_fdlist_free(struct pa_alsa_fdlist *fdl);
int pa_alsa_fdlist_set_mixer(struct pa_alsa_fdlist *fdl, snd_mixer_t *mixer_handle, pa_mainloop_api* m);

int pa_alsa_set_hw_params(
        snd_pcm_t *pcm_handle,
        pa_sample_spec *ss,
        uint32_t *periods,
        snd_pcm_uframes_t *period_size,
        snd_pcm_uframes_t tsched_size,
        pa_bool_t *use_mmap,
        pa_bool_t *use_tsched,
        pa_bool_t require_exact_channel_number);

int pa_alsa_set_sw_params(snd_pcm_t *pcm, snd_pcm_uframes_t avail_min);

int pa_alsa_prepare_mixer(snd_mixer_t *mixer, const char *dev);
snd_mixer_elem_t *pa_alsa_find_elem(snd_mixer_t *mixer, const char *name, const char *fallback, pa_bool_t playback);
int pa_alsa_find_mixer_and_elem(snd_pcm_t *pcm, snd_mixer_t **_m, snd_mixer_elem_t **_e, const char *control_name);

typedef struct pa_alsa_profile_info {
    pa_channel_map map;
    const char *alsa_name;
    const char *description; /* internationalized */
    const char *name;
    unsigned priority;
} pa_alsa_profile_info;

/* Picks a working profile based on the specified ss/map */
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
        const pa_alsa_profile_info **profile);

/* Uses the specified profile */
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
        const pa_alsa_profile_info *profile);

/* Opens the explicit ALSA device */
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
        pa_bool_t require_exact_channel_number);

int pa_alsa_probe_profiles(
        const char *dev_id,
        const pa_sample_spec *ss,
        void (*cb)(const pa_alsa_profile_info *sink, const pa_alsa_profile_info *source, void *userdata),
        void *userdata);

int pa_alsa_calc_mixer_map(snd_mixer_elem_t *elem, const pa_channel_map *channel_map, snd_mixer_selem_channel_id_t mixer_map[], pa_bool_t playback);

void pa_alsa_dump(snd_pcm_t *pcm);
void pa_alsa_dump_status(snd_pcm_t *pcm);

void pa_alsa_redirect_errors_inc(void);
void pa_alsa_redirect_errors_dec(void);

void pa_alsa_init_proplist_pcm_info(pa_core *c, pa_proplist *p, snd_pcm_info_t *pcm_info);
void pa_alsa_init_proplist_card(pa_core *c, pa_proplist *p, int card);
void pa_alsa_init_proplist_pcm(pa_core *c, pa_proplist *p, snd_pcm_t *pcm);
pa_bool_t pa_alsa_init_description(pa_proplist *p);

int pa_alsa_recover_from_poll(snd_pcm_t *pcm, int revents);

pa_rtpoll_item* pa_alsa_build_pollfd(snd_pcm_t *pcm, pa_rtpoll *rtpoll);

snd_pcm_sframes_t pa_alsa_safe_avail(snd_pcm_t *pcm, size_t hwbuf_size, const pa_sample_spec *ss);
int pa_alsa_safe_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay, size_t hwbuf_size, const pa_sample_spec *ss);
int pa_alsa_safe_mmap_begin(snd_pcm_t *pcm, const snd_pcm_channel_area_t **areas, snd_pcm_uframes_t *offset, snd_pcm_uframes_t *frames, size_t hwbuf_size, const pa_sample_spec *ss);

char *pa_alsa_get_driver_name(int card);

char *pa_alsa_get_driver_name_by_pcm(snd_pcm_t *pcm);

char *pa_alsa_get_reserve_name(const char *device);

pa_bool_t pa_alsa_pcm_is_hw(snd_pcm_t *pcm);

#endif
