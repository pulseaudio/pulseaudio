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

#define UDP_FRAMES_PER_PACKET 352

typedef enum pa_raop_protocol {
    RAOP_TCP,
    RAOP_UDP,
} pa_raop_protocol_t;

typedef struct pa_raop_client pa_raop_client;

pa_raop_client* pa_raop_client_new(pa_core *core, const char *host, pa_raop_protocol_t protocol);
void pa_raop_client_free(pa_raop_client *c);

int pa_raop_client_authenticate (pa_raop_client *c, const char *password);
int pa_raop_client_connect(pa_raop_client *c);
int pa_raop_client_flush(pa_raop_client *c);
int pa_raop_client_teardown(pa_raop_client *c);

int pa_raop_client_udp_is_authenticated(pa_raop_client *c);
int pa_raop_client_udp_is_alive(pa_raop_client *c);
int pa_raop_client_udp_can_stream(pa_raop_client *c);
int pa_raop_client_udp_stream(pa_raop_client *c);

void pa_raop_client_set_encryption(pa_raop_client *c, int encryption);
pa_volume_t pa_raop_client_adjust_volume(pa_raop_client *c, pa_volume_t volume);
int pa_raop_client_set_volume(pa_raop_client *c, pa_volume_t volume);
int pa_raop_client_encode_sample(pa_raop_client *c, pa_memchunk *raw, pa_memchunk *encoded);

int pa_raop_client_udp_handle_timing_packet(pa_raop_client *c, const uint8_t packet[], ssize_t size);
int pa_raop_client_udp_handle_control_packet(pa_raop_client *c, const uint8_t packet[], ssize_t size);
int pa_raop_client_udp_get_blocks_size(pa_raop_client *c, size_t *size);
ssize_t pa_raop_client_udp_send_audio_packet(pa_raop_client *c, pa_memchunk *block);

typedef void (*pa_raop_client_cb_t)(int fd, void *userdata);
void pa_raop_client_tcp_set_callback(pa_raop_client *c, pa_raop_client_cb_t callback, void *userdata);

typedef void (*pa_raop_client_closed_cb_t)(void *userdata);
void pa_raop_client_tcp_set_closed_callback(pa_raop_client *c, pa_raop_client_closed_cb_t callback, void *userdata);

typedef void (*pa_raop_client_auth_cb_t)(int status, void *userdata);
void pa_raop_client_udp_set_auth_callback(pa_raop_client *c, pa_raop_client_auth_cb_t callback, void *userdata);

typedef void (*pa_raop_client_setup_cb_t)(int control_fd, int timing_fd, void *userdata);
void pa_raop_client_udp_set_setup_callback(pa_raop_client *c, pa_raop_client_setup_cb_t callback, void *userdata);

typedef void (*pa_raop_client_record_cb_t)(void *userdata);
void pa_raop_client_udp_set_record_callback(pa_raop_client *c, pa_raop_client_record_cb_t callback, void *userdata);

typedef void (*pa_raop_client_disconnected_cb_t)(void *userdata);
void pa_raop_client_udp_set_disconnected_callback(pa_raop_client *c, pa_raop_client_disconnected_cb_t callback, void *userdata);

#endif
