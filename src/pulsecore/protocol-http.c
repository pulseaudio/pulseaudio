/***
  This file is part of PulseAudio.

  Copyright 2005-2006 Lennart Poettering

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
#include <string.h>

#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/ioline.h>
#include <pulsecore/macro.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/cli-text.h>
#include <pulsecore/shared.h>

#include "protocol-http.h"

/* Don't allow more than this many concurrent connections */
#define MAX_CONNECTIONS 10

#define URL_ROOT "/"
#define URL_CSS "/style"
#define URL_STATUS "/status"
#define URL_LISTEN "/listen"
#define URL_LISTEN_PREFIX "/listen/"

#define MIME_HTML "text/html; charset=utf-8"
#define MIME_TEXT "text/plain; charset=utf-8"
#define MIME_CSS "text/css"

#define HTML_HEADER(t)                                                  \
    "<?xml version=\"1.0\"?>\n"                                         \
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n" \
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"                   \
    "        <head>\n"                                                  \
    "                <title>"t"</title>\n"                              \
    "                <link rel=\"stylesheet\" type=\"text/css\" href=\"style\"/>\n" \
    "        </head>\n"                                                 \
    "        <body>\n"

#define HTML_FOOTER                                                     \
    "        </body>\n"                                                 \
    "</html>\n"
enum state {
    STATE_REQUEST_LINE,
    STATE_MIME_HEADER,
    STATE_DATA
};

struct connection {
    pa_http_protocol *protocol;
    pa_ioline *line;
    enum state state;
    char *url;
    pa_module *module;
};

struct pa_http_protocol {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_idxset *connections;
};


static pa_bool_t is_mime_sample_spec(const pa_sample_spec *ss, const pa_channel_map *cm) {

    pa_assert(pa_channel_map_compatible(cm, ss));

    switch (ss->format) {
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_U8:

            if (ss->rate != 8000 &&
                ss->rate != 11025 &&
                ss->rate != 16000 &&
                ss->rate != 22050 &&
                ss->rate != 24000 &&
                ss->rate != 32000 &&
                ss->rate != 44100 &&
                ss->rate != 48000)
                return FALSE;

            if (ss->channels != 1 &&
                ss->channels != 2)
                return FALSE;

            if ((cm->channels == 1 && cm->map[0] != PA_CHANNEL_POSITION_MONO) ||
                (cm->channels == 2 && (cm->map[0] != PA_CHANNEL_POSITION_LEFT || cm->map[1] != PA_CHANNEL_POSITION_RIGHT)))
                return FALSE;

            return TRUE;

        case PA_SAMPLE_ULAW:

            if (ss->rate != 8000)
                return FALSE;

            if (ss->channels != 1)
                return FALSE;

            if (cm->map[0] != PA_CHANNEL_POSITION_MONO)
                return FALSE;

            return TRUE;

        default:
            return FALSE;
    }
}

static void mimefy_sample_spec(pa_sample_spec *ss, pa_channel_map *cm) {

    pa_assert(pa_channel_map_compatible(cm, ss));

    /* Turns the sample type passed in into the next 'better' one that
     * can be encoded for HTTP. If there is no 'better' one we pick
     * the 'best' one that is 'worse'. */

    if (ss->channels > 2)
        ss->channels = 2;

    if (ss->rate > 44100)
        ss->rate = 48000;
    else if (ss->rate > 32000)
        ss->rate = 44100;
    else if (ss->rate > 24000)
        ss->rate = 32000;
    else if (ss->rate > 22050)
        ss->rate = 24000;
    else if (ss->rate > 16000)
        ss->rate = 22050;
    else if (ss->rate > 11025)
        ss->rate = 16000;
    else if (ss->rate > 8000)
        ss->rate = 11025;
    else
        ss->rate = 8000;

    switch (ss->format) {
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
            ss->format = PA_SAMPLE_S24BE;
            break;

        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S16LE:
            ss->format = PA_SAMPLE_S16BE;
            break;

        case PA_SAMPLE_ULAW:
        case PA_SAMPLE_ALAW:

            if (ss->rate == 8000 && ss->channels == 1)
                ss->format = PA_SAMPLE_ULAW;
            else
                ss->format = PA_SAMPLE_S16BE;
            break;

        case PA_SAMPLE_U8:
            ss->format = PA_SAMPLE_U8;
            break;

        case PA_SAMPLE_MAX:
        case PA_SAMPLE_INVALID:
            pa_assert_not_reached();
    }

    pa_channel_map_init_auto(cm, ss->channels, PA_CHANNEL_MAP_DEFAULT);

    pa_assert(is_mime_sample_spec(ss, cm));
}

static char *sample_spec_to_mime_type(const pa_sample_spec *ss, const pa_channel_map *cm) {
    pa_assert(pa_channel_map_compatible(cm, ss));

    if (!is_mime_sample_spec(ss, cm))
        return NULL;

    switch (ss->format) {

        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_U8:
            return pa_sprintf_malloc("audio/%s; rate=%u; channels=%u",
                                     ss->format == PA_SAMPLE_S16BE ? "L16" :
                                     (ss->format == PA_SAMPLE_S24BE ? "L24" : "L8"),
                                     ss->rate, ss->channels);

        case PA_SAMPLE_ULAW:
            return pa_xstrdup("audio/basic");

        default:
            pa_assert_not_reached();
    }

    pa_assert(pa_sample_spec_valid(ss));
}

static char *mimefy_and_stringify_sample_spec(const pa_sample_spec *_ss, const pa_channel_map *_cm) {
    pa_sample_spec ss = *_ss;
    pa_channel_map cm = *_cm;

    mimefy_sample_spec(&ss, &cm);

    return sample_spec_to_mime_type(&ss, &cm);
}

static char *escape_html(const char *t) {
    pa_strbuf *sb;
    const char *p, *e;

    sb = pa_strbuf_new();

    for (e = p = t; *p; p++) {

        if (*p == '>' || *p == '<' || *p == '&') {

            if (p > e) {
                pa_strbuf_putsn(sb, e, p-e);
                e = p + 1;
            }

            if (*p == '>')
                pa_strbuf_puts(sb, "&gt;");
            else if (*p == '<')
                pa_strbuf_puts(sb, "&lt;");
            else
                pa_strbuf_puts(sb, "&amp;");
        }
    }

    if (p > e)
        pa_strbuf_putsn(sb, e, p-e);

    return pa_strbuf_tostring_free(sb);
}

static void http_response(
        struct connection *c,
        int code,
        const char *msg,
        const char *mime) {

    char *s;

    pa_assert(c);
    pa_assert(msg);
    pa_assert(mime);

    s = pa_sprintf_malloc(
            "HTTP/1.0 %i %s\n"
            "Connection: close\n"
            "Content-Type: %s\n"
            "Cache-Control: no-cache\n"
            "Expires: 0\n"
            "Server: "PACKAGE_NAME"/"PACKAGE_VERSION"\n"
            "\n", code, msg, mime);
    pa_ioline_puts(c->line, s);
    pa_xfree(s);
}

static void html_response(
        struct connection *c,
        int code,
        const char *msg,
        const char *text) {

    char *s;
    pa_assert(c);

    http_response(c, code, msg, MIME_HTML);

    if (!text)
        text = msg;

    s = pa_sprintf_malloc(
            HTML_HEADER("%s")
            "%s"
            HTML_FOOTER,
            text, text);

    pa_ioline_puts(c->line, s);
    pa_xfree(s);

    pa_ioline_defer_close(c->line);
}

static void internal_server_error(struct connection *c) {
    pa_assert(c);

    html_response(c, 500, "Internal Server Error", NULL);
}

static void connection_unlink(struct connection *c) {
    pa_assert(c);

    if (c->url)
        pa_xfree(c->url);

    if (c->line)
        pa_ioline_unref(c->line);

    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);

    pa_xfree(c);
}

static void html_print_field(pa_ioline *line, const char *left, const char *right) {
    char *eleft, *eright;

    eleft = escape_html(left);
    eright = escape_html(right);

    pa_ioline_printf(line,
                     "<tr><td><b>%s</b></td>"
                     "<td>%s</td></tr>\n", eleft, eright);

    pa_xfree(eleft);
    pa_xfree(eright);
}

static void handle_url(struct connection *c) {
    pa_assert(c);

    pa_log_debug("Request for %s", c->url);

    if (pa_streq(c->url, URL_ROOT)) {
        char *t;

        http_response(c, 200, "OK", MIME_HTML);

        pa_ioline_puts(c->line,
                       HTML_HEADER(PACKAGE_NAME" "PACKAGE_VERSION)
                       "<h1>"PACKAGE_NAME" "PACKAGE_VERSION"</h1>\n"
                       "<table>\n");

        t = pa_get_user_name_malloc();
        html_print_field(c->line, "User Name:", t);
        pa_xfree(t);

        t = pa_get_host_name_malloc();
        html_print_field(c->line, "Host name:", t);
        pa_xfree(t);

        t = pa_machine_id();
        html_print_field(c->line, "Machine ID:", t);
        pa_xfree(t);

        t = pa_uname_string();
        html_print_field(c->line, "System:", t);
        pa_xfree(t);

        t = pa_sprintf_malloc("%lu", (unsigned long) getpid());
        html_print_field(c->line, "Process ID:", t);
        pa_xfree(t);

        pa_ioline_puts(c->line,
                       "</table>\n"
                       "<p><a href=\"/status\">Show an extensive server status report</a></p>\n"
                       "<p><a href=\"/listen\">Monitor sinks and sources</a></p>\n"
                       HTML_FOOTER);

        pa_ioline_defer_close(c->line);

    } else if (pa_streq(c->url, URL_CSS)) {
        http_response(c, 200, "OK", MIME_CSS);

        pa_ioline_puts(c->line,
                       "body { color: black; background-color: white; }\n"
                       "a:link, a:visited { color: #900000; }\n"
                       "div.news-date { font-size: 80%; font-style: italic; }\n"
                       "pre { background-color: #f0f0f0; padding: 0.4cm; }\n"
                       ".grey { color: #8f8f8f; font-size: 80%; }"
                       "table {  margin-left: 1cm; border:1px solid lightgrey; padding: 0.2cm; }\n"
                       "td { padding-left:10px; padding-right:10px; }\n");

        pa_ioline_defer_close(c->line);

    } else if (pa_streq(c->url, URL_STATUS)) {
        char *r;

        http_response(c, 200, "OK", MIME_TEXT);
        r = pa_full_status_string(c->protocol->core);
        pa_ioline_puts(c->line, r);
        pa_xfree(r);

        pa_ioline_defer_close(c->line);

    } else if (pa_streq(c->url, URL_LISTEN)) {
        pa_source *source;
        pa_sink *sink;
        uint32_t idx;

        http_response(c, 200, "OK", MIME_HTML);

        pa_ioline_puts(c->line,
                       HTML_HEADER("Listen")
                       "<h2>Sinks</h2>\n"
                       "<p>\n");

        PA_IDXSET_FOREACH(sink, c->protocol->core->sinks, idx) {
            char *t, *m;

            t = escape_html(pa_strna(pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_DESCRIPTION)));
            m = mimefy_and_stringify_sample_spec(&sink->sample_spec, &sink->channel_map);

            pa_ioline_printf(c->line,
                             "<a href=\"/listen/%s\" title=\"%s\">%s</a><br/>\n",
                             sink->monitor_source->name, m, t);

            pa_xfree(t);
            pa_xfree(m);
        }

        pa_ioline_puts(c->line,
                       "</p>\n"
                       "<h2>Sources</h2>\n"
                       "<p>\n");

        PA_IDXSET_FOREACH(source, c->protocol->core->sources, idx) {
            char *t, *m;

            if (source->monitor_of)
                continue;

            t = escape_html(pa_strna(pa_proplist_gets(source->proplist, PA_PROP_DEVICE_DESCRIPTION)));
            m = mimefy_and_stringify_sample_spec(&source->sample_spec, &source->channel_map);

            pa_ioline_printf(c->line,
                             "<a href=\"/listen/%s\" title=\"%s\">%s</a><br/>\n",
                             source->name, m, t);

            pa_xfree(m);
            pa_xfree(t);

        }

        pa_ioline_puts(c->line,
                       "</p>\n"
                       HTML_FOOTER);

        pa_ioline_defer_close(c->line);
    } else
        html_response(c, 404, "Not Found", NULL);
}

static void line_callback(pa_ioline *line, const char *s, void *userdata) {
    struct connection *c = userdata;
    pa_assert(line);
    pa_assert(c);

    if (!s) {
        /* EOF */
        connection_unlink(c);
        return;
    }

    switch (c->state) {
        case STATE_REQUEST_LINE: {
            if (!pa_startswith(s, "GET "))
                goto fail;

            s +=4;

            c->url = pa_xstrndup(s, strcspn(s, " \r\n\t?"));
            c->state = STATE_MIME_HEADER;
            break;
        }

        case STATE_MIME_HEADER: {

            /* Ignore MIME headers */
            if (strcspn(s, " \r\n") != 0)
                break;

            /* We're done */
            c->state = STATE_DATA;

            handle_url(c);
            break;
        }

        default:
            ;
    }

    return;

fail:
    internal_server_error(c);
}

void pa_http_protocol_connect(pa_http_protocol *p, pa_iochannel *io, pa_module *m) {
    struct connection *c;

    pa_assert(p);
    pa_assert(io);
    pa_assert(m);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_xnew(struct connection, 1);
    c->protocol = p;
    c->line = pa_ioline_new(io);
    c->state = STATE_REQUEST_LINE;
    c->url = NULL;
    c->module = m;

    pa_ioline_set_callback(c->line, line_callback, c);

    pa_idxset_put(p->connections, c, NULL);
}

void pa_http_protocol_disconnect(pa_http_protocol *p, pa_module *m) {
    struct connection *c;
    uint32_t idx;

    pa_assert(p);
    pa_assert(m);

    PA_IDXSET_FOREACH(c, p->connections, idx)
        if (c->module == m)
            connection_unlink(c);
}

static pa_http_protocol* http_protocol_new(pa_core *c) {
    pa_http_protocol *p;

    pa_assert(c);

    p = pa_xnew(pa_http_protocol, 1);
    PA_REFCNT_INIT(p);
    p->core = c;
    p->connections = pa_idxset_new(NULL, NULL);

    pa_assert_se(pa_shared_set(c, "http-protocol", p) >= 0);

    return p;
}

pa_http_protocol* pa_http_protocol_get(pa_core *c) {
    pa_http_protocol *p;

    if ((p = pa_shared_get(c, "http-protocol")))
        return pa_http_protocol_ref(p);

    return http_protocol_new(c);
}

pa_http_protocol* pa_http_protocol_ref(pa_http_protocol *p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) >= 1);

    PA_REFCNT_INC(p);

    return p;
}

void pa_http_protocol_unref(pa_http_protocol *p) {
    struct connection *c;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) >= 1);

    if (PA_REFCNT_DEC(p) > 0)
        return;

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_unlink(c);

    pa_idxset_free(p->connections, NULL, NULL);

    pa_assert_se(pa_shared_remove(p->core, "http-protocol") >= 0);

    pa_xfree(p);
}
