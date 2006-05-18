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
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <polyp/timeval.h>
#include <polyp/util.h>
#include <polyp/xmalloc.h>

#include <polypcore/module.h>
#include <polypcore/llist.h>
#include <polypcore/source.h>
#include <polypcore/source-output.h>
#include <polypcore/memblockq.h>
#include <polypcore/log.h>
#include <polypcore/core-util.h>
#include <polypcore/modargs.h>
#include <polypcore/namereg.h>

#include "module-rtp-send-symdef.h"

#include "rtp.h"
#include "sdp.h"
#include "sap.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Read data from source and send it to the network via RTP/SAP/SDP")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE(
        "source=<name of the source> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "destination=<destination IP address> "
        "port=<port number> "
        "mtu=<maximum transfer unit> "
        "loop=<loopback to local host?>"
)

#define DEFAULT_PORT 46000
#define SAP_PORT 9875
#define DEFAULT_DESTINATION "224.0.0.56"
#define MEMBLOCKQ_MAXLENGTH (1024*170)
#define DEFAULT_MTU 1280
#define SAP_INTERVAL 5000000

static const char* const valid_modargs[] = {
    "source",
    "format",
    "channels",
    "rate",
    "destination",
    "port",
    "loop",
    NULL
};

struct userdata {
    pa_module *module;
    pa_core *core;

    pa_source_output *source_output;
    pa_memblockq *memblockq;

    pa_rtp_context rtp_context;
    pa_sap_context sap_context;
    size_t mtu;

    pa_time_event *sap_event;
};

static void source_output_push(pa_source_output *o, const pa_memchunk *chunk) {
    struct userdata *u;
    assert(o);
    u = o->userdata;

    if (pa_memblockq_push(u->memblockq, chunk) < 0) {
        pa_log(__FILE__": Failed to push chunk into memblockq.");
        return;
    }
    
    pa_rtp_send(&u->rtp_context, u->mtu, u->memblockq);
}

static void source_output_kill(pa_source_output* o) {
    struct userdata *u;
    assert(o);
    u = o->userdata;

    pa_module_unload_request(u->module);

    pa_source_output_disconnect(u->source_output);
    pa_source_output_unref(u->source_output);
    u->source_output = NULL;
}

static pa_usec_t source_output_get_latency (pa_source_output *o) {
    struct userdata *u;
    assert(o);
    u = o->userdata;

    return pa_bytes_to_usec(pa_memblockq_get_length(u->memblockq), &o->sample_spec);
}

static void sap_event_cb(pa_mainloop_api *m, pa_time_event *t, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;
    struct timeval next;
    
    assert(m);
    assert(t);
    assert(tv);
    assert(u);

    pa_sap_send(&u->sap_context, 0);

    pa_gettimeofday(&next);
    pa_timeval_add(&next, SAP_INTERVAL);
    m->time_restart(t, &next);
}

int pa__init(pa_core *c, pa_module*m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    const char *dest;
    uint32_t port = DEFAULT_PORT, mtu;
    int af, fd = -1, sap_fd = -1;
    pa_source *s;
    pa_sample_spec ss;
    pa_channel_map cm;
    struct sockaddr_in sa4, sap_sa4;
    struct sockaddr_in6 sa6, sap_sa6;
    struct sockaddr_storage sa_dst;
    pa_source_output *o = NULL;
    uint8_t payload;
    char *p;
    int r;
    socklen_t k;
    struct timeval tv;
    char hn[128], *n;
    int loop = 0;
    
    assert(c);
    assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments");
        goto fail;
    }

    if (!(s = pa_namereg_get(m->core, pa_modargs_get_value(ma, "source", NULL), PA_NAMEREG_SOURCE, 1))) {
        pa_log(__FILE__": source does not exist.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "loop", &loop) < 0) {
        pa_log(__FILE__": failed to parse \"loop\" parameter.");
        goto fail;
    }

    ss = s->sample_spec;
    pa_rtp_sample_spec_fixup(&ss);
    cm = s->channel_map;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": failed to parse sample specification");
        goto fail;
    }

    if (!pa_rtp_sample_spec_valid(&ss)) {
        pa_log(__FILE__": specified sample type not compatible with RTP");
        goto fail;
    }

    if (ss.channels != cm.channels)
        pa_channel_map_init_auto(&cm, ss.channels, PA_CHANNEL_MAP_AIFF);

    payload = pa_rtp_payload_from_sample_spec(&ss);

    mtu = (DEFAULT_MTU/pa_frame_size(&ss))*pa_frame_size(&ss);
    
    if (pa_modargs_get_value_u32(ma, "mtu", &mtu) < 0 || mtu < 1 || mtu % pa_frame_size(&ss) != 0) {
        pa_log(__FILE__": invalid mtu.");
        goto fail;
    }

    port = DEFAULT_PORT + ((rand() % 512) << 1);
    if (pa_modargs_get_value_u32(ma, "port", &port) < 0 || port < 1 || port > 0xFFFF) {
        pa_log(__FILE__": port= expects a numerical argument between 1 and 65535.");
        goto fail;
    }

    if (port & 1)
        pa_log_warn(__FILE__": WARNING: port number not even as suggested in RFC3550!");

    dest = pa_modargs_get_value(ma, "destination", DEFAULT_DESTINATION);

    if (inet_pton(AF_INET6, dest, &sa6.sin6_addr) > 0) {
        sa6.sin6_family = af = AF_INET6;
        sa6.sin6_port = htons(port);
        sap_sa6 = sa6;
        sap_sa6.sin6_port = htons(SAP_PORT);
    } else if (inet_pton(AF_INET, dest, &sa4.sin_addr) > 0) {
        sa4.sin_family = af = AF_INET;
        sa4.sin_port = htons(port);
        sap_sa4 = sa4;
        sap_sa4.sin_port = htons(SAP_PORT);
    } else {
        pa_log(__FILE__": invalid destination '%s'", dest);
        goto fail;
    }
    
    if ((fd = socket(af, SOCK_DGRAM, 0)) < 0) {
        pa_log(__FILE__": socket() failed: %s", strerror(errno));
        goto fail;
    }

    if (connect(fd, af == AF_INET ? (struct sockaddr*) &sa4 : (struct sockaddr*) &sa6, af == AF_INET ? sizeof(sa4) : sizeof(sa6)) < 0) {
        pa_log(__FILE__": connect() failed: %s", strerror(errno));
        goto fail;
    }

    if ((sap_fd = socket(af, SOCK_DGRAM, 0)) < 0) {
        pa_log(__FILE__": socket() failed: %s", strerror(errno));
        goto fail;
    }

    if (connect(sap_fd, af == AF_INET ? (struct sockaddr*) &sap_sa4 : (struct sockaddr*) &sap_sa6, af == AF_INET ? sizeof(sap_sa4) : sizeof(sap_sa6)) < 0) {
        pa_log(__FILE__": connect() failed: %s", strerror(errno));
        goto fail;
    }

    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0 ||
        setsockopt(sap_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        pa_log(__FILE__": IP_MULTICAST_LOOP failed: %s", strerror(errno));
        goto fail;
    }
    
    if (!(o = pa_source_output_new(s, __FILE__, "RTP Monitor Stream", &ss, &cm, PA_RESAMPLER_INVALID))) {
        pa_log(__FILE__": failed to create source output.");
        goto fail;
    }

    o->push = source_output_push;
    o->kill = source_output_kill;
    o->get_latency = source_output_get_latency;
    o->owner = m;
    
    u = pa_xnew(struct userdata, 1);
    m->userdata = u;
    o->userdata = u;

    u->module = m;
    u->core = c;
    u->source_output = o;
    
    u->memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            MEMBLOCKQ_MAXLENGTH,
            pa_frame_size(&ss),
            1,
            0,
            NULL,
            c->memblock_stat);

    u->mtu = mtu;
    
    k = sizeof(sa_dst);
    r = getsockname(fd, (struct sockaddr*) &sa_dst, &k);
    assert(r >= 0);

    n = pa_sprintf_malloc("Polypaudio RTP Stream on %s", pa_get_fqdn(hn, sizeof(hn)));
        
    p = pa_sdp_build(af,
                     af == AF_INET ? (void*) &((struct sockaddr_in*) &sa_dst)->sin_addr : (void*) &((struct sockaddr_in6*) &sa_dst)->sin6_addr,
                     af == AF_INET ? (void*) &sa4.sin_addr : (void*) &sa6.sin6_addr,
                     n, port, payload, &ss);

    pa_xfree(n);
    
    pa_rtp_context_init_send(&u->rtp_context, fd, c->cookie, payload, pa_frame_size(&ss));
    pa_sap_context_init_send(&u->sap_context, sap_fd, p);

    pa_log_info(__FILE__": RTP stream initialized with mtu %u on %s:%u, SSRC=0x%08x, payload=%u, initial sequence #%u", mtu, dest, port, u->rtp_context.ssrc, payload, u->rtp_context.sequence);
    pa_log_info(__FILE__": SDP-Data:\n%s\n"__FILE__": EOF", p);

    pa_sap_send(&u->sap_context, 0);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, SAP_INTERVAL);
    u->sap_event = c->mainloop->time_new(c->mainloop, &tv, sap_event_cb, u);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    if (fd >= 0)
        close(fd);
    
    if (sap_fd >= 0)
        close(sap_fd);

    if (o) {
        pa_source_output_disconnect(o);
        pa_source_output_unref(o);
    }
        
    return -1;
}

void pa__done(pa_core *c, pa_module*m) {
    struct userdata *u;
    assert(c);
    assert(m);

    if (!(u = m->userdata))
        return;

    c->mainloop->time_free(u->sap_event);
    
    if (u->source_output) {
        pa_source_output_disconnect(u->source_output);
        pa_source_output_unref(u->source_output);
    }

    pa_rtp_context_destroy(&u->rtp_context);

    pa_sap_send(&u->sap_context, 1);
    pa_sap_context_destroy(&u->sap_context);

    pa_memblockq_free(u->memblockq);
    
    pa_xfree(u);
}
