#ifndef fooraopclientfoo
#define fooraopclientfoo

/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#include <pulse/volume.h>

#include <pulsecore/core.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/rtpoll.h>

typedef enum pa_raop_protocol {
    PA_RAOP_PROTOCOL_TCP,
    PA_RAOP_PROTOCOL_UDP
} pa_raop_protocol_t;

typedef enum pa_raop_encryption {
    PA_RAOP_ENCRYPTION_NONE,
    PA_RAOP_ENCRYPTION_RSA,
    PA_RAOP_ENCRYPTION_FAIRPLAY,
    PA_RAOP_ENCRYPTION_MFISAP,
    PA_RAOP_ENCRYPTION_FAIRPLAY_SAP25
} pa_raop_encryption_t;

typedef enum pa_raop_codec {
    PA_RAOP_CODEC_PCM,
    PA_RAOP_CODEC_ALAC,
    PA_RAOP_CODEC_AAC,
    PA_RAOP_CODEC_AAC_ELD
} pa_raop_codec_t;

typedef struct pa_raop_client pa_raop_client;

typedef enum pa_raop_state {
    PA_RAOP_INVALID_STATE,
    PA_RAOP_AUTHENTICATED,
    PA_RAOP_CONNECTED,
    PA_RAOP_RECORDING,
    PA_RAOP_DISCONNECTED
} pa_raop_state_t;

pa_raop_client* pa_raop_client_new(pa_core *core, const char *host, pa_raop_protocol_t protocol,
                                   pa_raop_encryption_t encryption, pa_raop_codec_t codec, bool autoreconnect);
void pa_raop_client_free(pa_raop_client *c);

int pa_raop_client_authenticate(pa_raop_client *c, const char *password);
bool pa_raop_client_is_authenticated(pa_raop_client *c);

int pa_raop_client_announce(pa_raop_client *c);
bool pa_raop_client_is_alive(pa_raop_client *c);
bool pa_raop_client_is_recording(pa_raop_client *c);
bool pa_raop_client_can_stream(pa_raop_client *c);
int pa_raop_client_stream(pa_raop_client *c);
int pa_raop_client_set_volume(pa_raop_client *c, pa_volume_t volume);
int pa_raop_client_flush(pa_raop_client *c);
int pa_raop_client_teardown(pa_raop_client *c);
void pa_raop_client_disconnect(pa_raop_client *c);

void pa_raop_client_get_frames_per_block(pa_raop_client *c, size_t *size);
bool pa_raop_client_register_pollfd(pa_raop_client *c, pa_rtpoll *poll, pa_rtpoll_item **poll_item);
bool pa_raop_client_is_timing_fd(pa_raop_client *c, const int fd);
pa_volume_t pa_raop_client_adjust_volume(pa_raop_client *c, pa_volume_t volume);
void pa_raop_client_handle_oob_packet(pa_raop_client *c, const int fd, const uint8_t packet[], ssize_t size);
ssize_t pa_raop_client_send_audio_packet(pa_raop_client *c, pa_memchunk *block, size_t offset);

typedef void (*pa_raop_client_state_cb_t)(pa_raop_state_t state, void *userdata);
void pa_raop_client_set_state_callback(pa_raop_client *c, pa_raop_client_state_cb_t callback, void *userdata);

#endif
