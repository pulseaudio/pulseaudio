/***
  This file is part of PulseAudio.

  Copyright 2005-2006 Lennart Poettering

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

#define internal_server_error(c) http_message((c), 500, "Internal Server Error", NULL)

#define URL_ROOT "/"
#define URL_CSS "/style"
#define URL_STATUS "/status"

struct connection {
    pa_http_protocol *protocol;
    pa_ioline *line;
    enum {
        REQUEST_LINE,
        MIME_HEADER,
        DATA
    } state;
    char *url;
    pa_module *module;
};

struct pa_http_protocol {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_idxset *connections;
};

static void http_response(struct connection *c, int code, const char *msg, const char *mime) {
    char s[256];

    pa_assert(c);
    pa_assert(msg);
    pa_assert(mime);

    pa_snprintf(s, sizeof(s),
             "HTTP/1.0 %i %s\n"
             "Connection: close\n"
             "Content-Type: %s\n"
             "Cache-Control: no-cache\n"
             "Expires: 0\n"
             "Server: "PACKAGE_NAME"/"PACKAGE_VERSION"\n"
             "\n", code, msg, mime);

    pa_ioline_puts(c->line, s);
}

static void http_message(struct connection *c, int code, const char *msg, const char *text) {
    char s[256];
    pa_assert(c);

    http_response(c, code, msg, "text/html");

    if (!text)
        text = msg;

    pa_snprintf(s, sizeof(s),
             "<?xml version=\"1.0\"?>\n"
             "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
             "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>%s</title></head>\n"
             "<body>%s</body></html>\n",
             text, text);

    pa_ioline_puts(c->line, s);
    pa_ioline_defer_close(c->line);
}


static void connection_unlink(struct connection *c) {
    pa_assert(c);

    if (c->url)
        pa_xfree(c->url);

    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);

    pa_ioline_unref(c->line);
    pa_xfree(c);
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
        case REQUEST_LINE: {
            if (memcmp(s, "GET ", 4))
                goto fail;

            s +=4;

            c->url = pa_xstrndup(s, strcspn(s, " \r\n\t?"));
            c->state = MIME_HEADER;
            break;

        }

        case MIME_HEADER: {

            /* Ignore MIME headers */
            if (strcspn(s, " \r\n") != 0)
                break;

            /* We're done */
            c->state = DATA;

            pa_log_info("request for %s", c->url);

            if (!strcmp(c->url, URL_ROOT)) {
                char txt[256];
                http_response(c, 200, "OK", "text/html");

                pa_ioline_puts(c->line,
                               "<?xml version=\"1.0\"?>\n"
                               "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
                               "<html xmlns=\"http://www.w3.org/1999/xhtml\"><title>"PACKAGE_NAME" "PACKAGE_VERSION"</title>\n"
                               "<link rel=\"stylesheet\" type=\"text/css\" href=\"style\"/></head><body>\n");

                pa_ioline_puts(c->line,
                               "<h1>"PACKAGE_NAME" "PACKAGE_VERSION"</h1>\n"
                               "<table>");

#define PRINTF_FIELD(a,b) pa_ioline_printf(c->line, "<tr><td><b>%s</b></td><td>%s</td></tr>\n",(a),(b))

                PRINTF_FIELD("User Name:", pa_get_user_name(txt, sizeof(txt)));
                PRINTF_FIELD("Host name:", pa_get_host_name(txt, sizeof(txt)));
                PRINTF_FIELD("Default Sample Specification:", pa_sample_spec_snprint(txt, sizeof(txt), &c->protocol->core->default_sample_spec));
                PRINTF_FIELD("Default Sink:", pa_namereg_get_default_sink_name(c->protocol->core));
                PRINTF_FIELD("Default Source:", pa_namereg_get_default_source_name(c->protocol->core));

                pa_ioline_puts(c->line, "</table>");

                pa_ioline_puts(c->line, "<p><a href=\"/status\">Click here</a> for an extensive server status report.</p>");

                pa_ioline_puts(c->line, "</body></html>\n");

                pa_ioline_defer_close(c->line);
            } else if (!strcmp(c->url, URL_CSS)) {
                http_response(c, 200, "OK", "text/css");

                pa_ioline_puts(c->line,
                               "body { color: black; background-color: white; margin: 0.5cm; }\n"
                               "a:link, a:visited { color: #900000; }\n"
                               "p { margin-left: 0.5cm; margin-right: 0.5cm; }\n"
                               "h1 { color: #00009F; }\n"
                               "h2 { color: #00009F; }\n"
                               "ul { margin-left: .5cm; }\n"
                               "ol { margin-left: .5cm; }\n"
                               "pre { margin-left: .5cm; background-color: #f0f0f0; padding: 0.4cm;}\n"
                               ".grey { color: #afafaf; }\n"
                               "table {  margin-left: 1cm; border:1px solid lightgrey; padding: 0.2cm; }\n"
                               "td { padding-left:10px; padding-right:10px;  }\n");

                pa_ioline_defer_close(c->line);
            } else if (!strcmp(c->url, URL_STATUS)) {
                char *r;

                http_response(c, 200, "OK", "text/plain");
                r = pa_full_status_string(c->protocol->core);
                pa_ioline_puts(c->line, r);
                pa_xfree(r);

                pa_ioline_defer_close(c->line);
            } else
                http_message(c, 404, "Not Found", NULL);

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
    c->state = REQUEST_LINE;
    c->url = NULL;
    c->module = m;

    pa_ioline_set_callback(c->line, line_callback, c);

    pa_idxset_put(p->connections, c, NULL);
}

void pa_http_protocol_disconnect(pa_http_protocol *p, pa_module *m) {
    struct connection *c;
    void *state = NULL;

    pa_assert(p);
    pa_assert(m);

    while ((c = pa_idxset_iterate(p->connections, &state, NULL)))
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
