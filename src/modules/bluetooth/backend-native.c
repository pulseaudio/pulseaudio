/***
  This file is part of PulseAudio.

  Copyright 2014 Wim Taymans <wim.taymans at gmail.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/shared.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/log.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#include "bluez5-util.h"
#include "bt-codec-msbc.h"

struct pa_bluetooth_backend {
  pa_core *core;
  pa_dbus_connection *connection;
  pa_bluetooth_discovery *discovery;
  bool enable_shared_profiles;
  bool enable_hsp_hs;
  bool enable_hfp_hf;

  PA_LLIST_HEAD(pa_dbus_pending, pending);
};

struct transport_data {
    int rfcomm_fd;
    pa_io_event *rfcomm_io;
    int sco_fd;
    pa_io_event *sco_io;
    pa_mainloop_api *mainloop;
};

struct hfp_config {
    uint32_t capabilities;
    int state;
    bool support_codec_negotiation;
    bool support_msbc;
    int selected_codec;
};

/*
 * the separate hansfree headset (HF) and Audio Gateway (AG) features
 */
enum hfp_hf_features {
    HFP_HF_EC_NR = 0,
    HFP_HF_CALL_WAITING = 1,
    HFP_HF_CLI = 2,
    HFP_HF_VR = 3,
    HFP_HF_RVOL = 4,
    HFP_HF_ESTATUS = 5,
    HFP_HF_ECALL = 6,
    HFP_HF_CODECS = 7,
};

enum hfp_ag_features {
    HFP_AG_THREE_WAY = 0,
    HFP_AG_EC_NR = 1,
    HFP_AG_VR = 2,
    HFP_AG_RING = 3,
    HFP_AG_NUM_TAG = 4,
    HFP_AG_REJECT = 5,
    HFP_AG_ESTATUS = 6,
    HFP_AG_ECALL = 7,
    HFP_AG_EERR = 8,
    HFP_AG_CODECS = 9,
};

/* gateway features we support, which is as little as we can get away with */
static uint32_t hfp_features =
    /* HFP 1.6 requires this */
    (1 << HFP_AG_ESTATUS ) | (1 << HFP_AG_CODECS);

#define HSP_AG_PROFILE "/Profile/HSPAGProfile"
#define HFP_AG_PROFILE "/Profile/HFPAGProfile"
#define HSP_HS_PROFILE "/Profile/HSPHSProfile"

/* RFCOMM channel for HSP headset role
 * The choice seems to be a bit arbitrary -- it looks like at least channels 2, 4 and 5 also work*/
#define HSP_HS_DEFAULT_CHANNEL  3

#define PROFILE_INTROSPECT_XML                                          \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
    "<node>"                                                            \
    " <interface name=\"" BLUEZ_PROFILE_INTERFACE "\">"                 \
    "  <method name=\"Release\">"                                       \
    "  </method>"                                                       \
    "  <method name=\"RequestDisconnection\">"                          \
    "   <arg name=\"device\" direction=\"in\" type=\"o\"/>"             \
    "  </method>"                                                       \
    "  <method name=\"NewConnection\">"                                 \
    "   <arg name=\"device\" direction=\"in\" type=\"o\"/>"             \
    "   <arg name=\"fd\" direction=\"in\" type=\"h\"/>"                 \
    "   <arg name=\"opts\" direction=\"in\" type=\"a{sv}\"/>"           \
    "  </method>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"" DBUS_INTERFACE_INTROSPECTABLE "\">"           \
    "  <method name=\"Introspect\">"                                    \
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
    "  </method>"                                                       \
    " </interface>"                                                     \
    "</node>"

static pa_volume_t hsp_gain_to_volume(uint16_t gain) {
    pa_volume_t volume = (pa_volume_t) ((
        gain * PA_VOLUME_NORM
        /* Round to closest by adding half the denominator */
        + HSP_MAX_GAIN / 2
    ) / HSP_MAX_GAIN);

    if (volume > PA_VOLUME_NORM)
        volume = PA_VOLUME_NORM;

    return volume;
}

static uint16_t volume_to_hsp_gain(pa_volume_t volume) {
    uint16_t gain = volume * HSP_MAX_GAIN / PA_VOLUME_NORM;

    if (gain > HSP_MAX_GAIN)
        gain = HSP_MAX_GAIN;

    return gain;
}

static bool is_peer_audio_gateway(pa_bluetooth_profile_t peer_profile) {
    switch(peer_profile) {
        case PA_BLUETOOTH_PROFILE_HFP_HF:
        case PA_BLUETOOTH_PROFILE_HSP_HS:
            return false;
        case PA_BLUETOOTH_PROFILE_HFP_AG:
        case PA_BLUETOOTH_PROFILE_HSP_AG:
            return true;
        default:
            pa_assert_not_reached();
    }
}

static bool is_pulseaudio_audio_gateway(pa_bluetooth_profile_t peer_profile) {
    return !is_peer_audio_gateway(peer_profile);
}

static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_backend *backend, DBusMessage *m,
        DBusPendingCallNotifyFunction func, void *call_data) {

    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(backend);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(pa_dbus_connection_get(backend->connection), m, &call, -1));

    p = pa_dbus_pending_new(pa_dbus_connection_get(backend->connection), m, call, backend, call_data);
    PA_LLIST_PREPEND(pa_dbus_pending, backend->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static void rfcomm_fmt_write(int fd, const char* fmt_line, const char *fmt_command, va_list ap)
{
    size_t len;
    char buf[512];
    char command[512];

    pa_vsnprintf(command, sizeof(command), fmt_command, ap);

    pa_log_debug("RFCOMM >> %s", command);

    len = pa_snprintf(buf, sizeof(buf), fmt_line, command);

    /* we ignore any errors, it's not critical and real errors should
     * be caught with the HANGUP and ERROR events handled above */

    if ((size_t)write(fd, buf, len) != len)
        pa_log_error("RFCOMM write error: %s", pa_cstrerror(errno));
}

/* The format of COMMAND line sent from HS to AG is COMMAND<cr> */
static void rfcomm_write_command(int fd, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    rfcomm_fmt_write(fd, "%s\r", fmt, ap);
    va_end(ap);
}

/* The format of RESPONSE line sent from AG to HS is <cr><lf>RESPONSE<cr><lf> */
static void rfcomm_write_response(int fd, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    rfcomm_fmt_write(fd, "\r\n%s\r\n", fmt, ap);
    va_end(ap);
}

static int sco_setsockopt_enable_bt_voice(pa_bluetooth_transport *t, int fd) {
    /* the mSBC codec requires a special transparent eSCO connection */
    struct bt_voice voice;

    memset(&voice, 0, sizeof(voice));
    voice.setting = BT_VOICE_TRANSPARENT;
    if (setsockopt(fd, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) < 0) {
        pa_log_error("sockopt(): %s", pa_cstrerror(errno));
        return -1;
    }
    pa_log_info("Enabled BT_VOICE_TRANSPARENT connection for mSBC");
    return 0;
}

static int sco_do_connect(pa_bluetooth_transport *t) {
    pa_bluetooth_device *d = t->device;
    struct sockaddr_sco addr;
    socklen_t len;
    int err, i;
    int sock;
    bdaddr_t src;
    bdaddr_t dst;
    const char *src_addr, *dst_addr;

    src_addr = d->adapter->address;
    dst_addr = d->address;

    /* don't use ba2str to avoid -lbluetooth */
    for (i = 5; i >= 0; i--, src_addr += 3)
        src.b[i] = strtol(src_addr, NULL, 16);
    for (i = 5; i >= 0; i--, dst_addr += 3)
        dst.b[i] = strtol(dst_addr, NULL, 16);

    sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
    if (sock < 0) {
        pa_log_error("socket(SEQPACKET, SCO) %s", pa_cstrerror(errno));
        return -1;
    }

    len = sizeof(addr);
    memset(&addr, 0, len);
    addr.sco_family = AF_BLUETOOTH;
    bacpy(&addr.sco_bdaddr, &src);

    if (bind(sock, (struct sockaddr *) &addr, len) < 0) {
        pa_log_error("bind(): %s", pa_cstrerror(errno));
        goto fail_close;
    }

    if (t->setsockopt && t->setsockopt(t, sock) < 0)
        goto fail_close;

    memset(&addr, 0, len);
    addr.sco_family = AF_BLUETOOTH;
    bacpy(&addr.sco_bdaddr, &dst);

    pa_log_info("doing connect");
    err = connect(sock, (struct sockaddr *) &addr, len);
    if (err < 0 && !(errno == EAGAIN || errno == EINPROGRESS)) {
        pa_log_error("connect(): %s", pa_cstrerror(errno));
        goto fail_close;
    }
    return sock;

fail_close:
    close(sock);
    return -1;
}

static int sco_do_accept(pa_bluetooth_transport *t) {
    struct transport_data *trd = t->userdata;
    struct sockaddr_sco addr;
    socklen_t optlen;
    int sock;

    memset(&addr, 0, sizeof(addr));
    optlen = sizeof(addr);

    pa_log_info ("doing accept");
    sock = accept(trd->sco_fd, (struct sockaddr *) &addr, &optlen);
    if (sock < 0) {
        if (errno != EAGAIN)
            pa_log_error("accept(): %s", pa_cstrerror(errno));
        goto fail;
    }
    return sock;

fail:
    return -1;
}

static int sco_acquire_cb(pa_bluetooth_transport *t, bool optional, size_t *imtu, size_t *omtu) {
    int sock;
    socklen_t len;

    if (optional)
        sock = sco_do_accept(t);
    else
        sock = sco_do_connect(t);

    if (sock < 0)
        goto fail;

    if (imtu) *imtu = 60;
    if (omtu) *omtu = 60;

    if (t->device->autodetect_mtu) {
        struct sco_options sco_opt;

        len = sizeof(sco_opt);
        memset(&sco_opt, 0, len);

        if (getsockopt(sock, SOL_SCO, SCO_OPTIONS, &sco_opt, &len) < 0)
            pa_log_warn("getsockopt(SCO_OPTIONS) failed, loading defaults");
        else {
            pa_log_debug("autodetected imtu = omtu = %u", sco_opt.mtu);
            if (imtu) *imtu = sco_opt.mtu;
            if (omtu) *omtu = sco_opt.mtu;
        }
    }

    /* read/decode machinery only works if we get at most one MSBC encoded packet at a time
     * when it is fixed to process stream of packets, lift this assertion */
    pa_assert(*imtu <= MSBC_PACKET_SIZE);
    pa_assert(*omtu <= MSBC_PACKET_SIZE);

    return sock;

fail:
    return -1;
}

static void sco_release_cb(pa_bluetooth_transport *t) {
    pa_log_info("Transport %s released", t->path);
    /* device will close the SCO socket for us */
}

static ssize_t sco_transport_write(pa_bluetooth_transport *t, int fd, const void* buffer, size_t size, size_t write_mtu) {
    ssize_t l = 0;
    size_t written = 0;
    size_t write_size;

    pa_assert(t);

    /* since SCO setup is symmetric, fix write MTU to be size of last read packet */
    if (t->last_read_size)
        write_mtu = PA_MIN(t->last_read_size, write_mtu);

    /* if encoder buffer has less data than required to make complete packet */
    if (size < write_mtu)
        return 0;

    /* write out MTU sized chunks only */
    while (written < size) {
        write_size = PA_MIN(size - written, write_mtu);
        if (write_size < write_mtu)
            break;
        l = pa_write(fd, buffer + written, write_size, &t->stream_write_type);
        if (l < 0)
            break;
        written += l;
    }

    if (l < 0) {
        if (errno == EAGAIN) {
            /* Hmm, apparently the socket was not writable, give up for now */
            pa_log_debug("Got EAGAIN on write() after POLLOUT, probably there is a temporary connection loss.");
            /* Drain write buffer */
            written = size;
        } else if (errno == EINVAL && t->last_read_size == 0) {
            /* Likely write_link_mtu is still wrong, retry after next successful read */
            pa_log_debug("got write EINVAL, next successful read should fix MTU");
            /* Drain write buffer */
            written = size;
        } else {
            pa_log_error("Failed to write data to socket: %s", pa_cstrerror(errno));
            /* Report error from write call */
            return -1;
        }
    }

    /* if too much data left discard it all */
    if (size - written >= write_mtu) {
        pa_log_warn("Wrote memory block to socket only partially! %lu written, discarding pending write size %lu larger than write_mtu %lu",
                    written, size, write_mtu);
        /* Drain write buffer */
        written = size;
    }

    return written;
}

static void sco_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_bluetooth_transport *t = userdata;

    pa_assert(io);
    pa_assert(t);

    if (events & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) {
        pa_log_error("error listening SCO connection: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (t->state != PA_BLUETOOTH_TRANSPORT_STATE_PLAYING) {
        pa_log_info("SCO incoming connection: changing state to PLAYING");
        pa_bluetooth_transport_set_state (t, PA_BLUETOOTH_TRANSPORT_STATE_PLAYING);
    }

fail:
    return;
}

static int sco_listen(pa_bluetooth_transport *t) {
    struct transport_data *trd = t->userdata;
    struct sockaddr_sco addr;
    int sock, i;
    bdaddr_t src;
    const char *src_addr;

    sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_SCO);
    if (sock < 0) {
        pa_log_error("socket(SEQPACKET, SCO) %s", pa_cstrerror(errno));
        return -1;
    }

    src_addr = t->device->adapter->address;

    /* don't use ba2str to avoid -lbluetooth */
    for (i = 5; i >= 0; i--, src_addr += 3)
        src.b[i] = strtol(src_addr, NULL, 16);

    /* Bind to local address */
    memset(&addr, 0, sizeof(addr));
    addr.sco_family = AF_BLUETOOTH;
    bacpy(&addr.sco_bdaddr, &src);

    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        pa_log_error("bind(): %s", pa_cstrerror(errno));
        goto fail_close;
    }

    pa_log_info ("doing listen");
    if (listen(sock, 1) < 0) {
        pa_log_error("listen(): %s", pa_cstrerror(errno));
        goto fail_close;
    }

    trd->sco_fd = sock;
    trd->sco_io = trd->mainloop->io_new(trd->mainloop, sock, PA_IO_EVENT_INPUT,
        sco_io_callback, t);

    return sock;

fail_close:
    close(sock);
    return -1;
}

static void register_profile_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_backend *b;
    pa_bluetooth_profile_t profile;

    pa_assert(pending);
    pa_assert_se(p = userdata);
    pa_assert_se(b = p->context_data);
    pa_assert_se(profile = (pa_bluetooth_profile_t)p->call_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
        pa_log_info("Couldn't register profile %s because it is disabled in BlueZ", pa_bluetooth_profile_to_string(profile));
        profile_status_set(b->discovery, profile, PA_BLUETOOTH_PROFILE_STATUS_ACTIVE);
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error(BLUEZ_PROFILE_MANAGER_INTERFACE ".RegisterProfile() failed: %s: %s", dbus_message_get_error_name(r),
                     pa_dbus_get_error_message(r));
        profile_status_set(b->discovery, profile, PA_BLUETOOTH_PROFILE_STATUS_ACTIVE);
        goto finish;
    }

    profile_status_set(b->discovery, profile, PA_BLUETOOTH_PROFILE_STATUS_REGISTERED);

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, b->pending, p);
    pa_dbus_pending_free(p);
}

static void register_profile(pa_bluetooth_backend *b, const char *object, const char *uuid, pa_bluetooth_profile_t profile) {
    DBusMessage *m;
    DBusMessageIter i, d;
    dbus_bool_t autoconnect;
    dbus_uint16_t version, chan;

    pa_assert(profile_status_get(b->discovery, profile) == PA_BLUETOOTH_PROFILE_STATUS_ACTIVE);

    pa_log_debug("Registering Profile %s %s", pa_bluetooth_profile_to_string(profile), uuid);

    pa_assert_se(m = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez", BLUEZ_PROFILE_MANAGER_INTERFACE, "RegisterProfile"));

    dbus_message_iter_init_append(m, &i);
    pa_assert_se(dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &object));
    pa_assert_se(dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &uuid));
    dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
                                     DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING
                                     DBUS_TYPE_VARIANT_AS_STRING
                                     DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                     &d);
    if (pa_bluetooth_uuid_is_hsp_hs(uuid)) {
        /* In the headset role, the connection will only be initiated from the remote side */
        autoconnect = 0;
        pa_dbus_append_basic_variant_dict_entry(&d, "AutoConnect", DBUS_TYPE_BOOLEAN, &autoconnect);
        chan = HSP_HS_DEFAULT_CHANNEL;
        pa_dbus_append_basic_variant_dict_entry(&d, "Channel", DBUS_TYPE_UINT16, &chan);
        /* HSP version 1.2 */
        version = 0x0102;
        pa_dbus_append_basic_variant_dict_entry(&d, "Version", DBUS_TYPE_UINT16, &version);
    }
    dbus_message_iter_close_container(&i, &d);

    profile_status_set(b->discovery, profile, PA_BLUETOOTH_PROFILE_STATUS_REGISTERING);
    send_and_add_to_pending(b, m, register_profile_reply, (void *)profile);
}

static void transport_put(pa_bluetooth_transport *t)
{
    pa_bluetooth_transport_put(t);

    pa_log_debug("Transport %s available for profile %s", t->path, pa_bluetooth_profile_to_string(t->profile));
}

static pa_volume_t set_sink_volume(pa_bluetooth_transport *t, pa_volume_t volume);
static pa_volume_t set_source_volume(pa_bluetooth_transport *t, pa_volume_t volume);

static bool hfp_rfcomm_handle(int fd, pa_bluetooth_transport *t, const char *buf)
{
    struct hfp_config *c = t->config;
    int val;
    char str[5];
    const char *r;
    size_t len;
    const char *state;

    /* first-time initialize selected codec to CVSD */
    if (c->selected_codec == 0)
        c->selected_codec = 1;

    /* stateful negotiation */
    if (c->state == 0 && sscanf(buf, "AT+BRSF=%d", &val) == 1) {
        c->capabilities = val;
        pa_log_info("HFP capabilities returns 0x%x", val);
        rfcomm_write_response(fd, "+BRSF: %d", hfp_features);
        c->state = 1;

        return true;
    } else if (sscanf(buf, "AT+BAC=%3s", str) == 1) {
        c->support_msbc = false;

        state = NULL;

        /* check if codec id 2 (mSBC) is in the list of supported codecs */
        while ((r = pa_split_in_place(str, ",", &len, &state))) {
            if (len == 1 && r[0] == '2') {
                c->support_msbc = true;
                break;
            }
        }

        c->support_codec_negotiation = true;

        if (c->state == 1) {
            /* initial list of codecs supported by HF */
        } else {
            /* HF sent updated list of codecs */
        }

        /* no state change */

        return true;
    } else if (c->state == 1 && pa_startswith(buf, "AT+CIND=?")) {
        /* we declare minimal no indicators */
        rfcomm_write_response(fd, "+CIND: "
                     /* many indicators can be supported, only call and
                      * callheld are mandatory, so that's all we repy */
                     "(\"service\",(0-1)),"
                     "(\"call\",(0-1)),"
                     "(\"callsetup\",(0-3)),"
                     "(\"callheld\",(0-2))");
        c->state = 2;

        return true;
    } else if (c->state == 2 && pa_startswith(buf, "AT+CIND?")) {
        rfcomm_write_response(fd, "+CIND: 0,0,0,0");
        c->state = 3;

        return true;
    } else if ((c->state == 2 || c->state == 3) && pa_startswith(buf, "AT+CMER=")) {
        rfcomm_write_response(fd, "OK");

        if (c->support_codec_negotiation) {
            if (c->support_msbc && pa_bluetooth_discovery_get_enable_msbc(t->device->discovery)) {
                rfcomm_write_response(fd, "+BCS:2");
                c->state = 4;
            } else {
                rfcomm_write_response(fd, "+BCS:1");
                c->state = 4;
            }
        } else {
            c->state = 5;
            pa_bluetooth_transport_reconfigure(t, pa_bluetooth_get_hf_codec("CVSD"), sco_transport_write, NULL);
            transport_put(t);
        }

        return false;
    } else if (sscanf(buf, "AT+BCS=%d", &val)) {
        if (val == 1) {
            pa_bluetooth_transport_reconfigure(t, pa_bluetooth_get_hf_codec("CVSD"), sco_transport_write, NULL);
        } else if (val == 2 && pa_bluetooth_discovery_get_enable_msbc(t->device->discovery)) {
            pa_bluetooth_transport_reconfigure(t, pa_bluetooth_get_hf_codec("mSBC"), sco_transport_write, sco_setsockopt_enable_bt_voice);
        } else {
            pa_assert_fp(val != 1 && val != 2);
            rfcomm_write_response(fd, "ERROR");
            return false;
        }

        c->selected_codec = val;

        if (c->state == 4) {
            c->state = 5;
            pa_log_info("HFP negotiated codec %s", t->bt_codec->name);
            transport_put(t);
        }

        return true;
    } if (c->state == 4) {
        /* the ack for the codec setting may take a while. we need
         * to reply OK to everything else until then */
        return true;
    }

    /* if we get here, negotiation should be complete */
    if (c->state != 5) {
        pa_log_error("HFP negotiation failed in state %d with inbound %s\n",
                     c->state, buf);
        rfcomm_write_response(fd, "ERROR");
        return false;
    }

    /*
     * once we're fully connected, just reply OK to everything
     * it will just be the headset sending the occasional status
     * update, but we process only the ones we care about
     */
    return true;
}

static void rfcomm_io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_bluetooth_transport *t = userdata;

    pa_assert(io);
    pa_assert(t);

    if (events & (PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR)) {
        pa_log_info("Lost RFCOMM connection.");
        goto fail;
    }

    if (events & PA_IO_EVENT_INPUT) {
        char buf[512];
        ssize_t len;
        int gain, dummy;
        bool  do_reply = false;

        len = pa_read(fd, buf, 511, NULL);
        if (len < 0) {
            pa_log_error("RFCOMM read error: %s", pa_cstrerror(errno));
            goto fail;
        }
        buf[len] = 0;
        pa_log_debug("RFCOMM << %s", buf);

        /* There are only four HSP AT commands:
         * AT+VGS=value: value between 0 and 15, sent by the HS to AG to set the speaker gain.
         * +VGS=value is sent by AG to HS as a response to an AT+VGS command or when the gain
         * is changed on the AG side.
         * AT+VGM=value: value between 0 and 15, sent by the HS to AG to set the microphone gain.
         * +VGM=value is sent by AG to HS as a response to an AT+VGM command or when the gain
         * is changed on the AG side.
         * AT+CKPD=200: Sent by HS when headset button is pressed.
         * RING: Sent by AG to HS to notify of an incoming call. It can safely be ignored because
         * it does not expect a reply. */
        if (sscanf(buf, "AT+VGS=%d", &gain) == 1 || sscanf(buf, "\r\n+VGM%*[=:]%d\r\n", &gain) == 1) {
            if (!t->set_sink_volume) {
                pa_log_debug("HS/HF peer supports speaker gain control");
                t->set_sink_volume = set_sink_volume;
            }

            t->sink_volume = hsp_gain_to_volume(gain);
            pa_hook_fire(pa_bluetooth_discovery_hook(t->device->discovery, PA_BLUETOOTH_HOOK_TRANSPORT_SINK_VOLUME_CHANGED), t);
            do_reply = true;

        } else if (sscanf(buf, "AT+VGM=%d", &gain) == 1 || sscanf(buf, "\r\n+VGS%*[=:]%d\r\n", &gain) == 1) {
            if (!t->set_source_volume) {
                pa_log_debug("HS/HF peer supports microphone gain control");
                t->set_source_volume = set_source_volume;
            }

            t->source_volume = hsp_gain_to_volume(gain);
            pa_hook_fire(pa_bluetooth_discovery_hook(t->device->discovery, PA_BLUETOOTH_HOOK_TRANSPORT_SOURCE_VOLUME_CHANGED), t);
            do_reply = true;
        } else if (sscanf(buf, "AT+CKPD=%d", &dummy) == 1) {
            do_reply = true;
        } else if (t->config) { /* t->config is only non-null for hfp profile */
            do_reply = hfp_rfcomm_handle(fd, t, buf);
        } else {
            do_reply = false;
        }

        if (do_reply)
            rfcomm_write_response(fd, "OK");
    }

    return;

fail:
    pa_bluetooth_transport_unlink(t);
    pa_bluetooth_transport_free(t);
}

static void transport_destroy(pa_bluetooth_transport *t) {
    struct transport_data *trd = t->userdata;

    if (trd->sco_io) {
        trd->mainloop->io_free(trd->sco_io);
        shutdown(trd->sco_fd, SHUT_RDWR);
        close (trd->sco_fd);
    }

    trd->mainloop->io_free(trd->rfcomm_io);
    shutdown(trd->rfcomm_fd, SHUT_RDWR);
    close (trd->rfcomm_fd);

    pa_xfree(trd);
}

static pa_volume_t set_sink_volume(pa_bluetooth_transport *t, pa_volume_t volume) {
    struct transport_data *trd = t->userdata;
    uint16_t gain = volume_to_hsp_gain(volume);

    /* Propagate rounding and bound checks */
    volume = hsp_gain_to_volume(gain);

    if (t->sink_volume == volume)
        return volume;

    t->sink_volume = volume;

    /* If we are in the AG role, we send an unsolicited result-code to the headset
     * to change the speaker gain. In the HS role, source and sink are swapped,
     * so in this case we notify the AG that the microphone gain has changed
     * by sending a command. */
    if (is_pulseaudio_audio_gateway(t->profile)) {
        rfcomm_write_response(trd->rfcomm_fd, "+VGS=%d", gain);
    } else {
        rfcomm_write_command(trd->rfcomm_fd, "AT+VGM=%d", gain);
    }

    return volume;
}

static pa_volume_t set_source_volume(pa_bluetooth_transport *t, pa_volume_t volume) {
    struct transport_data *trd = t->userdata;
    uint16_t gain = volume_to_hsp_gain(volume);

    /* Propagate rounding and bound checks */
    volume = hsp_gain_to_volume(gain);

    if (t->source_volume == volume)
        return volume;

    t->source_volume = volume;

    /* If we are in the AG role, we send an unsolicited result-code to the headset
     * to change the microphone gain. In the HS role, source and sink are swapped,
     * so in this case we notify the AG that the speaker gain has changed
     * by sending a command. */
    if (is_pulseaudio_audio_gateway(t->profile)) {
        rfcomm_write_response(trd->rfcomm_fd, "+VGM=%d", gain);
    } else {
        rfcomm_write_command(trd->rfcomm_fd, "AT+VGS=%d", gain);
    }

    return volume;
}

static DBusMessage *profile_new_connection(DBusConnection *conn, DBusMessage *m, void *userdata) {
    pa_bluetooth_backend *b = userdata;
    pa_bluetooth_device *d;
    pa_bluetooth_transport *t;
    pa_bluetooth_profile_t p;
    DBusMessage *r;
    int fd;
    const char *sender, *path, PA_UNUSED *handler;
    DBusMessageIter arg_i;
    char *pathfd;
    struct transport_data *trd;

    if (!dbus_message_iter_init(m, &arg_i) || !pa_streq(dbus_message_get_signature(m), "oha{sv}")) {
        pa_log_error("Invalid signature found in NewConnection");
        goto fail;
    }

    handler = dbus_message_get_path(m);
    if (pa_streq(handler, HSP_AG_PROFILE)) {
        p = PA_BLUETOOTH_PROFILE_HSP_HS;
    } else if (pa_streq(handler, HSP_HS_PROFILE)) {
        p = PA_BLUETOOTH_PROFILE_HSP_AG;
    } else if (pa_streq(handler, HFP_AG_PROFILE)) {
        p = PA_BLUETOOTH_PROFILE_HFP_HF;
    } else {
        pa_log_error("Invalid handler");
        goto fail;
    }

    pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_OBJECT_PATH);
    dbus_message_iter_get_basic(&arg_i, &path);

    d = pa_bluetooth_discovery_get_device_by_path(b->discovery, path);
    if (d == NULL) {
        pa_log_error("Device doesn't exist for %s", path);
        goto fail;
    }

    if (d->enable_hfp_hf) {
        if (p == PA_BLUETOOTH_PROFILE_HSP_HS && pa_hashmap_get(d->uuids, PA_BLUETOOTH_UUID_HFP_HF)) {
            /* If peer connecting to HSP Audio Gateway supports HFP HF profile
             * reject this connection to force it to connect to HSP Audio Gateway instead.
             */
            pa_log_info("HFP HF enabled in native backend and is supported by peer, rejecting HSP HS peer connection");
            goto fail;
        }
    }

    pa_assert_se(dbus_message_iter_next(&arg_i));

    pa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_UNIX_FD);
    dbus_message_iter_get_basic(&arg_i, &fd);

    pa_log_debug("dbus: NewConnection path=%s, fd=%d, profile %s", path, fd,
        pa_bluetooth_profile_to_string(p));

    sender = dbus_message_get_sender(m);

    pathfd = pa_sprintf_malloc ("%s/fd%d", path, fd);
    t = pa_bluetooth_transport_new(d, sender, pathfd, p, NULL,
                                   p == PA_BLUETOOTH_PROFILE_HFP_HF ?
                                   sizeof(struct hfp_config) : 0);
    pa_xfree(pathfd);

    t->acquire = sco_acquire_cb;
    t->release = sco_release_cb;
    t->destroy = transport_destroy;

    /* If PA is the HF/HS we are in control of volume attenuation and
     * can always send volume commands (notifications) to keep the peer
     * updated on actual volume value.
     *
     * If the peer is the HF/HS it is responsible for attenuation of both
     * speaker and microphone gain.
     * On HFP speaker/microphone gain support is reported by bit 4 in the
     * `AT+BRSF=` command. Since it isn't explicitly documented whether this
     * applies to speaker or microphone gain but the peer is required to send
     * an initial value with `AT+VG[MS]=` either callback is hooked
     * independently as soon as this command is received.
     * On HSP this is not specified and is assumed to be dynamic for both
     * speaker and microphone.
     */
    if (is_peer_audio_gateway(p)) {
        t->set_sink_volume = set_sink_volume;
        t->set_source_volume = set_source_volume;
    }

    pa_bluetooth_transport_reconfigure(t, pa_bluetooth_get_hf_codec("CVSD"), sco_transport_write, NULL);

    trd = pa_xnew0(struct transport_data, 1);
    trd->rfcomm_fd = fd;
    trd->mainloop = b->core->mainloop;
    trd->rfcomm_io = trd->mainloop->io_new(b->core->mainloop, fd, PA_IO_EVENT_INPUT,
        rfcomm_io_callback, t);
    t->userdata =  trd;

    sco_listen(t);

    if (p != PA_BLUETOOTH_PROFILE_HFP_HF)
        transport_put(t);

    pa_assert_se(r = dbus_message_new_method_return(m));

    return r;

fail:
    pa_assert_se(r = dbus_message_new_error(m, BLUEZ_ERROR_INVALID_ARGUMENTS, "Unable to handle new connection"));
    return r;
}

static DBusMessage *profile_request_disconnection(DBusConnection *conn, DBusMessage *m, void *userdata) {
    DBusMessage *r;

    pa_assert_se(r = dbus_message_new_method_return(m));

    return r;
}

static DBusHandlerResult profile_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    pa_bluetooth_backend *b = userdata;
    DBusMessage *r = NULL;
    const char *path, *interface, *member;

    pa_assert(b);

    path = dbus_message_get_path(m);
    interface = dbus_message_get_interface(m);
    member = dbus_message_get_member(m);

    pa_log_debug("dbus: path=%s, interface=%s, member=%s", path, interface, member);

    if (!pa_streq(path, HSP_AG_PROFILE) && !pa_streq(path, HSP_HS_PROFILE)
        && !pa_streq(path, HFP_AG_PROFILE))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
        const char *xml = PROFILE_INTROSPECT_XML;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID));

    } else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "Release")) {
        pa_log_debug("Release not handled");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    } else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "RequestDisconnection")) {
        r = profile_request_disconnection(c, m, userdata);
    } else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "NewConnection"))
        r = profile_new_connection(c, m, userdata);
    else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (r) {
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(b->connection), r, NULL));
        dbus_message_unref(r);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static void profile_init(pa_bluetooth_backend *b, pa_bluetooth_profile_t profile) {
    static const DBusObjectPathVTable vtable_profile = {
        .message_function = profile_handler,
    };
    const char *object_name;
    const char *uuid;

    pa_assert(b);

    switch (profile) {
        case PA_BLUETOOTH_PROFILE_HSP_HS:
            object_name = HSP_AG_PROFILE;
            uuid = PA_BLUETOOTH_UUID_HSP_AG;
            break;
        case PA_BLUETOOTH_PROFILE_HSP_AG:
            object_name = HSP_HS_PROFILE;
            uuid = PA_BLUETOOTH_UUID_HSP_HS;
            break;
        case PA_BLUETOOTH_PROFILE_HFP_HF:
            object_name = HFP_AG_PROFILE;
            uuid = PA_BLUETOOTH_UUID_HFP_AG;
            break;
        default:
            pa_assert_not_reached();
            break;
    }

    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(b->connection), object_name, &vtable_profile, b));

    profile_status_set(b->discovery, profile, PA_BLUETOOTH_PROFILE_STATUS_ACTIVE);
    register_profile(b, object_name, uuid, profile);
}

static void profile_done(pa_bluetooth_backend *b, pa_bluetooth_profile_t profile) {
    pa_assert(b);

    profile_status_set(b->discovery, profile, PA_BLUETOOTH_PROFILE_STATUS_INACTIVE);

    switch (profile) {
        case PA_BLUETOOTH_PROFILE_HSP_HS:
            dbus_connection_unregister_object_path(pa_dbus_connection_get(b->connection), HSP_AG_PROFILE);
            break;
        case PA_BLUETOOTH_PROFILE_HSP_AG:
            dbus_connection_unregister_object_path(pa_dbus_connection_get(b->connection), HSP_HS_PROFILE);
            break;
        case PA_BLUETOOTH_PROFILE_HFP_HF:
            dbus_connection_unregister_object_path(pa_dbus_connection_get(b->connection), HFP_AG_PROFILE);
            break;
        default:
            pa_assert_not_reached();
            break;
    }
}

static void native_backend_apply_profile_registration_change(pa_bluetooth_backend *native_backend, bool enable_shared_profiles) {
    if (enable_shared_profiles) {
        profile_init(native_backend, PA_BLUETOOTH_PROFILE_HSP_AG);
        if (native_backend->enable_hfp_hf)
            profile_init(native_backend, PA_BLUETOOTH_PROFILE_HFP_HF);
    } else {
        profile_done(native_backend, PA_BLUETOOTH_PROFILE_HSP_AG);
        if (native_backend->enable_hfp_hf)
            profile_done(native_backend, PA_BLUETOOTH_PROFILE_HFP_HF);
    }
}

void pa_bluetooth_native_backend_enable_shared_profiles(pa_bluetooth_backend *native_backend, bool enable) {

   if (enable == native_backend->enable_shared_profiles)
       return;

   native_backend_apply_profile_registration_change(native_backend, enable);

   native_backend->enable_shared_profiles = enable;
}

pa_bluetooth_backend *pa_bluetooth_native_backend_new(pa_core *c, pa_bluetooth_discovery *y, bool enable_shared_profiles) {
    pa_bluetooth_backend *backend;
    DBusError err;

    pa_log_debug("Bluetooth Headset Backend API support using the native backend");

    backend = pa_xnew0(pa_bluetooth_backend, 1);
    backend->core = c;

    dbus_error_init(&err);
    if (!(backend->connection = pa_dbus_bus_get(c, DBUS_BUS_SYSTEM, &err))) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        dbus_error_free(&err);
        pa_xfree(backend);
        return NULL;
    }

    backend->discovery = y;
    backend->enable_shared_profiles = enable_shared_profiles;
    backend->enable_hfp_hf = pa_bluetooth_discovery_get_enable_native_hfp_hf(y);
    backend->enable_hsp_hs = pa_bluetooth_discovery_get_enable_native_hsp_hs(y);

    if (!backend->enable_hsp_hs && !backend->enable_hfp_hf)
        pa_log_warn("Both HSP HS and HFP HF bluetooth profiles disabled in native backend. Native backend will not register for headset connections.");

    if (backend->enable_hsp_hs)
        profile_init(backend, PA_BLUETOOTH_PROFILE_HSP_HS);

    if (backend->enable_shared_profiles)
        native_backend_apply_profile_registration_change(backend, true);

    return backend;
}

void pa_bluetooth_native_backend_free(pa_bluetooth_backend *backend) {
    pa_assert(backend);

    pa_dbus_free_pending_list(&backend->pending);

    if (backend->enable_shared_profiles)
        native_backend_apply_profile_registration_change(backend, false);

    if (backend->enable_hsp_hs)
        profile_done(backend, PA_BLUETOOTH_PROFILE_HSP_HS);

    pa_dbus_connection_unref(backend->connection);

    pa_xfree(backend);
}
