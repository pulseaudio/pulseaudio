/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2008 Colin Guthrie
  Copyright 2013 Hajime Fujita
  Copyright 2013 Martin Blanchard

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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>

#ifdef HAVE_LINUX_SOCKIOS_H
#include <linux/sockios.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/poll.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/time-smoother.h>

#include "raop-sink.h"
#include "raop-client.h"
#include "raop-util.h"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_card *card;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_item;
    bool oob;

    pa_raop_client *raop;
    pa_raop_protocol_t protocol;
    pa_raop_encryption_t encryption;
    pa_raop_codec_t codec;

    size_t block_size;
    pa_memchunk memchunk;

    pa_usec_t delay;
    pa_usec_t start;
    pa_smoother *smoother;
    uint64_t write_count;

    uint32_t latency;
};

enum {
    PA_SINK_MESSAGE_SET_RAOP_STATE = PA_SINK_MESSAGE_MAX
};

static void userdata_free(struct userdata *u);

static void sink_set_volume_cb(pa_sink *s);

static void raop_state_cb(pa_raop_state_t state, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("State change recieved, informing IO thread...");

    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), PA_SINK_MESSAGE_SET_RAOP_STATE, PA_INT_TO_PTR(state), 0, NULL, NULL);
}

static int64_t sink_get_latency(const struct userdata *u) {
    pa_usec_t now;
    int64_t latency;

    pa_assert(u);
    pa_assert(u->smoother);

    now = pa_rtclock_now();
    now = pa_smoother_get(u->smoother, now);

    latency = pa_bytes_to_usec(u->write_count, &u->sink->sample_spec) - (int64_t) now;

    /* RAOP default latency */
    latency += u->latency * PA_USEC_PER_MSEC;

    return latency;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    pa_assert(u);
    pa_assert(u->raop);

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            int64_t r = 0;

            if (pa_raop_client_can_stream(u->raop))
                r = sink_get_latency(u);

            *((int64_t*) data) = r;

            return 0;
        }

        case PA_SINK_MESSAGE_SET_RAOP_STATE: {
            switch ((pa_raop_state_t) PA_PTR_TO_UINT(data)) {
                case PA_RAOP_AUTHENTICATED: {
                    if (!pa_raop_client_is_authenticated(u->raop)) {
                        pa_module_unload_request(u->module, true);
                    }

                    return 0;
                }

                case PA_RAOP_CONNECTED: {
                    pa_assert(!u->rtpoll_item);

                    u->oob = pa_raop_client_register_pollfd(u->raop, u->rtpoll, &u->rtpoll_item);

                    return 0;
                }

                case PA_RAOP_RECORDING: {
                    pa_usec_t now;

                    now = pa_rtclock_now();
                    pa_rtpoll_set_timer_absolute(u->rtpoll, now);
                    u->write_count = 0;
                    u->start = now;

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                        /* Our stream has been suspended so we just flush it... */
                        pa_rtpoll_set_timer_disabled(u->rtpoll);
                        pa_raop_client_flush(u->raop);
                    } else {
                        /* Set the initial volume */
                        sink_set_volume_cb(u->sink);
                    }

                    return 0;
                }

                case PA_RAOP_INVALID_STATE:
                case PA_RAOP_DISCONNECTED: {
                    unsigned int nbfds = 0;
                    struct pollfd *pollfd;
                    unsigned int i;

                    if (u->rtpoll_item) {
                        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, &nbfds);
                        if (pollfd) {
                            for (i = 0; i < nbfds; i++) {
                                if (pollfd->fd >= 0)
                                   pa_close(pollfd->fd);
                                pollfd++;
                            }
                        }
                        pa_rtpoll_item_free(u->rtpoll_item);
                        u->rtpoll_item = NULL;
                    }

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED)
                        pa_rtpoll_set_timer_disabled(u->rtpoll);
                    else if (u->sink->thread_info.state != PA_SINK_IDLE)
                        pa_module_unload_request(u->module, true);

                    return 0;
                }
            }

            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    /* It may be that only the suspend cause is changing, in which case there's
     * nothing to do. */
    if (new_state == s->thread_info.state)
        return 0;

    switch (new_state) {
        case PA_SINK_SUSPENDED:
            pa_log_debug("RAOP: SUSPENDED");

            pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

            /* Issue a TEARDOWN if we are still connected */
            if (pa_raop_client_is_alive(u->raop)) {
                pa_raop_client_teardown(u->raop);
            }

            break;

        case PA_SINK_IDLE:
            pa_log_debug("RAOP: IDLE");

            /* Issue a FLUSH if we're comming from running state */
            if (u->sink->thread_info.state == PA_SINK_RUNNING) {
                pa_rtpoll_set_timer_disabled(u->rtpoll);
                pa_raop_client_flush(u->raop);
            }

            break;

        case PA_SINK_RUNNING: {
            pa_usec_t now;

            pa_log_debug("RAOP: RUNNING");

            now = pa_rtclock_now();
            pa_smoother_reset(u->smoother, now, false);

            if (!pa_raop_client_is_alive(u->raop)) {
                /* Connecting will trigger a RECORD and start steaming */
                pa_raop_client_announce(u->raop);
            } else if (!pa_raop_client_can_stream(u->raop)) {
                /* RECORD alredy sent, simply start streaming */
                pa_raop_client_stream(u->raop);
                pa_rtpoll_set_timer_absolute(u->rtpoll, now);
                u->write_count = 0;
                u->start = now;
            }

            break;
        }

        case PA_SINK_UNLINKED:
        case PA_SINK_INIT:
        case PA_SINK_INVALID_STATE:
            break;
    }

    return 0;
}

static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume hw;
    pa_volume_t v, v_orig;
    char t[PA_CVOLUME_SNPRINT_VERBOSE_MAX];

    pa_assert(u);

    /* If we're muted we don't need to do anything. */
    if (s->muted)
        return;

    /* Calculate the max volume of all channels.
     * We'll use this as our (single) volume on the APEX device and emulate
     * any variation in channel volumes in software. */
    v = pa_cvolume_max(&s->real_volume);

    v_orig = v;
    v = pa_raop_client_adjust_volume(u->raop, v_orig);

    pa_log_debug("Volume adjusted: orig=%u adjusted=%u", v_orig, v);

    /* Create a pa_cvolume version of our single value. */
    pa_cvolume_set(&hw, s->sample_spec.channels, v);

    /* Perform any software manipulation of the volume needed. */
    pa_sw_cvolume_divide(&s->soft_volume, &s->real_volume, &hw);

    pa_log_debug("Requested volume: %s", pa_cvolume_snprint_verbose(t, sizeof(t), &s->real_volume, &s->channel_map, false));
    pa_log_debug("Got hardware volume: %s", pa_cvolume_snprint_verbose(t, sizeof(t), &hw, &s->channel_map, false));
    pa_log_debug("Calculated software volume: %s",
                 pa_cvolume_snprint_verbose(t, sizeof(t), &s->soft_volume, &s->channel_map, true));

    /* Any necessary software volume manipulation is done so set
     * our hw volume (or v as a single value) on the device. */
    pa_raop_client_set_volume(u->raop, v);
}

static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);
    pa_assert(u->raop);

    if (s->muted) {
        pa_raop_client_set_volume(u->raop, PA_VOLUME_MUTED);
    } else {
        sink_set_volume_cb(s);
    }
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    size_t offset = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);
    pa_smoother_set_time_offset(u->smoother, pa_rtclock_now());

    for (;;) {
        struct pollfd *pollfd = NULL;
        unsigned int i, nbfds = 0;
        pa_usec_t now, estimated, intvl;
        uint64_t position;
        size_t index;
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->sink->thread_info.rewind_requested)
                pa_sink_process_rewind(u->sink, 0);
        }

        /* Polling (audio data + control socket + timing socket). */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;
        else if (ret == 0)
            goto finish;

        if (u->rtpoll_item) {
            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, &nbfds);
            /* If !oob: streaming driven by pollds (POLLOUT) */
            if (pollfd && !u->oob && !pollfd->revents) {
                for (i = 0; i < nbfds; i++) {
                    pollfd->events = POLLOUT;
                    pollfd->revents = 0;

                    pollfd++;
                }

                continue;
            }

            /* if oob: streaming managed by timing, pollfd for oob sockets */
            if (pollfd && u->oob && !pa_rtpoll_timer_elapsed(u->rtpoll)) {
                uint8_t packet[32];
                ssize_t read;

                for (i = 0; i < nbfds; i++) {
                    if (pollfd->revents & pollfd->events) {
                        pollfd->revents = 0;
                        read = pa_read(pollfd->fd, packet, sizeof(packet), NULL);
                        pa_raop_client_handle_oob_packet(u->raop, pollfd->fd, packet, read);
                    }

                    pollfd++;
                }

                continue;
            }
        }

        if (u->sink->thread_info.state != PA_SINK_RUNNING)
            continue;
        if (!pa_raop_client_can_stream(u->raop))
            continue;

        /* This assertion is meant to silence a complaint from Coverity about
         * pollfd being possibly NULL when we access it later. That's a false
         * positive, because we check pa_raop_client_can_stream() above, and if
         * that returns true, it means that the connection is up, and when the
         * connection is up, pollfd will be non-NULL. */
        pa_assert(pollfd);

        if (u->memchunk.length <= 0) {
            if (u->memchunk.memblock)
                pa_memblock_unref(u->memchunk.memblock);
            pa_memchunk_reset(&u->memchunk);

            /* Grab unencoded audio data from PulseAudio */
            pa_sink_render_full(u->sink, u->block_size, &u->memchunk);
            offset = u->memchunk.index;
        }

        pa_assert(u->memchunk.length > 0);

        index = u->memchunk.index;
        if (pa_raop_client_send_audio_packet(u->raop, &u->memchunk, offset) < 0) {
            if (errno == EINTR) {
                /* Just try again. */
                pa_log_debug("Failed to write data to FIFO (EINTR), retrying");
                goto fail;
            } else if (errno != EAGAIN) {
                /* Buffer is full, wait for POLLOUT. */
                pollfd->events = POLLOUT;
                pollfd->revents = 0;
            } else {
                pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
                goto fail;
            }
        } else {
            u->write_count += (uint64_t) u->memchunk.index - (uint64_t) index;
            position = u->write_count - pa_usec_to_bytes(u->delay, &u->sink->sample_spec);

            now = pa_rtclock_now();
            estimated = pa_bytes_to_usec(position, &u->sink->sample_spec);
            pa_smoother_put(u->smoother, now, estimated);

            if (u->oob && !pollfd->revents) {
                /* Sleep until next packet transmission */
                intvl = u->start + pa_bytes_to_usec(u->write_count, &u->sink->sample_spec);
                pa_rtpoll_set_timer_absolute(u->rtpoll, intvl);
            } else if (!u->oob) {
                if (u->memchunk.length > 0) {
                    pollfd->events = POLLOUT;
                    pollfd->revents = 0;
                } else {
                    intvl = u->start + pa_bytes_to_usec(u->write_count, &u->sink->sample_spec);
                    pa_rtpoll_set_timer_absolute(u->rtpoll, intvl);
                    pollfd->revents = 0;
                    pollfd->events = 0;
                }
            }
        }
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

static int sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    return 0;
}

static pa_device_port *raop_create_port(struct userdata *u, const char *server) {
    pa_device_port_new_data data;
    pa_device_port *port;

    pa_device_port_new_data_init(&data);

    pa_device_port_new_data_set_name(&data, "network-output");
    pa_device_port_new_data_set_description(&data, server);
    pa_device_port_new_data_set_direction(&data, PA_DIRECTION_OUTPUT);

    port = pa_device_port_new(u->core, &data, 0);

    pa_device_port_new_data_done(&data);

    if (port == NULL)
        return NULL;

    pa_device_port_ref(port);

    return port;
}

static pa_card_profile *raop_create_profile() {
    pa_card_profile *profile;

    profile = pa_card_profile_new("RAOP", _("RAOP standard profile"), 0);
    profile->priority = 10;
    profile->n_sinks = 1;
    profile->n_sources = 0;
    profile->max_sink_channels = 2;
    profile->max_source_channels = 0;

    return profile;
}

static pa_card *raop_create_card(pa_module *m, pa_device_port *port, pa_card_profile *profile, const char *server, const char *nicename) {
    pa_card_new_data data;
    pa_card *card;
    char *card_name;

    pa_card_new_data_init(&data);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, server);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, nicename);
    data.driver = __FILE__;

    card_name = pa_sprintf_malloc("raop_client.%s", server);
    pa_card_new_data_set_name(&data, card_name);
    pa_xfree(card_name);

    pa_hashmap_put(data.ports, port->name, port);
    pa_hashmap_put(data.profiles, profile->name, profile);

    card = pa_card_new(m->core, &data);

    pa_card_new_data_done(&data);

    if (card == NULL)
        return NULL;

    pa_card_choose_initial_profile(card);

    pa_card_put(card);

    return card;
}

pa_sink* pa_raop_sink_new(pa_module *m, pa_modargs *ma, const char *driver) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    char *thread_name = NULL;
    const char *server, *protocol, *encryption, *codec;
    const char /* *username, */ *password;
    pa_sink_new_data data;
    const char *name = NULL;
    const char *description = NULL;
    pa_device_port *port;
    pa_card_profile *profile;

    pa_assert(m);
    pa_assert(ma);

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log("Failed to parse sample specification");
        goto fail;
    }

    if (!(server = pa_modargs_get_value(ma, "server", NULL))) {
        pa_log("Failed to parse server argument");
        goto fail;
    }

    if (!(protocol = pa_modargs_get_value(ma, "protocol", NULL))) {
        pa_log("Failed to parse protocol argument");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->thread = NULL;
    u->rtpoll = pa_rtpoll_new();
    u->rtpoll_item = NULL;
    u->latency = RAOP_DEFAULT_LATENCY;

    if (pa_modargs_get_value_u32(ma, "latency_msec", &u->latency) < 0) {
        pa_log("Failed to parse latency_msec argument");
        goto fail;
    }

    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        pa_log("pa_thread_mq_init() failed.");
        goto fail;
    }

    u->oob = true;

    u->block_size = 0;
    pa_memchunk_reset(&u->memchunk);

    u->delay = 0;
    u->smoother = pa_smoother_new(
            PA_USEC_PER_SEC,
            PA_USEC_PER_SEC*2,
            true,
            true,
            10,
            0,
            false);
    u->write_count = 0;

    if (pa_streq(protocol, "TCP")) {
        u->protocol = PA_RAOP_PROTOCOL_TCP;
    } else if (pa_streq(protocol, "UDP")) {
        u->protocol = PA_RAOP_PROTOCOL_UDP;
    } else {
        pa_log("Unsupported transport protocol argument: %s", protocol);
        goto fail;
    }

    encryption = pa_modargs_get_value(ma, "encryption", NULL);
    codec = pa_modargs_get_value(ma, "codec", NULL);

    if (!encryption) {
        u->encryption = PA_RAOP_ENCRYPTION_NONE;
    } else if (pa_streq(encryption, "none")) {
        u->encryption = PA_RAOP_ENCRYPTION_NONE;
    } else if (pa_streq(encryption, "RSA")) {
        u->encryption = PA_RAOP_ENCRYPTION_RSA;
    } else {
        pa_log("Unsupported encryption type argument: %s", encryption);
        goto fail;
    }

    if (!codec) {
        u->codec = PA_RAOP_CODEC_PCM;
    } else if (pa_streq(codec, "PCM")) {
        u->codec = PA_RAOP_CODEC_PCM;
    } else if (pa_streq(codec, "ALAC")) {
        u->codec = PA_RAOP_CODEC_ALAC;
    } else {
        pa_log("Unsupported audio codec argument: %s", codec);
        goto fail;
    }

    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;

    if ((name = pa_modargs_get_value(ma, "sink_name", NULL))) {
        pa_sink_new_data_set_name(&data, name);
    } else {
        char *nick;

        if ((name = pa_modargs_get_value(ma, "name", NULL)))
            nick = pa_sprintf_malloc("raop_client.%s", name);
        else
            nick = pa_sprintf_malloc("raop_client.%s", server);
        pa_sink_new_data_set_name(&data, nick);
        pa_xfree(nick);
    }

    pa_sink_new_data_set_sample_spec(&data, &ss);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, server);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_INTENDED_ROLES, "music");
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "RAOP sink '%s'", server);

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    port = raop_create_port(u, server);
    if (port == NULL) {
        pa_log("Failed to create port object");
        goto fail;
    }

    profile = raop_create_profile();
    pa_hashmap_put(port->profiles, profile->name, profile);

    description = pa_proplist_gets(data.proplist, PA_PROP_DEVICE_DESCRIPTION);
    if (description == NULL)
        description = server;

    u->card = raop_create_card(m, port, profile, server, description);
    if (u->card == NULL) {
        pa_log("Failed to create card object");
        goto fail;
    }

    data.card = u->card;
    pa_hashmap_put(data.ports, port->name, port);

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY | PA_SINK_NETWORK);
    pa_sink_new_data_done(&data);

    if (!(u->sink)) {
        pa_log("Failed to create sink object");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);
    pa_sink_set_set_mute_callback(u->sink, sink_set_mute_cb);
    u->sink->userdata = u;
    u->sink->set_port = sink_set_port_cb;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->raop = pa_raop_client_new(u->core, server, u->protocol, u->encryption, u->codec);

    if (!(u->raop)) {
        pa_log("Failed to create RAOP client object");
        goto fail;
    }

    /* The number of frames per blocks is not negotiable... */
    pa_raop_client_get_frames_per_block(u->raop, &u->block_size);
    u->block_size *= pa_frame_size(&ss);
    pa_sink_set_max_request(u->sink, u->block_size);

    pa_raop_client_set_state_callback(u->raop, raop_state_cb, u);

    thread_name = pa_sprintf_malloc("raop-sink-%s", server);
    if (!(u->thread = pa_thread_new(thread_name, thread_func, u))) {
        pa_log("Failed to create sink thread");
        goto fail;
    }
    pa_xfree(thread_name);
    thread_name = NULL;

    pa_sink_put(u->sink);

    /* username = pa_modargs_get_value(ma, "username", NULL); */
    password = pa_modargs_get_value(ma, "password", NULL);
    pa_raop_client_authenticate(u->raop, password );

    return u->sink;

fail:
    pa_xfree(thread_name);

    if (u)
        userdata_free(u);

    return NULL;
}

static void userdata_free(struct userdata *u) {
    pa_assert(u);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);
    u->sink = NULL;

    if (u->rtpoll_item)
        pa_rtpoll_item_free(u->rtpoll_item);
    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);
    u->rtpoll_item = NULL;
    u->rtpoll = NULL;

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->raop)
        pa_raop_client_free(u->raop);
    u->raop = NULL;

    if (u->smoother)
        pa_smoother_free(u->smoother);
    u->smoother = NULL;

    if (u->card)
        pa_card_free(u->card);

    pa_xfree(u);
}

void pa_raop_sink_free(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    userdata_free(u);
}
