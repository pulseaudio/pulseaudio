/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/llist.h>
#include <pulsecore/source.h>
#include <pulsecore/source-output.h>
#include <pulsecore/memblockq.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/socket-util.h>

#include "module-rtp-send-symdef.h"

#include "rtp.h"
#include "sdp.h"
#include "sap.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Read data from source and send it to the network via RTP/SAP/SDP");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "source=<name of the source> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "destination=<destination IP address> "
        "port=<port number> "
        "mtu=<maximum transfer unit> "
        "loop=<loopback to local host?> "
        "ttl=<ttl value>"
);

#define DEFAULT_PORT 46000
#define DEFAULT_TTL 1
#define SAP_PORT 9875
#define DEFAULT_DESTINATION "224.0.0.56"
#define MEMBLOCKQ_MAXLENGTH (1024*170)
#define DEFAULT_MTU 1280
#define SAP_INTERVAL (5*PA_USEC_PER_SEC)

static const char* const valid_modargs[] = {
    "source",
    "format",
    "channels",
    "rate",
    "destination",
    "port",
    "mtu" ,
    "loop",
    "ttl",
    NULL
};

struct userdata {
    pa_module *module;

    pa_source_output *source_output;
    pa_memblockq *memblockq;

    pa_rtp_context rtp_context;
    pa_sap_context sap_context;
    size_t mtu;

    pa_time_event *sap_event;
};

/* Called from I/O thread context */
static int source_output_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u;
    pa_assert_se(u = PA_SOURCE_OUTPUT(o)->userdata);

    switch (code) {
        case PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY:
            *((pa_usec_t*) data) = pa_bytes_to_usec(pa_memblockq_get_length(u->memblockq), &u->source_output->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
    }

    return pa_source_output_process_msg(o, code, data, offset, chunk);
}

/* Called from I/O thread context */
static void source_output_push(pa_source_output *o, const pa_memchunk *chunk) {
    struct userdata *u;
    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    if (pa_memblockq_push(u->memblockq, chunk) < 0) {
        pa_log_warn("Failed to push chunk into memblockq.");
        return;
    }

    pa_rtp_send(&u->rtp_context, u->mtu, u->memblockq);
}

/* Called from main context */
static void source_output_kill(pa_source_output* o) {
    struct userdata *u;
    pa_source_output_assert_ref(o);
    pa_assert_se(u = o->userdata);

    pa_module_unload_request(u->module, TRUE);

    pa_source_output_unlink(u->source_output);
    pa_source_output_unref(u->source_output);
    u->source_output = NULL;
}

static void sap_event_cb(pa_mainloop_api *m, pa_time_event *t, const struct timeval *tv, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(m);
    pa_assert(t);
    pa_assert(u);

    pa_sap_send(&u->sap_context, 0);

    pa_core_rttime_restart(u->module->core, t, pa_rtclock_now() + SAP_INTERVAL);
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    const char *dest;
    uint32_t port = DEFAULT_PORT, mtu;
    uint32_t ttl = DEFAULT_TTL;
    sa_family_t af;
    int fd = -1, sap_fd = -1;
    pa_source *s;
    pa_sample_spec ss;
    pa_channel_map cm;
    struct sockaddr_in sa4, sap_sa4;
#ifdef HAVE_IPV6
    struct sockaddr_in6 sa6, sap_sa6;
#endif
    struct sockaddr_storage sa_dst;
    pa_source_output *o = NULL;
    uint8_t payload;
    char *p;
    int r, j;
    socklen_t k;
    char hn[128], *n;
    pa_bool_t loop = FALSE;
    pa_source_output_new_data data;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (!(s = pa_namereg_get(m->core, pa_modargs_get_value(ma, "source", NULL), PA_NAMEREG_SOURCE))) {
        pa_log("Source does not exist.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "loop", &loop) < 0) {
        pa_log("Failed to parse \"loop\" parameter.");
        goto fail;
    }

    ss = s->sample_spec;
    pa_rtp_sample_spec_fixup(&ss);
    cm = s->channel_map;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log("Failed to parse sample specification");
        goto fail;
    }

    if (!pa_rtp_sample_spec_valid(&ss)) {
        pa_log("Specified sample type not compatible with RTP");
        goto fail;
    }

    if (ss.channels != cm.channels)
        pa_channel_map_init_auto(&cm, ss.channels, PA_CHANNEL_MAP_AIFF);

    payload = pa_rtp_payload_from_sample_spec(&ss);

    mtu = (uint32_t) pa_frame_align(DEFAULT_MTU, &ss);

    if (pa_modargs_get_value_u32(ma, "mtu", &mtu) < 0 || mtu < 1 || mtu % pa_frame_size(&ss) != 0) {
        pa_log("Invalid MTU.");
        goto fail;
    }

    port = DEFAULT_PORT + ((uint32_t) (rand() % 512) << 1);
    if (pa_modargs_get_value_u32(ma, "port", &port) < 0 || port < 1 || port > 0xFFFF) {
        pa_log("port= expects a numerical argument between 1 and 65535.");
        goto fail;
    }

    if (port & 1)
        pa_log_warn("Port number not even as suggested in RFC3550!");

    if (pa_modargs_get_value_u32(ma, "ttl", &ttl) < 0 || ttl < 1 || ttl > 0xFF) {
        pa_log("ttl= expects a numerical argument between 1 and 255.");
        goto fail;
    }

    dest = pa_modargs_get_value(ma, "destination", DEFAULT_DESTINATION);

    if (inet_pton(AF_INET, dest, &sa4.sin_addr) > 0) {
        sa4.sin_family = af = AF_INET;
        sa4.sin_port = htons((uint16_t) port);
        sap_sa4 = sa4;
        sap_sa4.sin_port = htons(SAP_PORT);
#ifdef HAVE_IPV6
    } else if (inet_pton(AF_INET6, dest, &sa6.sin6_addr) > 0) {
        sa6.sin6_family = af = AF_INET6;
        sa6.sin6_port = htons((uint16_t) port);
        sap_sa6 = sa6;
        sap_sa6.sin6_port = htons(SAP_PORT);
#endif
    } else {
        pa_log("Invalid destination '%s'", dest);
        goto fail;
    }

    if ((fd = socket(af, SOCK_DGRAM, 0)) < 0) {
        pa_log("socket() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (af == AF_INET && connect(fd, (struct sockaddr*) &sa4, sizeof(sa4)) < 0) {
        pa_log("connect() failed: %s", pa_cstrerror(errno));
        goto fail;
#ifdef HAVE_IPV6
    } else if (af == AF_INET6 && connect(fd, (struct sockaddr*) &sa6, sizeof(sa6)) < 0) {
        pa_log("connect() failed: %s", pa_cstrerror(errno));
        goto fail;
#endif
    }

    if ((sap_fd = socket(af, SOCK_DGRAM, 0)) < 0) {
        pa_log("socket() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (af == AF_INET && connect(sap_fd, (struct sockaddr*) &sap_sa4, sizeof(sap_sa4)) < 0) {
        pa_log("connect() failed: %s", pa_cstrerror(errno));
        goto fail;
#ifdef HAVE_IPV6
    } else if (af == AF_INET6 && connect(sap_fd, (struct sockaddr*) &sap_sa6, sizeof(sap_sa6)) < 0) {
        pa_log("connect() failed: %s", pa_cstrerror(errno));
        goto fail;
#endif
    }

    j = !!loop;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &j, sizeof(j)) < 0 ||
        setsockopt(sap_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &j, sizeof(j)) < 0) {
        pa_log("IP_MULTICAST_LOOP failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (ttl != DEFAULT_TTL) {
        int _ttl = (int) ttl;

        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &_ttl, sizeof(_ttl)) < 0) {
            pa_log("IP_MULTICAST_TTL failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        if (setsockopt(sap_fd, IPPROTO_IP, IP_MULTICAST_TTL, &_ttl, sizeof(_ttl)) < 0) {
            pa_log("IP_MULTICAST_TTL (sap) failed: %s", pa_cstrerror(errno));
            goto fail;
        }
    }

    /* If the socket queue is full, let's drop packets */
    pa_make_fd_nonblock(fd);
    pa_make_udp_socket_low_delay(fd);
    pa_make_fd_cloexec(fd);
    pa_make_fd_cloexec(sap_fd);

    pa_source_output_new_data_init(&data);
    pa_proplist_sets(data.proplist, PA_PROP_MEDIA_NAME, "RTP Monitor Stream");
    pa_proplist_sets(data.proplist, "rtp.destination", dest);
    pa_proplist_setf(data.proplist, "rtp.mtu", "%lu", (unsigned long) mtu);
    pa_proplist_setf(data.proplist, "rtp.port", "%lu", (unsigned long) port);
    pa_proplist_setf(data.proplist, "rtp.ttl", "%lu", (unsigned long) ttl);
    data.driver = __FILE__;
    data.module = m;
    data.source = s;
    pa_source_output_new_data_set_sample_spec(&data, &ss);
    pa_source_output_new_data_set_channel_map(&data, &cm);
    data.flags = PA_SOURCE_OUTPUT_DONT_INHIBIT_AUTO_SUSPEND;

    pa_source_output_new(&o, m->core, &data);
    pa_source_output_new_data_done(&data);

    if (!o) {
        pa_log("failed to create source output.");
        goto fail;
    }

    o->parent.process_msg = source_output_process_msg;
    o->push = source_output_push;
    o->kill = source_output_kill;

    pa_log_info("Configured source latency of %llu ms.",
                (unsigned long long) pa_source_output_set_requested_latency(o, pa_bytes_to_usec(mtu, &o->sample_spec)) / PA_USEC_PER_MSEC);

    m->userdata = o->userdata = u = pa_xnew(struct userdata, 1);
    u->module = m;
    u->source_output = o;

    u->memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            MEMBLOCKQ_MAXLENGTH,
            pa_frame_size(&ss),
            1,
            0,
            0,
            NULL);

    u->mtu = mtu;

    k = sizeof(sa_dst);
    pa_assert_se((r = getsockname(fd, (struct sockaddr*) &sa_dst, &k)) >= 0);

    n = pa_sprintf_malloc("PulseAudio RTP Stream on %s", pa_get_fqdn(hn, sizeof(hn)));

    if (af == AF_INET) {
        p = pa_sdp_build(af,
                     (void*) &((struct sockaddr_in*) &sa_dst)->sin_addr,
                     (void*) &sa4.sin_addr,
                     n, (uint16_t) port, payload, &ss);
#ifdef HAVE_IPV6
    } else {
        p = pa_sdp_build(af,
                     (void*) &((struct sockaddr_in6*) &sa_dst)->sin6_addr,
                     (void*) &sa6.sin6_addr,
                     n, (uint16_t) port, payload, &ss);
#endif
    }

    pa_xfree(n);

    pa_rtp_context_init_send(&u->rtp_context, fd, m->core->cookie, payload, pa_frame_size(&ss));
    pa_sap_context_init_send(&u->sap_context, sap_fd, p);

    pa_log_info("RTP stream initialized with mtu %u on %s:%u ttl=%u, SSRC=0x%08x, payload=%u, initial sequence #%u", mtu, dest, port, ttl, u->rtp_context.ssrc, payload, u->rtp_context.sequence);
    pa_log_info("SDP-Data:\n%s\nEOF", p);

    pa_sap_send(&u->sap_context, 0);

    u->sap_event = pa_core_rttime_new(m->core, pa_rtclock_now() + SAP_INTERVAL, sap_event_cb, u);

    pa_source_output_put(u->source_output);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    if (fd >= 0)
        pa_close(fd);

    if (sap_fd >= 0)
        pa_close(sap_fd);

    if (o) {
        pa_source_output_unlink(o);
        pa_source_output_unref(o);
    }

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sap_event)
        m->core->mainloop->time_free(u->sap_event);

    if (u->source_output) {
        pa_source_output_unlink(u->source_output);
        pa_source_output_unref(u->source_output);
    }

    pa_rtp_context_destroy(&u->rtp_context);

    pa_sap_send(&u->sap_context, 1);
    pa_sap_context_destroy(&u->sap_context);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    pa_xfree(u);
}
