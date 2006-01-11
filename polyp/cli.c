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

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "ioline.h"
#include "cli.h"
#include "module.h"
#include "sink.h"
#include "source.h"
#include "client.h"
#include "sink-input.h"
#include "source-output.h"
#include "tokenizer.h"
#include "strbuf.h"
#include "namereg.h"
#include "cli-text.h"
#include "cli-command.h"
#include "xmalloc.h"
#include "log.h"

#define PROMPT ">>> "
#define PA_TYPEID_CLI PA_TYPEID_MAKE('C', 'L', 'I', '_')

struct pa_cli {
    pa_core *core;
    pa_ioline *line;

    void (*eof_callback)(pa_cli *c, void *userdata);
    void *userdata;

    pa_client *client;

    int fail, kill_requested, defer_kill;
};

static void line_callback(pa_ioline *line, const char *s, void *userdata);
static void client_kill(pa_client *c);

pa_cli* pa_cli_new(pa_core *core, pa_iochannel *io, pa_module *m) {
    char cname[256];
    pa_cli *c;
    assert(io);

    c = pa_xmalloc(sizeof(pa_cli));
    c->core = core;
    c->line = pa_ioline_new(io);
    assert(c->line);

    c->userdata = NULL;
    c->eof_callback = NULL;

    pa_iochannel_socket_peer_to_string(io, cname, sizeof(cname));
    c->client = pa_client_new(core, PA_TYPEID_CLI, cname);
    assert(c->client);
    c->client->kill = client_kill;
    c->client->userdata = c;
    c->client->owner = m;
    
    pa_ioline_set_callback(c->line, line_callback, c);
    pa_ioline_puts(c->line, "Welcome to polypaudio! Use \"help\" for usage information.\n"PROMPT);

    c->fail = c->kill_requested = c->defer_kill = 0;
    
    return c;
}

void pa_cli_free(pa_cli *c) {
    assert(c);
    pa_ioline_close(c->line);
    pa_ioline_unref(c->line);
    pa_client_free(c->client);
    pa_xfree(c);
}

static void client_kill(pa_client *client) {
    pa_cli *c;
    assert(client && client->userdata);
    c = client->userdata;
    
    pa_log_debug(__FILE__": CLI client killed.\n");
    if (c->defer_kill)
        c->kill_requested = 1;
    else {
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);
    }
}

static void line_callback(pa_ioline *line, const char *s, void *userdata) {
    pa_strbuf *buf;
    pa_cli *c = userdata;
    char *p;
    assert(line && c);

    if (!s) {
        pa_log_debug(__FILE__": CLI got EOF from user.\n");
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);

        return;
    }

    buf = pa_strbuf_new();
    assert(buf);
    c->defer_kill++;
    pa_cli_command_execute_line(c->core, s, buf, &c->fail);
    c->defer_kill--;
    pa_ioline_puts(line, p = pa_strbuf_tostring_free(buf));
    pa_xfree(p);

    if (c->kill_requested) {
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);
    } else    
        pa_ioline_puts(line, PROMPT);
}

void pa_cli_set_eof_callback(pa_cli *c, void (*cb)(pa_cli*c, void *userdata), void *userdata) {
    assert(c);
    c->eof_callback = cb;
    c->userdata = userdata;
}
