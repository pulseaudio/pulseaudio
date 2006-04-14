/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <polypcore/util.h>

#include "sdp.h"

static const char* map_format(pa_sample_format_t f) {
    switch (f) {
        case PA_SAMPLE_S16BE: return "L16";
        case PA_SAMPLE_U8: return "L8";
        case PA_SAMPLE_ALAW: return "PCMA";
        case PA_SAMPLE_ULAW: return "PCMU";
        default:
            return NULL;
    }
}

char *pa_sdp_build(int af, const void *src, const void *dst, const char *name, uint16_t port, uint8_t payload, const pa_sample_spec *ss) {
    uint32_t ntp;
    char buf_src[64], buf_dst[64];
    const char *u, *f, *a;

    assert(src);
    assert(dst);
    assert(af == AF_INET || af == AF_INET6);

    f = map_format(ss->format);
    assert(f);
    
    if (!(u = getenv("USER")))
        if (!(u = getenv("USERNAME")))
            u = "-";
    
    ntp = time(NULL) + 2208988800;

    a = inet_ntop(af, src, buf_src, sizeof(buf_src));
    assert(a);
    a = inet_ntop(af, dst, buf_dst, sizeof(buf_dst));
    assert(a);
    
    return pa_sprintf_malloc(
            "v=0\n"
            "o=%s %lu 0 IN %s %s\n"
            "s=%s\n"
            "c=IN %s %s\n"
            "t=%lu 0\n"
            "a=recvonly\n"
            "m=audio %u RTP/AVP %i\n"
            "a=rtpmap:%i %s/%u/%u\n"
            "a=type:broadcast\n",
            u, (unsigned long) ntp, af == AF_INET ? "IP4" : "IP6", buf_src,
            name,
            af == AF_INET ? "IP4" : "IP6", buf_dst,
            (unsigned long) ntp,
            port, payload,
            payload, f, ss->rate, ss->channels);
}
