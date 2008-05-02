#ifndef foortsphfoo
#define foortsphfoo

/* $Id: rtp.h 1465 2007-05-29 17:24:48Z lennart $ */

/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

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

#include <inttypes.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>

#include <pulsecore/memblockq.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/socket-client.h>

#include "headerlist.h"

typedef struct pa_rtsp_context {
    int fd;
    const char* useragent;
    pa_headerlist* headers;
    char* url;
    uint32_t port;
    uint32_t cseq;
    char* session;
    char* transport;
    struct in_addr local_addr;
} pa_rtsp_context;

pa_rtsp_context* pa_rtsp_context_new(const char* useragent);
void pa_rtsp_context_free(pa_rtsp_context* c);

int pa_rtsp_connect(pa_rtsp_context* c, const char* hostname, uint16_t port, const char* sid);
void pa_rtsp_disconnect(pa_rtsp_context* c);

const char* pa_rtsp_localip(pa_rtsp_context* c);
int pa_rtsp_announce(pa_rtsp_context* c, const char* sdp);

int pa_rtsp_setup(pa_rtsp_context* c, pa_headerlist** response_headers);
int pa_rtsp_record(pa_rtsp_context* c);
int pa_rtsp_teardown(pa_rtsp_context* c);

int pa_rtsp_setparameter(pa_rtsp_context* c, const char* param);
int pa_rtsp_flush(pa_rtsp_context* c);

#endif
