/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/native-common.h>
#include <pulsecore/macro.h>

#include "pstream-util.h"

void pa_pstream_send_tagstruct_with_creds(pa_pstream *p, pa_tagstruct *t, const pa_creds *creds) {
    size_t length;
    uint8_t *data;
    pa_packet *packet;

    pa_assert(p);
    pa_assert(t);

    pa_assert_se(data = pa_tagstruct_free_data(t, &length));
    pa_assert_se(packet = pa_packet_new_dynamic(data, length));
    pa_pstream_send_packet(p, packet, creds);
    pa_packet_unref(packet);
}

void pa_pstream_send_error(pa_pstream *p, uint32_t tag, uint32_t error) {
    pa_tagstruct *t;

    pa_assert_se(t = pa_tagstruct_new(NULL, 0));
    pa_tagstruct_putu32(t, PA_COMMAND_ERROR);
    pa_tagstruct_putu32(t, tag);
    pa_tagstruct_putu32(t, error);
    pa_pstream_send_tagstruct(p, t);
}

void pa_pstream_send_simple_ack(pa_pstream *p, uint32_t tag) {
    pa_tagstruct *t;

    pa_assert_se(t = pa_tagstruct_new(NULL, 0));
    pa_tagstruct_putu32(t, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(t, tag);
    pa_pstream_send_tagstruct(p, t);
}
