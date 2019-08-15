#ifndef foortspclienthfoo
#define foortspclienthfoo

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

#include <inttypes.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>

#include <pulsecore/socket-client.h>
#include <pulse/mainloop-api.h>

#include "headerlist.h"

typedef struct pa_rtsp_client pa_rtsp_client;

typedef enum pa_rtsp_state {
  STATE_CONNECT,
  STATE_OPTIONS,
  STATE_ANNOUNCE,
  STATE_SETUP,
  STATE_RECORD,
  STATE_SET_PARAMETER,
  STATE_FLUSH,
  STATE_TEARDOWN,
  STATE_DISCONNECTED
} pa_rtsp_state_t;

typedef enum pa_rtsp_status {
  STATUS_OK             = 200,
  STATUS_BAD_REQUEST    = 400,
  STATUS_UNAUTHORIZED   = 401,
  STATUS_NO_RESPONSE    = 444,
  STATUS_INTERNAL_ERROR = 500
} pa_rtsp_status_t;

typedef void (*pa_rtsp_cb_t)(pa_rtsp_client *c, pa_rtsp_state_t state, pa_rtsp_status_t code, pa_headerlist *headers, void *userdata);

pa_rtsp_client* pa_rtsp_client_new(pa_mainloop_api *mainloop, const char *hostname, uint16_t port, const char *useragent, bool autoreconnect);
void pa_rtsp_client_free(pa_rtsp_client *c);

int pa_rtsp_connect(pa_rtsp_client *c);
void pa_rtsp_set_callback(pa_rtsp_client *c, pa_rtsp_cb_t callback, void *userdata);
void pa_rtsp_disconnect(pa_rtsp_client *c);

const char* pa_rtsp_localip(pa_rtsp_client *c);
uint32_t pa_rtsp_serverport(pa_rtsp_client *c);
bool pa_rtsp_exec_ready(const pa_rtsp_client *c);

void pa_rtsp_set_url(pa_rtsp_client *c, const char *url);

bool pa_rtsp_has_header(pa_rtsp_client *c, const char *key);
void pa_rtsp_add_header(pa_rtsp_client *c, const char *key, const char *value);
const char* pa_rtsp_get_header(pa_rtsp_client *c, const char *key);
void pa_rtsp_remove_header(pa_rtsp_client *c, const char *key);

int pa_rtsp_options(pa_rtsp_client *c);
int pa_rtsp_announce(pa_rtsp_client *c, const char *sdp);
int pa_rtsp_setup(pa_rtsp_client *c, const char *transport);
int pa_rtsp_record(pa_rtsp_client *c, uint16_t *seq, uint32_t *rtptime);
int pa_rtsp_setparameter(pa_rtsp_client *c, const char *param);
int pa_rtsp_flush(pa_rtsp_client *c, uint16_t seq, uint32_t rtptime);
int pa_rtsp_teardown(pa_rtsp_client *c);

#endif
