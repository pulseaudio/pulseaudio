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
#include "sinkinput.h"
#include "sourceoutput.h"
#include "tokenizer.h"
#include "strbuf.h"
#include "namereg.h"

struct pa_cli {
    struct pa_core *core;
    struct pa_ioline *line;

    void (*eof_callback)(struct pa_cli *c, void *userdata);
    void *userdata;

    struct pa_client *client;
};

struct command {
    const char *name;
    int (*proc) (struct pa_cli *cli, struct pa_tokenizer*t);
    const char *help;
    unsigned args;
};

static void line_callback(struct pa_ioline *line, const char *s, void *userdata);

static int pa_cli_command_exit(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_help(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_modules(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_clients(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_sinks(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_sources(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_sink_inputs(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_source_outputs(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_stat(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_info(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_load(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_unload(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_sink_volume(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_sink_input_volume(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_sink_default(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_source_default(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_kill_client(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_kill_sink_input(struct pa_cli *c, struct pa_tokenizer *t);
static int pa_cli_command_kill_source_output(struct pa_cli *c, struct pa_tokenizer *t);

static const struct command commands[] = {
    { "exit",                    pa_cli_command_exit,               "Terminate the daemon",         1 },
    { "help",                    pa_cli_command_help,               "Show this help",               1 },
    { "modules",                 pa_cli_command_modules,            "List loaded modules",          1 },
    { "sinks",                   pa_cli_command_sinks,              "List loaded sinks",            1 },
    { "sources",                 pa_cli_command_sources,            "List loaded sources",          1 },
    { "clients",                 pa_cli_command_clients,            "List loaded clients",          1 },
    { "sink_inputs",             pa_cli_command_sink_inputs,        "List sink inputs",             1 },
    { "source_outputs",          pa_cli_command_source_outputs,     "List source outputs",          1 },
    { "stat",                    pa_cli_command_stat,               "Show memory block statistics", 1 },
    { "info",                    pa_cli_command_info,               "Show comprehensive status",    1 },
    { "ls",                      pa_cli_command_info,               NULL,                           1 },
    { "list",                    pa_cli_command_info,               NULL,                           1 },
    { "load",                    pa_cli_command_load,               "Load a module (args: name, arguments)",                     3},
    { "unload",                  pa_cli_command_unload,             "Unload a module (args: index)",                             2},
    { "sink_volume",             pa_cli_command_sink_volume,        "Set the volume of a sink (args: index|name, volume)",             3},
    { "sink_input_volume",       pa_cli_command_sink_input_volume,  "Set the volume of a sink input (args: index|name, volume)", 3},
    { "sink_default",            pa_cli_command_sink_default,       "Set the default sink (args: index|name)", 2},
    { "source_default",          pa_cli_command_source_default,     "Set the default source (args: index|name)", 2},
    { "kill_client",             pa_cli_command_kill_client,        "Kill a client (args: index)", 2},
    { "kill_sink_input",         pa_cli_command_kill_sink_input,    "Kill a sink input (args: index)", 2},
    { "kill_source_output",      pa_cli_command_kill_source_output, "Kill a source output (args: index)", 2},
    { NULL, NULL, NULL, 0 }
};

static const char prompt[] = ">>> ";

static void client_kill(struct pa_client *c);

struct pa_cli* pa_cli_new(struct pa_core *core, struct pa_iochannel *io) {
    char cname[256];
    struct pa_cli *c;
    assert(io);

    c = malloc(sizeof(struct pa_cli));
    assert(c);
    c->core = core;
    c->line = pa_ioline_new(io);
    assert(c->line);

    c->userdata = NULL;
    c->eof_callback = NULL;

    pa_iochannel_socket_peer_to_string(io, cname, sizeof(cname));
    c->client = pa_client_new(core, "CLI", cname);
    assert(c->client);
    c->client->kill = client_kill;
    c->client->userdata = c;
    
    pa_ioline_set_callback(c->line, line_callback, c);
    pa_ioline_puts(c->line, "Welcome to polypaudio! Use \"help\" for usage information.\n");
    pa_ioline_puts(c->line, prompt);
    
    return c;
}

void pa_cli_free(struct pa_cli *c) {
    assert(c);
    pa_ioline_free(c->line);
    pa_client_free(c->client);
    free(c);
}

static void client_kill(struct pa_client *client) {
    struct pa_cli *c;
    assert(client && client->userdata);
    c = client->userdata;
    fprintf(stderr, "CLI client killed.\n");

    if (c->eof_callback)
        c->eof_callback(c, c->userdata);
}

static void line_callback(struct pa_ioline *line, const char *s, void *userdata) {
    struct pa_cli *c = userdata;
    const char *cs;
    const char delimiter[] = " \t\n\r";
    assert(line && c);

    if (!s) {
        fprintf(stderr, "CLI got EOF from user.\n");
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);

        return;
    }

    cs = s+strspn(s, delimiter);
    if (*cs && *cs != '#') {
        const struct command*command;
        int unknown = 1;
        size_t l;
        
        l = strcspn(s, delimiter);

        for (command = commands; command->name; command++) 
            if (strlen(command->name) == l && !strncmp(s, command->name, l)) {
                int ret;
                struct pa_tokenizer *t = pa_tokenizer_new(s, command->args);
                assert(t);
                ret = command->proc(c, t);
                pa_tokenizer_free(t);
                unknown = 0;

                /* A negative return value denotes that the cli object is probably invalid now */
                if (ret < 0)
                    return;
                break;
            }

        if (unknown)
            pa_ioline_puts(line, "Unknown command\n");
    }
    
    pa_ioline_puts(line, prompt);
}

void pa_cli_set_eof_callback(struct pa_cli *c, void (*cb)(struct pa_cli*c, void *userdata), void *userdata) {
    assert(c && cb);
    c->eof_callback = cb;
    c->userdata = userdata;
}

static uint32_t parse_index(const char *n) {
    long index;
    char *x;
    index = strtol(n, &x, 0);
    if (!x || *x != 0 || index < 0)
        return (uint32_t) PA_IDXSET_INVALID;

    return (uint32_t) index;
}

static int pa_cli_command_exit(struct pa_cli *c, struct pa_tokenizer *t) {
    assert(c && c->core && c->core->mainloop && t);
    c->core->mainloop->quit(c->core->mainloop, 0);
    return 0;
}

static int pa_cli_command_help(struct pa_cli *c, struct pa_tokenizer *t) {
    const struct command*command;
    struct pa_strbuf *pa_strbuf;
    char *p;
    assert(c && t);

    pa_strbuf = pa_strbuf_new();
    assert(pa_strbuf);

    pa_strbuf_puts(pa_strbuf, "Available commands:\n");
    
    for (command = commands; command->name; command++)
        if (command->help)
            pa_strbuf_printf(pa_strbuf, "    %-20s %s\n", command->name, command->help);

    pa_ioline_puts(c->line, p = pa_strbuf_tostring_free(pa_strbuf));
    free(p);
    return 0;
}

static int pa_cli_command_modules(struct pa_cli *c, struct pa_tokenizer *t) {
    char *s;
    assert(c && t);
    s = pa_module_list_to_string(c->core);
    assert(s);
    pa_ioline_puts(c->line, s);
    free(s);
    return 0;
}

static int pa_cli_command_clients(struct pa_cli *c, struct pa_tokenizer *t) {
    char *s;
    assert(c && t);
    s = pa_client_list_to_string(c->core);
    assert(s);
    pa_ioline_puts(c->line, s);
    free(s);
    return 0;
}

static int pa_cli_command_sinks(struct pa_cli *c, struct pa_tokenizer *t) {
    char *s;
    assert(c && t);
    s = pa_sink_list_to_string(c->core);
    assert(s);
    pa_ioline_puts(c->line, s);
    free(s);
    return 0;
}

static int pa_cli_command_sources(struct pa_cli *c, struct pa_tokenizer *t) {
    char *s;
    assert(c && t);
    s = pa_source_list_to_string(c->core);
    assert(s);
    pa_ioline_puts(c->line, s);
    free(s);
    return 0;
}

static int pa_cli_command_sink_inputs(struct pa_cli *c, struct pa_tokenizer *t) {
    char *s;
    assert(c && t);
    s = pa_sink_input_list_to_string(c->core);
    assert(s);
    pa_ioline_puts(c->line, s);
    free(s);
    return 0;
}

static int pa_cli_command_source_outputs(struct pa_cli *c, struct pa_tokenizer *t) {
    char *s;
    assert(c && t);
    s = pa_source_output_list_to_string(c->core);
    assert(s);
    pa_ioline_puts(c->line, s);
    free(s);
    return 0;
}

static int pa_cli_command_stat(struct pa_cli *c, struct pa_tokenizer *t) {
    char txt[256];
    assert(c && t);
    snprintf(txt, sizeof(txt), "Memory blocks allocated: %u, total size: %u bytes.\n", pa_memblock_count, pa_memblock_total);
    pa_ioline_puts(c->line, txt);
    return 0;
}

static int pa_cli_command_info(struct pa_cli *c, struct pa_tokenizer *t) {
    assert(c && t);
    pa_cli_command_stat(c, t);
    pa_cli_command_modules(c, t);
    pa_cli_command_sinks(c, t);
    pa_cli_command_sources(c, t);
    pa_cli_command_clients(c, t);
    pa_cli_command_sink_inputs(c, t);
    pa_cli_command_source_outputs(c, t);
    return 0;
}

static int pa_cli_command_load(struct pa_cli *c, struct pa_tokenizer *t) {
    struct pa_module *m;
    const char *name;
    char txt[256];
    assert(c && t);

    if (!(name = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specfiy the module name and optionally arguments.\n");
        return 0;
    }
    
    if (!(m = pa_module_load(c->core, name,  pa_tokenizer_get(t, 2)))) {
        pa_ioline_puts(c->line, "Module load failed.\n");
        return 0;
    }

    snprintf(txt, sizeof(txt), "Module successfully loaded, index: %u.\n", m->index);
    pa_ioline_puts(c->line, txt);
    return 0;
}

static int pa_cli_command_unload(struct pa_cli *c, struct pa_tokenizer *t) {
    struct pa_module *m;
    uint32_t index;
    const char *i;
    char *e;
    assert(c && t);

    if (!(i = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specfiy the module index.\n");
        return 0;
    }

    index = (uint32_t) strtoul(i, &e, 10);
    if (*e || !(m = pa_idxset_get_by_index(c->core->modules, index))) {
        pa_ioline_puts(c->line, "Invalid module index.\n");
        return 0;
    }

    pa_module_unload_request(c->core, m);
    return 0;
}

static int pa_cli_command_sink_volume(struct pa_cli *c, struct pa_tokenizer *t) {
    const char *n, *v;
    char *x = NULL;
    struct pa_sink *sink;
    long volume;

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specify a sink either by its name or its index.\n");
        return 0;
    }

    if (!(v = pa_tokenizer_get(t, 2))) {
        pa_ioline_puts(c->line, "You need to specify a volume >= 0. (0 is muted, 0x100 is normal volume)\n");
        return 0;
    }

    volume = strtol(v, &x, 0);
    if (!x || *x != 0 || volume < 0) {
        pa_ioline_puts(c->line, "Failed to parse volume.\n");
        return 0;
    }

    if (!(sink = pa_namereg_get(c->core, n, PA_NAMEREG_SINK))) {
        pa_ioline_puts(c->line, "No sink found by this name or index.\n");
        return 0;
    }

    sink->volume = (uint32_t) volume;
    return 0;
}

static int pa_cli_command_sink_input_volume(struct pa_cli *c, struct pa_tokenizer *t) {
    const char *n, *v;
    struct pa_sink_input *si;
    long volume;
    uint32_t index;
    char *x;

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specify a sink input by its index.\n");
        return 0;
    }

    if ((index = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_ioline_puts(c->line, "Failed to parse index.\n");
        return 0;
    }

    if (!(v = pa_tokenizer_get(t, 2))) {
        pa_ioline_puts(c->line, "You need to specify a volume >= 0. (0 is muted, 0x100 is normal volume)\n");
        return 0;
    }

    x = NULL;
    volume = strtol(v, &x, 0);
    if (!x || *x != 0 || volume < 0) {
        pa_ioline_puts(c->line, "Failed to parse volume.\n");
        return 0;
    }

    if (!(si = pa_idxset_get_by_index(c->core->sink_inputs, (uint32_t) index))) {
        pa_ioline_puts(c->line, "No sink input found with this index.\n");
        return 0;
    }

    si->volume = (uint32_t) volume;
    return 0;
}

static int pa_cli_command_sink_default(struct pa_cli *c, struct pa_tokenizer *t) {
    const char *n;
    struct pa_sink *sink;
    assert(c && t);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specify a sink either by its name or its index.\n");
        return 0;
    }

    if (!(sink = pa_namereg_get(c->core, n, PA_NAMEREG_SINK))) {
        pa_ioline_puts(c->line, "No sink found by this name or index.\n");
        return 0;
    }

    c->core->default_sink_index = sink->index;
    return 0;
}

static int pa_cli_command_source_default(struct pa_cli *c, struct pa_tokenizer *t) {
    const char *n;
    struct pa_source *source;
    assert(c && t);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specify a source either by its name or its index.\n");
        return 0;
    }

    if (!(source = pa_namereg_get(c->core, n, PA_NAMEREG_SOURCE))) {
        pa_ioline_puts(c->line, "No source found by this name or index.\n");
        return 0;
    }

    c->core->default_source_index = source->index;
    return 0;
}

static int pa_cli_command_kill_client(struct pa_cli *c, struct pa_tokenizer *t) {
    const char *n;
    struct pa_client *client;
    uint32_t index;
    int ret;
    assert(c && t);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specify a client by its index.\n");
        return 0;
    }

    if ((index = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_ioline_puts(c->line, "Failed to parse index.\n");
        return 0;
    }

    if (!(client = pa_idxset_get_by_index(c->core->clients, index))) {
        pa_ioline_puts(c->line, "No client found by this index.\n");
        return 0;
    }

    ret = (client->userdata == c) ? -1 : 0;
    pa_client_kill(client);
    return ret;
}

static int pa_cli_command_kill_sink_input(struct pa_cli *c, struct pa_tokenizer *t) {
    const char *n;
    struct pa_sink_input *sink_input;
    uint32_t index;
    assert(c && t);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specify a sink input by its index.\n");
        return 0;
    }

    if ((index = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_ioline_puts(c->line, "Failed to parse index.\n");
        return 0;
    }

    if (!(sink_input = pa_idxset_get_by_index(c->core->sink_inputs, index))) {
        pa_ioline_puts(c->line, "No sink input found by this index.\n");
        return 0;
    }

    pa_sink_input_kill(sink_input);
    return 0;
}

static int pa_cli_command_kill_source_output(struct pa_cli *c, struct pa_tokenizer *t) {
    const char *n;
    struct pa_source_output *source_output;
    uint32_t index;
    assert(c && t);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_ioline_puts(c->line, "You need to specify a source output by its index.\n");
        return 0;
    }

    if ((index = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_ioline_puts(c->line, "Failed to parse index.\n");
        return 0;
    }

    if (!(source_output = pa_idxset_get_by_index(c->core->source_outputs, index))) {
        pa_ioline_puts(c->line, "No source output found by this index.\n");
        return 0;
    }

    pa_source_output_kill(source_output);
    return 0;
}
