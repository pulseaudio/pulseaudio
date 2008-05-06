/* $Id$ */

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/poll.h>

#include "rtsp.h"

struct pa_rtsp_context {
    pa_socket_client *sc;
    pa_iochannel *io;
    pa_rtsp_cb_t callback;
    void* userdata;
    const char* useragent;
    pa_headerlist* headers;
    char* localip;
    char* url;
    uint32_t port;
    uint32_t cseq;
    char* session;
    char* transport;
    pa_rtsp_state state;
};

/*
 * read one line from the file descriptor
 * timeout: msec unit, -1 for infinite
 * if CR comes then following LF is expected
 * returned string in line is always null terminated, maxlen-1 is maximum string length
 */
static int pa_read_line(pa_iochannel* io, char *line, int maxlen, int timeout)
{
    int i, rval;
    int count;
    int fd;
    char ch;
    struct pollfd pfds;

    pa_assert(io);
    fd = pa_iochannel_get_recv_fd(io);

    count = 0;
    *line = 0;
    pfds.events = POLLIN;
    pfds.fd = fd;

    for (i=0; i<maxlen; ++i) {
        if (!poll(&pfds, 1, timeout))
            return 0;

        rval = read(fd, &ch, 1);

        if (-1 == rval) {
            if (EAGAIN == errno)
                return 0;
            /*ERRMSG("%s:read error: %s\n", __func__, strerror(errno));*/
            return -1;
        }

        if (0 == rval) {
            /*INFMSG("%s:disconnected on the other end\n", __func__);*/
            return -1;
        }

        if ('\n' == ch) {
            *line = 0;
            return count;
        }

        if ('\r' == ch)
            continue;

        *line++ = ch;
        count++;

        if (count >= maxlen-1)
            break;
    }

    *line = 0;
    return count;
}


static int pa_rtsp_exec(pa_rtsp_context* c, const char* cmd,
                        const char* content_type, const char* content,
                        int expect_response,
                        pa_headerlist* headers) {
    pa_strbuf* buf;
    char* hdrs;
    ssize_t l;

    pa_assert(c);
    pa_assert(c->url);

    if (!cmd)
        return -1;

    buf = pa_strbuf_new();
    pa_strbuf_printf(buf, "%s %s RTSP/1.0\r\nCSeq: %d\r\n", cmd, c->url, ++c->cseq);
    if (c->session)
        pa_strbuf_printf(buf, "Session: %s\r\n", c->session);

    /* Add the headers */
    if (headers) {
        hdrs = pa_headerlist_to_string(headers);
        pa_strbuf_puts(buf, hdrs);
        pa_xfree(hdrs);
    }

    if (content_type && content) {
        pa_strbuf_printf(buf, "Content-Type: %s\r\nContent-Length: %d\r\n",
          content_type, (int)strlen(content));
    }

    pa_strbuf_printf(buf, "User-Agent: %s\r\n", c->useragent);

    if (c->headers) {
        hdrs = pa_headerlist_to_string(c->headers);
        pa_strbuf_puts(buf, hdrs);
        pa_xfree(hdrs);
    }

    pa_strbuf_puts(buf, "\r\n");

    if (content_type && content) {
        pa_strbuf_puts(buf, content);
    }

    /* Our packet is created... now we can send it :) */
    hdrs = pa_strbuf_tostring_free(buf);
    l = pa_iochannel_write(c->io, hdrs, strlen(hdrs));
    pa_xfree(hdrs);

    return 0;
}


pa_rtsp_context* pa_rtsp_context_new(const char* useragent) {
    pa_rtsp_context *c;

    c = pa_xnew0(pa_rtsp_context, 1);
    c->headers = pa_headerlist_new();

    if (useragent)
        c->useragent = useragent;
    else
        c->useragent = "PulseAudio RTSP Client";

    return c;
}


void pa_rtsp_context_free(pa_rtsp_context* c) {
    if (c) {
        if (c->sc)
            pa_socket_client_unref(c->sc);

        pa_xfree(c->url);
        pa_xfree(c->localip);
        pa_xfree(c->session);
        pa_xfree(c->transport);
        pa_headerlist_free(c->headers);
    }
    pa_xfree(c);
}


static void io_callback(PA_GCC_UNUSED pa_iochannel *io, void *userdata) {
    pa_strbuf* buf;
    pa_headerlist* response_headers = NULL;
    char response[1024];
    int timeout;
    char* token;
    char* header;
    char* delimpos;
    char delimiters[] = " ";
    pa_rtsp_context *c = userdata;
    pa_assert(c);

    /* TODO: convert this to a pa_ioline based reader */
    if (STATE_CONNECT == c->state) {
        response_headers = pa_headerlist_new();
    }
    timeout = 5000;
    /* read in any response headers */
    if (pa_read_line(c->io, response, sizeof(response), timeout) > 0) {
        const char* token_state = NULL;

        timeout = 1000;
        pa_xfree(pa_split(response, delimiters, &token_state));
        token = pa_split(response, delimiters, &token_state);
        if (!token || strcmp(token, "200")) {
            pa_xfree(token);
            pa_log("Invalid Response");
            /* TODO: Bail out completely */
            return;
        }
        pa_xfree(token);

        /* We want to return the headers? */
        if (!response_headers) {
            /* We have no storage, so just clear out the response. */
            while (pa_read_line(c->io, response, sizeof(response), timeout) > 0);
        } else {
            /* TODO: Move header reading into the headerlist. */
            header = NULL;
            buf = pa_strbuf_new();
            while (pa_read_line(c->io, response, sizeof(response), timeout) > 0) {
                /* If the first character is a space, it's a continuation header */
                if (header && ' ' == response[0]) {
                    /* Add this line to the buffer (sans the space. */
                    pa_strbuf_puts(buf, &(response[1]));
                    continue;
                }

                if (header) {
                    /* This is not a continuation header so let's dump the full
                      header/value into our proplist */
                    pa_headerlist_puts(response_headers, header, pa_strbuf_tostring_free(buf));
                    pa_xfree(header);
                    buf = pa_strbuf_new();
                }

                delimpos = strstr(response, ":");
                if (!delimpos) {
                    pa_log("Invalid response header");
                    return;
                }

                if (strlen(delimpos) > 1) {
                    /* Cut our line off so we can copy the header name out */
                    *delimpos++ = '\0';

                    /* Trim the front of any spaces */
                    while (' ' == *delimpos)
                        ++delimpos;

                    pa_strbuf_puts(buf, delimpos);
                } else {
                    /* Cut our line off so we can copy the header name out */
                    *delimpos = '\0';
                }

                /* Save the header name */
                header = pa_xstrdup(response);
            }
            /* We will have a header left from our looping itteration, so add it in :) */
            if (header) {
                /* This is not a continuation header so let's dump it into our proplist */
                pa_headerlist_puts(response_headers, header, pa_strbuf_tostring(buf));
            }
            pa_strbuf_free(buf);
        }
    }

    /* Deal with a CONNECT response */
    if (STATE_CONNECT == c->state) {
        const char* token_state = NULL;
        const char* pc = NULL;
        c->session = pa_xstrdup(pa_headerlist_gets(response_headers, "Session"));
        c->transport = pa_xstrdup(pa_headerlist_gets(response_headers, "Transport"));

        if (!c->session || !c->transport) {
            pa_headerlist_free(response_headers);
            return;
        }

        /* Now parse out the server port component of the response. */
        c->port = 0;
        delimiters[0] = ';';
        while ((token = pa_split(c->transport, delimiters, &token_state))) {
            if ((pc = strstr(token, "="))) {
                if (0 == strncmp(token, "server_port", 11)) {
                    pa_atou(pc+1, &c->port);
                    pa_xfree(token);
                    break;
                }
            }
            pa_xfree(token);
        }
        if (0 == c->port) {
            /* Error no server_port in response */
            pa_headerlist_free(response_headers);
            return;
        }
    }

    /* Call our callback */
    if (c->callback)
        c->callback(c, c->state, response_headers, c->userdata);


    if (response_headers)
        pa_headerlist_free(response_headers);

    /*
    if (do_read(u) < 0 || do_write(u) < 0) {

        if (u->io) {
            pa_iochannel_free(u->io);
            u->io = NULL;
        }

       pa_module_unload_request(u->module);
    }
    */
}

static void on_connection(pa_socket_client *sc, pa_iochannel *io, void *userdata) {
    pa_rtsp_context *c = userdata;
    union {
        struct sockaddr sa;
        struct sockaddr_in in;
        struct sockaddr_in6 in6;
    } sa;
    socklen_t sa_len = sizeof(sa);

    pa_assert(sc);
    pa_assert(c);
    pa_assert(c->sc == sc);

    pa_socket_client_unref(c->sc);
    c->sc = NULL;

    if (!io) {
        pa_log("Connection failed: %s", pa_cstrerror(errno));
        return;
    }
    pa_assert(!c->io);
    c->io = io;
    pa_iochannel_set_callback(c->io, io_callback, c);

    /* Get the local IP address for use externally */
    if (0 == getsockname(pa_iochannel_get_recv_fd(io), &sa.sa, &sa_len)) {
        char buf[INET6_ADDRSTRLEN];
        const char *res = NULL;

        if (AF_INET == sa.sa.sa_family) {
            res = inet_ntop(sa.sa.sa_family, &sa.in.sin_addr, buf, sizeof(buf));
        } else if (AF_INET6 == sa.sa.sa_family) {
            res = inet_ntop(AF_INET6, &sa.in6.sin6_addr, buf, sizeof(buf));
        }
        if (res)
            c->localip = pa_xstrdup(res);
    }
}

int pa_rtsp_connect(pa_rtsp_context *c, pa_mainloop_api *mainloop, const char* hostname, uint16_t port) {
    pa_assert(c);
    pa_assert(mainloop);
    pa_assert(hostname);
    pa_assert(port > 0);

    if (!(c->sc = pa_socket_client_new_string(mainloop, hostname, port))) {
        pa_log("failed to connect to server '%s:%d'", hostname, port);
        return -1;
    }

    pa_socket_client_set_callback(c->sc, on_connection, c);
    c->state = STATE_CONNECT;
    return 0;
}

void pa_rtsp_set_callback(pa_rtsp_context *c, pa_rtsp_cb_t callback, void *userdata) {
    pa_assert(c);

    c->callback = callback;
    c->userdata = userdata;
}

void pa_rtsp_disconnect(pa_rtsp_context *c) {
    pa_assert(c);

    if (c->io)
        pa_iochannel_free(c->io);
    c->io = NULL;
}


const char* pa_rtsp_localip(pa_rtsp_context* c) {
    pa_assert(c);

    return c->localip;
}

uint32_t pa_rtsp_serverport(pa_rtsp_context* c) {
    pa_assert(c);

    return c->port;
}

void pa_rtsp_set_url(pa_rtsp_context* c, const char* url) {
    pa_assert(c);

    c->url = pa_xstrdup(url);
}

void pa_rtsp_add_header(pa_rtsp_context *c, const char* key, const char* value)
{
    pa_assert(c);
    pa_assert(key);
    pa_assert(value);

    pa_headerlist_puts(c->headers, key, value);
}

void pa_rtsp_remove_header(pa_rtsp_context *c, const char* key)
{
    pa_assert(c);
    pa_assert(key);

    pa_headerlist_remove(c->headers, key);
}

int pa_rtsp_announce(pa_rtsp_context *c, const char* sdp) {
    pa_assert(c);
    if (!sdp)
        return -1;

    c->state = STATE_ANNOUNCE;
    return pa_rtsp_exec(c, "ANNOUNCE", "application/sdp", sdp, 1, NULL);
}


int pa_rtsp_setup(pa_rtsp_context* c) {
    pa_headerlist* headers;
    int rv;

    pa_assert(c);

    headers = pa_headerlist_new();
    pa_headerlist_puts(headers, "Transport", "RTP/AVP/TCP;unicast;interleaved=0-1;mode=record");

    c->state = STATE_SETUP;
    rv = pa_rtsp_exec(c, "SETUP", NULL, NULL, 1, headers);
    pa_headerlist_free(headers);
    return rv;
}


int pa_rtsp_record(pa_rtsp_context* c) {
    pa_headerlist* headers;
    int rv;

    pa_assert(c);
    if (!c->session) {
        /* No seesion in progres */
        return -1;
    }

    headers = pa_headerlist_new();
    pa_headerlist_puts(headers, "Range", "npt=0-");
    pa_headerlist_puts(headers, "RTP-Info", "seq=0;rtptime=0");

    c->state = STATE_RECORD;
    rv = pa_rtsp_exec(c, "RECORD", NULL, NULL, 1, headers);
    pa_headerlist_free(headers);
    return rv;
}


int pa_rtsp_teardown(pa_rtsp_context *c) {
    pa_assert(c);

    c->state = STATE_TEARDOWN;
    return pa_rtsp_exec(c, "TEARDOWN", NULL, NULL, 0, NULL);
}


int pa_rtsp_setparameter(pa_rtsp_context *c, const char* param) {
    pa_assert(c);
    if (!param)
        return -1;

    c->state = STATE_SET_PARAMETER;
    return pa_rtsp_exec(c, "SET_PARAMETER", "text/parameters", param, 1, NULL);
}


int pa_rtsp_flush(pa_rtsp_context *c) {
    pa_headerlist* headers;
    int rv;

    pa_assert(c);

    headers = pa_headerlist_new();
    pa_headerlist_puts(headers, "RTP-Info", "seq=0;rtptime=0");

    c->state = STATE_FLUSH;
    rv = pa_rtsp_exec(c, "FLUSH", NULL, NULL, 1, headers);
    pa_headerlist_free(headers);
    return rv;
}
