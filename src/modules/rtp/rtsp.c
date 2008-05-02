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

/*
 * read one line from the file descriptor
 * timeout: msec unit, -1 for infinite
 * if CR comes then following LF is expected
 * returned string in line is always null terminated, maxlen-1 is maximum string length
 */
static int pa_read_line(int fd, char *line, int maxlen, int timeout)
{
    int i, rval;
    int count;
    char ch;
    struct pollfd pfds;
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
                        pa_headerlist* headers, pa_headerlist** response_headers) {
    pa_strbuf* buf;
    char* hdrs;
    ssize_t l;
    char response[1024];
    int timeout;
    char* token;
    const char* token_state;
    char delimiters[2];
    char* header;
    char* delimpos;


    pa_assert(c);
    pa_assert(c->url);

    if (!cmd)
        return 0;

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
    l = pa_write(c->fd, hdrs, strlen(hdrs), NULL);
    pa_xfree(hdrs);

    /* Do we expect a response? */
    if (!expect_response)
        return 1;

    timeout = 5000;
    if (pa_read_line(c->fd, response, sizeof(response), timeout) <= 0) {
        /*ERRMSG("%s: request failed\n",__func__);*/
        return 0;
    }

    delimiters[0] = ' ';
    delimiters[1] = '\0';
    token_state = NULL;
    pa_xfree(pa_split(response, delimiters, &token_state));
    token = pa_split(response, delimiters, &token_state);
    if (!token || strcmp(token, "200")) {
        pa_xfree(token);
        /*ERRMSG("%s: request failed, error %s\n",__func__,token);*/
        return 0;
    }
    pa_xfree(token);

    /* We want to return the headers? */
    if (!response_headers)
    {
        /* We have no storage, so just clear out the response. */
        while (pa_read_line(c->fd, response, sizeof(response), timeout) > 0) {
            /* Reduce timeout for future requests */
            timeout = 1000;
        }
        return 1;
    }

    /* TODO: Move header reading into the headerlist. */
    header = NULL;
    buf = pa_strbuf_new();
    while (pa_read_line(c->fd, response, sizeof(response), timeout) > 0) {
        /* Reduce timeout for future requests */
        timeout = 1000;

        /* If the first character is a space, it's a continuation header */
        if (header && ' ' == response[0]) {
            /* Add this line to the buffer (sans the space. */
            pa_strbuf_puts(buf, &(response[1]));
            continue;
        }

        if (header) {
            /* This is not a continuation header so let's dump the full
               header/value into our proplist */
            pa_headerlist_puts(*response_headers, header, pa_strbuf_tostring_free(buf));
            pa_xfree(header);
            buf = pa_strbuf_new();
        }

        delimpos = strstr(response, ":");
        if (!delimpos) {
            /*ERRMSG("%s: Request failed, bad header\n",__func__);*/
            return 0;
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
        pa_headerlist_puts(*response_headers, header, pa_strbuf_tostring(buf));
    }
    pa_strbuf_free(buf);

    return 1;
}


pa_rtsp_context* pa_rtsp_context_new(const char* useragent) {
    pa_rtsp_context *c;

    c = pa_xnew0(pa_rtsp_context, 1);
    c->fd = -1;
    c->headers = pa_headerlist_new();

    if (useragent)
        c->useragent = useragent;
    else
        c->useragent = "PulseAudio RTSP Client";

    return c;
}


void pa_rtsp_context_free(pa_rtsp_context* c) {
    if (c) {
        pa_xfree(c->url);
        pa_xfree(c->session);
        pa_xfree(c->transport);
        pa_headerlist_free(c->headers);
    }
    pa_xfree(c);
}


int pa_rtsp_connect(pa_rtsp_context *c, const char* hostname, uint16_t port, const char* sid) {
    struct sockaddr_in sa;
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    struct hostent *host = NULL;
    int r;

    pa_assert(c);
    pa_assert(hostname);
    pa_assert(port > 0);
    pa_assert(sid);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    host = gethostbyname(hostname);
    if (!host) {
        unsigned int addr = inet_addr(hostname);
        if (addr != INADDR_NONE)
            host = gethostbyaddr((char*)&addr, 4, AF_INET);
        if (!host)
            return 0;
    }
    memcpy(&sa.sin_addr, host->h_addr, sizeof(struct in_addr));

    if ((c->fd = socket(sa.sin_family, SOCK_STREAM, 0)) < 0) {
        pa_log("socket(): %s", pa_cstrerror(errno));
        return 0;
    }

    /* Q: is FD_CLOEXEC reqd? */
    pa_make_fd_cloexec(c->fd);
    pa_make_tcp_socket_low_delay(c->fd);

    if ((r = connect(c->fd, &sa, sizeof(struct sockaddr_in))) < 0) {
#ifdef OS_IS_WIN32
        if (WSAGetLastError() != EWOULDBLOCK) {
            pa_log_debug("connect(): %d", WSAGetLastError());
#else
        if (errno != EINPROGRESS) {
            pa_log_debug("connect(): %s (%d)", pa_cstrerror(errno), errno);
#endif
            pa_close(c->fd);
            c->fd = -1;
            return 0;
        }
    }

    if (0 != getsockname(c->fd, (struct sockaddr*)&name, &namelen)) {
        pa_close(c->fd);
        c->fd = -1;
        return 0;
    }
    memcpy(&c->local_addr, &name.sin_addr, sizeof(struct in_addr));
    c->url = pa_sprintf_malloc("rtsp://%s/%s", inet_ntoa(name.sin_addr), sid);

    return 1;
}


void pa_rtsp_disconnect(pa_rtsp_context *c) {
    pa_assert(c);

    if (c->fd < 0)
      return;
    pa_close(c->fd);
    c->fd = -1;
}


const char* pa_rtsp_localip(pa_rtsp_context* c) {
    pa_assert(c);

    if (c->fd < 0)
        return NULL;
    return inet_ntoa(c->local_addr);
}


int pa_rtsp_announce(pa_rtsp_context *c, const char* sdp) {
    pa_assert(c);
    if (!sdp)
        return 0;

    return pa_rtsp_exec(c, "ANNOUNCE", "application/sdp", sdp, 1, NULL, NULL);
}


int pa_rtsp_setup(pa_rtsp_context* c, pa_headerlist** response_headers) {
    pa_headerlist* headers;
    pa_headerlist* rheaders;
    char delimiters[2];
    char* token;
    const char* token_state;
    const char* pc;

    pa_assert(c);

    headers = pa_headerlist_new();
    rheaders = pa_headerlist_new();
    pa_headerlist_puts(headers, "Transport", "RTP/AVP/TCP;unicast;interleaved=0-1;mode=record");

    if (!pa_rtsp_exec(c, "SETUP", NULL, NULL, 1, headers, &rheaders)) {
        pa_headerlist_free(headers);
        pa_headerlist_free(rheaders);
        return 0;
    }
    pa_headerlist_free(headers);

    c->session = pa_xstrdup(pa_headerlist_gets(rheaders, "Session"));
    c->transport = pa_xstrdup(pa_headerlist_gets(rheaders, "Transport"));

    if (!c->session || !c->transport) {
        pa_headerlist_free(rheaders);
        return 0;
    }

    /* Now parse out the server port component of the response. */
    c->port = 0;
    delimiters[0] = ';';
    delimiters[1] = '\0';
    token_state = NULL;
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
        pa_headerlist_free(rheaders);
        return 0;
    }

    *response_headers = rheaders;
    return 1;
}


int pa_rtsp_record(pa_rtsp_context* c) {
    pa_headerlist* headers;
    int rv;

    pa_assert(c);
    if (!c->session) {
        /* No seesion in progres */
        return 0;
    }

    headers = pa_headerlist_new();
    pa_headerlist_puts(headers, "Range", "npt=0-");
    pa_headerlist_puts(headers, "RTP-Info", "seq=0;rtptime=0");

    rv = pa_rtsp_exec(c, "RECORD", NULL, NULL, 1, headers, NULL);
    pa_headerlist_free(headers);
    return rv;
}


int pa_rtsp_teardown(pa_rtsp_context *c) {
    pa_assert(c);

    return pa_rtsp_exec(c, "TEARDOWN", NULL, NULL, 0, NULL, NULL);
}


int pa_rtsp_setparameter(pa_rtsp_context *c, const char* param) {
    pa_assert(c);
    if (!param)
        return 0;

    return pa_rtsp_exec(c, "SET_PARAMETER", "text/parameters", param, 1, NULL, NULL);
}


int pa_rtsp_flush(pa_rtsp_context *c) {
    pa_headerlist* headers;
    int rv;

    pa_assert(c);

    headers = pa_headerlist_new();
    pa_headerlist_puts(headers, "RTP-Info", "seq=0;rtptime=0");

    rv = pa_rtsp_exec(c, "FLUSH", NULL, NULL, 1, headers, NULL);
    pa_headerlist_free(headers);
    return rv;
}
