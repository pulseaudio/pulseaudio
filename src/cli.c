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

struct cli {
    struct core *core;
    struct ioline *line;

    void (*eof_callback)(struct cli *c, void *userdata);
    void *userdata;

    struct client *client;
};

struct command {
    const char *name;
    void (*proc) (struct cli *cli, struct tokenizer*t);
    const char *help;
    unsigned args;
};

static void line_callback(struct ioline *line, const char *s, void *userdata);

static void cli_command_exit(struct cli *c, struct tokenizer *t);
static void cli_command_help(struct cli *c, struct tokenizer *t);
static void cli_command_modules(struct cli *c, struct tokenizer *t);
static void cli_command_clients(struct cli *c, struct tokenizer *t);
static void cli_command_sinks(struct cli *c, struct tokenizer *t);
static void cli_command_sources(struct cli *c, struct tokenizer *t);
static void cli_command_sink_inputs(struct cli *c, struct tokenizer *t);
static void cli_command_source_outputs(struct cli *c, struct tokenizer *t);
static void cli_command_stat(struct cli *c, struct tokenizer *t);
static void cli_command_info(struct cli *c, struct tokenizer *t);
static void cli_command_load(struct cli *c, struct tokenizer *t);
static void cli_command_unload(struct cli *c, struct tokenizer *t);
static void cli_command_sink_volume(struct cli *c, struct tokenizer *t);
static void cli_command_sink_input_volume(struct cli *c, struct tokenizer *t);

static const struct command commands[] = {
    { "exit",                    cli_command_exit,              "Terminate the daemon",         1 },
    { "help",                    cli_command_help,              "Show this help",               1 },
    { "modules",                 cli_command_modules,           "List loaded modules",          1 },
    { "sinks",                   cli_command_sinks,             "List loaded sinks",            1 },
    { "sources",                 cli_command_sources,           "List loaded sources",          1 },
    { "clients",                 cli_command_clients,           "List loaded clients",          1 },
    { "sink_inputs",             cli_command_sink_inputs,       "List sink inputs",             1 },
    { "source_outputs",          cli_command_source_outputs,    "List source outputs",          1 },
    { "stat",                    cli_command_stat,              "Show memory block statistics", 1 },
    { "info",                    cli_command_info,              "Show comprehensive status",    1 },
    { "load",                    cli_command_load,              "Load a module (args: name, arguments)",                     3},
    { "unload",                  cli_command_unload,            "Unload a module (args: index)",                             2},
    { "sink_volume",             cli_command_sink_volume,       "Set the volume of a sink (args: sink, volume)",             3},
    { "sink_input_volume",       cli_command_sink_input_volume, "Set the volume of a sink input (args: sink input, volume)", 3},
    { NULL, NULL, NULL, 0 }
};

static const char prompt[] = ">>> ";

struct cli* cli_new(struct core *core, struct iochannel *io) {
    char cname[256];
    struct cli *c;
    assert(io);

    c = malloc(sizeof(struct cli));
    assert(c);
    c->core = core;
    c->line = ioline_new(io);
    assert(c->line);

    c->userdata = NULL;
    c->eof_callback = NULL;

    iochannel_peer_to_string(io, cname, sizeof(cname));
    c->client = client_new(core, "CLI", cname);
    assert(c->client);
    
    ioline_set_callback(c->line, line_callback, c);
    ioline_puts(c->line, "Welcome to polypaudio! Use \"help\" for usage information.\n");
    ioline_puts(c->line, prompt);
    
    return c;
}

void cli_free(struct cli *c) {
    assert(c);
    ioline_free(c->line);
    client_free(c->client);
    free(c);
}

static void line_callback(struct ioline *line, const char *s, void *userdata) {
    struct cli *c = userdata;
    const char *cs;
    const char delimiter[] = " \t\n\r";
    assert(line && c);

    if (!s) {
        fprintf(stderr, "CLI client exited\n");
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
                struct tokenizer *t = tokenizer_new(s, command->args);
                assert(t);
                command->proc(c, t);
                tokenizer_free(t);
                unknown = 0;
                break;
            }

        if (unknown)
            ioline_puts(line, "Unknown command\n");
    }
    
    ioline_puts(c->line, prompt);
}

void cli_set_eof_callback(struct cli *c, void (*cb)(struct cli*c, void *userdata), void *userdata) {
    assert(c && cb);
    c->eof_callback = cb;
    c->userdata = userdata;
}

static void cli_command_exit(struct cli *c, struct tokenizer *t) {
    assert(c && c->core && c->core->mainloop && t);
    c->core->mainloop->quit(c->core->mainloop, 0);
}

static void cli_command_help(struct cli *c, struct tokenizer *t) {
    const struct command*command;
    struct strbuf *strbuf;
    char *p;
    assert(c && t);

    strbuf = strbuf_new();
    assert(strbuf);

    strbuf_puts(strbuf, "Available commands:\n");
    
    for (command = commands; command->name; command++)
        strbuf_printf(strbuf, "    %-20s %s\n", command->name, command->help);

    ioline_puts(c->line, p = strbuf_tostring_free(strbuf));
    free(p);
}

static void cli_command_modules(struct cli *c, struct tokenizer *t) {
    char *s;
    assert(c && t);
    s = module_list_to_string(c->core);
    assert(s);
    ioline_puts(c->line, s);
    free(s);
}

static void cli_command_clients(struct cli *c, struct tokenizer *t) {
    char *s;
    assert(c && t);
    s = client_list_to_string(c->core);
    assert(s);
    ioline_puts(c->line, s);
    free(s);
}

static void cli_command_sinks(struct cli *c, struct tokenizer *t) {
    char *s;
    assert(c && t);
    s = sink_list_to_string(c->core);
    assert(s);
    ioline_puts(c->line, s);
    free(s);
}

static void cli_command_sources(struct cli *c, struct tokenizer *t) {
    char *s;
    assert(c && t);
    s = source_list_to_string(c->core);
    assert(s);
    ioline_puts(c->line, s);
    free(s);
}

static void cli_command_sink_inputs(struct cli *c, struct tokenizer *t) {
    char *s;
    assert(c && t);
    s = sink_input_list_to_string(c->core);
    assert(s);
    ioline_puts(c->line, s);
    free(s);
}

static void cli_command_source_outputs(struct cli *c, struct tokenizer *t) {
    char *s;
    assert(c && t);
    s = source_output_list_to_string(c->core);
    assert(s);
    ioline_puts(c->line, s);
    free(s);
}

static void cli_command_stat(struct cli *c, struct tokenizer *t) {
    char txt[256];
    assert(c && t);
    snprintf(txt, sizeof(txt), "Memory blocks allocated: %u, total size: %u bytes.\n", memblock_count, memblock_total);
    ioline_puts(c->line, txt);
}

static void cli_command_info(struct cli *c, struct tokenizer *t) {
    assert(c && t);
    cli_command_stat(c, t);
    cli_command_modules(c, t);
    cli_command_sources(c, t);
    cli_command_sinks(c, t);
    cli_command_clients(c, t);
    cli_command_sink_inputs(c, t);
    cli_command_source_outputs(c, t);
}

static void cli_command_load(struct cli *c, struct tokenizer *t) {
    struct module *m;
    const char *name;
    char txt[256];
    assert(c && t);

    if (!(name = tokenizer_get(t, 1))) {
        ioline_puts(c->line, "You need to specfiy the module name and optionally arguments.\n");
        return;
    }
    
    if (!(m = module_load(c->core, name,  tokenizer_get(t, 2)))) {
        ioline_puts(c->line, "Module load failed.\n");
        return;
    }

    snprintf(txt, sizeof(txt), "Module successfully loaded, index: %u.\n", m->index);
    ioline_puts(c->line, txt);
}

static void cli_command_unload(struct cli *c, struct tokenizer *t) {
    struct module *m;
    uint32_t index;
    const char *i;
    char *e;
    assert(c && t);

    if (!(i = tokenizer_get(t, 1))) {
        ioline_puts(c->line, "You need to specfiy the module index.\n");
        return;
    }

    index = (uint32_t) strtoul(i, &e, 10);
    if (*e || !(m = idxset_get_by_index(c->core->modules, index))) {
        ioline_puts(c->line, "Invalid module index.\n");
        return;
    }

    module_unload_request(c->core, m);
}


static void cli_command_sink_volume(struct cli *c, struct tokenizer *t) {
    const char *n, *v;
    char *x = NULL;
    struct sink *sink;
    long volume;

    if (!(n = tokenizer_get(t, 1))) {
        ioline_puts(c->line, "You need to specify a sink either by its name or its index.\n");
        return;
    }

    if (!(v = tokenizer_get(t, 2))) {
        ioline_puts(c->line, "You need to specify a volume >= 0. (0 is muted, 0x100 is normal volume)\n");
        return;
    }

    volume = strtol(v, &x, 0);
    if (!x || *x != 0 || volume < 0) {
        ioline_puts(c->line, "Failed to parse volume.\n");
        return;
    }

    if (!(sink = namereg_get(c->core, n, NAMEREG_SINK))) {
        ioline_puts(c->line, "No sink found by this name or index.\n");
        return;
    }

    sink->volume = (uint32_t) volume;
}

static void cli_command_sink_input_volume(struct cli *c, struct tokenizer *t) {
    const char *n, *v;
    char *x = NULL;
    struct sink_input *si;
    long index, volume;

    if (!(n = tokenizer_get(t, 1))) {
        ioline_puts(c->line, "You need to specify a sink input by its index.\n");
        return;
    }

    index = strtol(n, &x, 0);
    if (!x || *x != 0 || index < 0) {
        ioline_puts(c->line, "Failed to parse index.\n");
        return;
    }

    if (!(v = tokenizer_get(t, 2))) {
        ioline_puts(c->line, "You need to specify a volume >= 0. (0 is muted, 0x100 is normal volume)\n");
        return;
    }

    x = NULL;
    volume = strtol(v, &x, 0);
    if (!x || *x != 0 || volume < 0) {
        ioline_puts(c->line, "Failed to parse volume.\n");
        return;
    }

    if (!(si = idxset_get_by_index(c->core->sink_inputs, (uint32_t) index))) {
        ioline_puts(c->line, "No sink input found with this index.\n");
        return;
    }

    si->volume = (uint32_t) volume;
}

