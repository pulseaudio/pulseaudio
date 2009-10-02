/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <ltdl.h>

#include <pulse/xmalloc.h>
#include <pulse/error.h>

#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/client.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/tokenizer.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/namereg.h>
#include <pulsecore/cli-text.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/sound-file.h>
#include <pulsecore/play-memchunk.h>
#include <pulsecore/sound-file-stream.h>
#include <pulsecore/shared.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/modinfo.h>

#include "cli-command.h"

struct command {
    const char *name;
    int (*proc) (pa_core *c, pa_tokenizer*t, pa_strbuf *buf, pa_bool_t *fail);
    const char *help;
    unsigned args;
};

#define META_INCLUDE ".include"
#define META_FAIL ".fail"
#define META_NOFAIL ".nofail"
#define META_IFEXISTS ".ifexists"
#define META_ELSE ".else"
#define META_ENDIF ".endif"

enum {
    IFSTATE_NONE = -1,
    IFSTATE_FALSE = 0,
    IFSTATE_TRUE = 1,
};

/* Prototypes for all available commands */
static int pa_cli_command_exit(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_help(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_modules(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_clients(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_cards(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sinks(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sources(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sink_inputs(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_source_outputs(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_stat(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_info(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_load(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_unload(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_describe(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sink_volume(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sink_input_volume(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_source_volume(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sink_mute(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_source_mute(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sink_input_mute(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sink_default(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_source_default(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_kill_client(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_kill_sink_input(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_kill_source_output(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_scache_play(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_scache_remove(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_scache_list(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_scache_load(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_scache_load_dir(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_play_file(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_dump(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_list_shared_props(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_move_sink_input(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_move_source_output(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_vacuum(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_suspend_sink(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_suspend_source(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_suspend(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_log_level(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_log_meta(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_log_time(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_log_backtrace(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_update_sink_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_update_source_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_update_sink_input_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_update_source_output_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_card_profile(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_sink_port(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);
static int pa_cli_command_source_port(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail);

/* A method table for all available commands */

static const struct command commands[] = {
    { "exit",                    pa_cli_command_exit,               "Terminate the daemon",         1 },
    { "help",                    pa_cli_command_help,               "Show this help",               1 },
    { "list-modules",            pa_cli_command_modules,            "List loaded modules",          1 },
    { "list-sinks",              pa_cli_command_sinks,              "List loaded sinks",            1 },
    { "list-sources",            pa_cli_command_sources,            "List loaded sources",          1 },
    { "list-clients",            pa_cli_command_clients,            "List loaded clients",          1 },
    { "list-sink-inputs",        pa_cli_command_sink_inputs,        "List sink inputs",             1 },
    { "list-source-outputs",     pa_cli_command_source_outputs,     "List source outputs",          1 },
    { "list-cards",              pa_cli_command_cards,              "List cards",                   1 },
    { "stat",                    pa_cli_command_stat,               "Show memory block statistics", 1 },
    { "info",                    pa_cli_command_info,               "Show comprehensive status",    1 },
    { "ls",                      pa_cli_command_info,               NULL,                           1 },
    { "list",                    pa_cli_command_info,               NULL,                           1 },
    { "load-module",             pa_cli_command_load,               "Load a module (args: name, arguments)", 3},
    { "unload-module",           pa_cli_command_unload,             "Unload a module (args: index)", 2},
    { "describe-module",         pa_cli_command_describe,           "Describe a module (arg: name)", 2},
    { "set-sink-volume",         pa_cli_command_sink_volume,        "Set the volume of a sink (args: index|name, volume)", 3},
    { "set-sink-input-volume",   pa_cli_command_sink_input_volume,  "Set the volume of a sink input (args: index, volume)", 3},
    { "set-source-volume",       pa_cli_command_source_volume,      "Set the volume of a source (args: index|name, volume)", 3},
    { "set-sink-mute",           pa_cli_command_sink_mute,          "Set the mute switch of a sink (args: index|name, bool)", 3},
    { "set-sink-input-mute",     pa_cli_command_sink_input_mute,    "Set the mute switch of a sink input (args: index, bool)", 3},
    { "set-source-mute",         pa_cli_command_source_mute,        "Set the mute switch of a source (args: index|name, bool)", 3},
    { "update-sink-proplist",    pa_cli_command_update_sink_proplist, "Update the properties of a sink (args: index|name, properties)", 3},
    { "update-source-proplist",  pa_cli_command_update_source_proplist, "Update the properties of a source (args: index|name, properties)", 3},
    { "update-sink-input-proplist", pa_cli_command_update_sink_input_proplist, "Update the properties of a sink input (args: index, properties)", 3},
    { "update-source-output-proplist", pa_cli_command_update_source_output_proplist, "Update the properties of a source_output (args: index, properties)", 3},
    { "set-default-sink",        pa_cli_command_sink_default,       "Set the default sink (args: index|name)", 2},
    { "set-default-source",      pa_cli_command_source_default,     "Set the default source (args: index|name)", 2},
    { "kill-client",             pa_cli_command_kill_client,        "Kill a client (args: index)", 2},
    { "kill-sink-input",         pa_cli_command_kill_sink_input,    "Kill a sink input (args: index)", 2},
    { "kill-source-output",      pa_cli_command_kill_source_output, "Kill a source output (args: index)", 2},
    { "list-samples",            pa_cli_command_scache_list,        "List all entries in the sample cache", 1},
    { "play-sample",             pa_cli_command_scache_play,        "Play a sample from the sample cache (args: name, sink|index)", 3},
    { "remove-sample",           pa_cli_command_scache_remove,      "Remove a sample from the sample cache (args: name)", 2},
    { "load-sample",             pa_cli_command_scache_load,        "Load a sound file into the sample cache (args: name, filename)", 3},
    { "load-sample-lazy",        pa_cli_command_scache_load,        "Lazily load a sound file into the sample cache (args: name, filename)", 3},
    { "load-sample-dir-lazy",    pa_cli_command_scache_load_dir,    "Lazily load all files in a directory into the sample cache (args: pathname)", 2},
    { "play-file",               pa_cli_command_play_file,          "Play a sound file (args: filename, sink|index)", 3},
    { "dump",                    pa_cli_command_dump,               "Dump daemon configuration", 1},
    { "shared",                  pa_cli_command_list_shared_props,  NULL, 1},
    { "move-sink-input",         pa_cli_command_move_sink_input,    "Move sink input to another sink (args: index, sink)", 3},
    { "move-source-output",      pa_cli_command_move_source_output, "Move source output to another source (args: index, source)", 3},
    { "vacuum",                  pa_cli_command_vacuum,             NULL, 1},
    { "suspend-sink",            pa_cli_command_suspend_sink,       "Suspend sink (args: index|name, bool)", 3},
    { "suspend-source",          pa_cli_command_suspend_source,     "Suspend source (args: index|name, bool)", 3},
    { "suspend",                 pa_cli_command_suspend,            "Suspend all sinks and all sources (args: bool)", 2},
    { "set-card-profile",        pa_cli_command_card_profile,       "Change the profile of a card (args: index, name)", 3},
    { "set-sink-port",           pa_cli_command_sink_port,          "Change the port of a sink (args: index, name)", 3},
    { "set-source-port",         pa_cli_command_source_port,        "Change the port of a source (args: index, name)", 3},
    { "set-log-level",           pa_cli_command_log_level,          "Change the log level (args: numeric level)", 2},
    { "set-log-meta",            pa_cli_command_log_meta,           "Show source code location in log messages (args: bool)", 2},
    { "set-log-time",            pa_cli_command_log_time,           "Show timestamps in log messages (args: bool)", 2},
    { "set-log-backtrace",       pa_cli_command_log_backtrace,      "Show backtrace in log messages (args: frames)", 2},
    { NULL, NULL, NULL, 0 }
};

static const char whitespace[] = " \t\n\r";
static const char linebreak[] = "\n\r";

static uint32_t parse_index(const char *n) {
    uint32_t idx;

    if (pa_atou(n, &idx) < 0)
        return (uint32_t) PA_IDXSET_INVALID;

    return idx;
}

static int pa_cli_command_exit(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (pa_core_exit(c, FALSE, 0) < 0)
        pa_strbuf_puts(buf, "Not allowed to terminate daemon.\n");

    return 0;
}

static int pa_cli_command_help(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const struct command*command;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_strbuf_puts(buf, "Available commands:\n");

    for (command = commands; command->name; command++)
        if (command->help)
            pa_strbuf_printf(buf, "    %-25s %s\n", command->name, command->help);
    return 0;
}

static int pa_cli_command_modules(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_module_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);
    return 0;
}

static int pa_cli_command_clients(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_client_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);
    return 0;
}

static int pa_cli_command_cards(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_card_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);
    return 0;
}

static int pa_cli_command_sinks(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_sink_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);
    return 0;
}

static int pa_cli_command_sources(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_source_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);
    return 0;
}

static int pa_cli_command_sink_inputs(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_sink_input_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);
    return 0;
}

static int pa_cli_command_source_outputs(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_source_output_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);
    return 0;
}

static int pa_cli_command_stat(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char ss[PA_SAMPLE_SPEC_SNPRINT_MAX];
    char cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char bytes[PA_BYTES_SNPRINT_MAX];
    const pa_mempool_stat *stat;
    unsigned k;
    pa_sink *def_sink;
    pa_source *def_source;

    static const char* const type_table[PA_MEMBLOCK_TYPE_MAX] = {
        [PA_MEMBLOCK_POOL] = "POOL",
        [PA_MEMBLOCK_POOL_EXTERNAL] = "POOL_EXTERNAL",
        [PA_MEMBLOCK_APPENDED] = "APPENDED",
        [PA_MEMBLOCK_USER] = "USER",
        [PA_MEMBLOCK_FIXED] = "FIXED",
        [PA_MEMBLOCK_IMPORTED] = "IMPORTED",
    };

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    stat = pa_mempool_get_stat(c->mempool);

    pa_strbuf_printf(buf, "Memory blocks currently allocated: %u, size: %s.\n",
                     (unsigned) pa_atomic_load(&stat->n_allocated),
                     pa_bytes_snprint(bytes, sizeof(bytes), (unsigned) pa_atomic_load(&stat->allocated_size)));

    pa_strbuf_printf(buf, "Memory blocks allocated during the whole lifetime: %u, size: %s.\n",
                     (unsigned) pa_atomic_load(&stat->n_accumulated),
                     pa_bytes_snprint(bytes, sizeof(bytes), (unsigned) pa_atomic_load(&stat->accumulated_size)));

    pa_strbuf_printf(buf, "Memory blocks imported from other processes: %u, size: %s.\n",
                     (unsigned) pa_atomic_load(&stat->n_imported),
                     pa_bytes_snprint(bytes, sizeof(bytes), (unsigned) pa_atomic_load(&stat->imported_size)));

    pa_strbuf_printf(buf, "Memory blocks exported to other processes: %u, size: %s.\n",
                     (unsigned) pa_atomic_load(&stat->n_exported),
                     pa_bytes_snprint(bytes, sizeof(bytes), (unsigned) pa_atomic_load(&stat->exported_size)));

    pa_strbuf_printf(buf, "Total sample cache size: %s.\n",
                     pa_bytes_snprint(bytes, sizeof(bytes), (unsigned) pa_scache_total_size(c)));

    pa_strbuf_printf(buf, "Default sample spec: %s\n",
                     pa_sample_spec_snprint(ss, sizeof(ss), &c->default_sample_spec));

    pa_strbuf_printf(buf, "Default channel map: %s\n",
                     pa_channel_map_snprint(cm, sizeof(cm), &c->default_channel_map));

    def_sink = pa_namereg_get_default_sink(c);
    def_source = pa_namereg_get_default_source(c);
    pa_strbuf_printf(buf, "Default sink name: %s\n"
                     "Default source name: %s\n",
                     def_sink ? def_sink->name : "none",
                     def_source ? def_source->name : "none");

    for (k = 0; k < PA_MEMBLOCK_TYPE_MAX; k++)
        pa_strbuf_printf(buf,
                         "Memory blocks of type %s: %u allocated/%u accumulated.\n",
                         type_table[k],
                         (unsigned) pa_atomic_load(&stat->n_allocated_by_type[k]),
                         (unsigned) pa_atomic_load(&stat->n_accumulated_by_type[k]));

    return 0;
}

static int pa_cli_command_info(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_cli_command_stat(c, t, buf, fail);
    pa_cli_command_modules(c, t, buf, fail);
    pa_cli_command_sinks(c, t, buf, fail);
    pa_cli_command_sources(c, t, buf, fail);
    pa_cli_command_clients(c, t, buf, fail);
    pa_cli_command_cards(c, t, buf, fail);
    pa_cli_command_sink_inputs(c, t, buf, fail);
    pa_cli_command_source_outputs(c, t, buf, fail);
    pa_cli_command_scache_list(c, t, buf, fail);
    return 0;
}

static int pa_cli_command_load(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *name;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(name = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify the module name and optionally arguments.\n");
        return -1;
    }

    if (!pa_module_load(c, name,  pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "Module load failed.\n");
        return -1;
    }

    return 0;
}

static int pa_cli_command_unload(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    pa_module *m;
    uint32_t idx;
    const char *i;
    char *e;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(i = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify the module index.\n");
        return -1;
    }

    idx = (uint32_t) strtoul(i, &e, 10);
    if (*e || !(m = pa_idxset_get_by_index(c->modules, idx))) {
        pa_strbuf_puts(buf, "Invalid module index.\n");
        return -1;
    }

    pa_module_unload_request(m, FALSE);
    return 0;
}

static int pa_cli_command_describe(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *name;
    pa_modinfo *i;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(name = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify the module name.\n");
        return -1;
    }

    if ((i = pa_modinfo_get_by_name(name))) {

        pa_strbuf_printf(buf, "Name: %s\n", name);

        if (!i->description && !i->version && !i->author && !i->usage)
            pa_strbuf_printf(buf, "No module information available\n");
        else {
            if (i->version)
                pa_strbuf_printf(buf, "Version: %s\n", i->version);
            if (i->description)
                pa_strbuf_printf(buf, "Description: %s\n", i->description);
            if (i->author)
                pa_strbuf_printf(buf, "Author: %s\n", i->author);
            if (i->usage)
                pa_strbuf_printf(buf, "Usage: %s\n", i->usage);
            pa_strbuf_printf(buf, "Load Once: %s\n", pa_yes_no(i->load_once));
            if (i->deprecated)
                pa_strbuf_printf(buf, "Warning, deprecated: %s\n", i->deprecated);
        }

        pa_modinfo_free(i);
    } else
        pa_strbuf_puts(buf, "Failed to open module.\n");

    return 0;
}

static int pa_cli_command_sink_volume(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *v;
    pa_sink *sink;
    uint32_t volume;
    pa_cvolume cvolume;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink either by its name or its index.\n");
        return -1;
    }

    if (!(v = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a volume >= 0. (0 is muted, 0x10000 is normal volume)\n");
        return -1;
    }

    if (pa_atou(v, &volume) < 0) {
        pa_strbuf_puts(buf, "Failed to parse volume.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, n, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink found by this name or index.\n");
        return -1;
    }

    pa_cvolume_set(&cvolume, 1, volume);
    pa_sink_set_volume(sink, &cvolume, TRUE, TRUE);
    return 0;
}

static int pa_cli_command_sink_input_volume(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *v;
    pa_sink_input *si;
    pa_volume_t volume;
    pa_cvolume cvolume;
    uint32_t idx;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink input by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(v = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a volume >= 0. (0 is muted, 0x10000 is normal volume)\n");
        return -1;
    }

    if (pa_atou(v, &volume) < 0) {
        pa_strbuf_puts(buf, "Failed to parse volume.\n");
        return -1;
    }

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, (uint32_t) idx))) {
        pa_strbuf_puts(buf, "No sink input found with this index.\n");
        return -1;
    }

    pa_cvolume_set(&cvolume, 1, volume);
    pa_sink_input_set_volume(si, &cvolume, TRUE, TRUE);
    return 0;
}

static int pa_cli_command_source_volume(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *v;
    pa_source *source;
    uint32_t volume;
    pa_cvolume cvolume;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source either by its name or its index.\n");
        return -1;
    }

    if (!(v = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a volume >= 0. (0 is muted, 0x10000 is normal volume)\n");
        return -1;
    }

    if (pa_atou(v, &volume) < 0) {
        pa_strbuf_puts(buf, "Failed to parse volume.\n");
        return -1;
    }

    if (!(source = pa_namereg_get(c, n, PA_NAMEREG_SOURCE))) {
        pa_strbuf_puts(buf, "No source found by this name or index.\n");
        return -1;
    }

    pa_cvolume_set(&cvolume, 1, volume);
    pa_source_set_volume(source, &cvolume, TRUE);
    return 0;
}

static int pa_cli_command_sink_mute(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *m;
    pa_sink *sink;
    int mute;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink either by its name or its index.\n");
        return -1;
    }

    if (!(m = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a mute switch setting (0/1).\n");
        return -1;
    }

    if ((mute = pa_parse_boolean(m)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse mute switch.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, n, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink found by this name or index.\n");
        return -1;
    }

    pa_sink_set_mute(sink, mute, TRUE);
    return 0;
}

static int pa_cli_command_source_mute(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *m;
    pa_source *source;
    int mute;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source either by its name or its index.\n");
        return -1;
    }

    if (!(m = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a mute switch setting (0/1).\n");
        return -1;
    }

    if ((mute = pa_parse_boolean(m)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse mute switch.\n");
        return -1;
    }

    if (!(source = pa_namereg_get(c, n, PA_NAMEREG_SOURCE))) {
        pa_strbuf_puts(buf, "No sink found by this name or index.\n");
        return -1;
    }

    pa_source_set_mute(source, mute, TRUE);
    return 0;
}

static int pa_cli_command_update_sink_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *s;
    pa_sink *sink;
    pa_proplist *p;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink either by its name or its index.\n");
        return -1;
    }

    if (!(s = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a \"key=value\" argument.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, n, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink found by this name or index.\n");
        return -1;
    }

    if (!(p = pa_proplist_from_string(s))) {
        pa_strbuf_puts(buf, "Failed to parse proplist.\n");
        return -1;
    }

    pa_sink_update_proplist(sink, PA_UPDATE_REPLACE, p);

    pa_proplist_free(p);

    return 0;
}

static int pa_cli_command_update_source_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *s;
    pa_source *source;
    pa_proplist *p;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source either by its name or its index.\n");
        return -1;
    }

    if (!(s = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a \"key=value\" argument.\n");
        return -1;
    }

    if (!(source = pa_namereg_get(c, n, PA_NAMEREG_SOURCE))) {
        pa_strbuf_puts(buf, "No source found by this name or index.\n");
        return -1;
    }

    if (!(p = pa_proplist_from_string(s))) {
        pa_strbuf_puts(buf, "Failed to parse proplist.\n");
        return -1;
    }

    pa_source_update_proplist(source, PA_UPDATE_REPLACE, p);

    pa_proplist_free(p);

    return 0;
}

static int pa_cli_command_update_sink_input_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *s;
    pa_sink_input *si;
    uint32_t idx;
    pa_proplist *p;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink input either by index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(s = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a \"key=value\" argument.\n");
        return -1;
    }

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, (uint32_t) idx))) {
        pa_strbuf_puts(buf, "No sink input found with this index.\n");
        return -1;
    }

    if (!(p = pa_proplist_from_string(s))) {
        pa_strbuf_puts(buf, "Failed to parse proplist.\n");
        return -1;
    }

    pa_sink_input_update_proplist(si, PA_UPDATE_REPLACE, p);

    pa_proplist_free(p);

    return 0;
}

static int pa_cli_command_update_source_output_proplist(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *s;
    pa_source_output *so;
    uint32_t idx;
    pa_proplist *p;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source output by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(s = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a \"key=value\" argument.\n");
        return -1;
    }

    if (!(so = pa_idxset_get_by_index(c->source_outputs, (uint32_t) idx))) {
        pa_strbuf_puts(buf, "No source output found with this index.\n");
        return -1;
    }

    if (!(p = pa_proplist_from_string(s))) {
        pa_strbuf_puts(buf, "Failed to parse proplist.\n");
        return -1;
    }

    pa_source_output_update_proplist(so, PA_UPDATE_REPLACE, p);

    pa_proplist_free(p);

    return 0;
}

static int pa_cli_command_sink_input_mute(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *v;
    pa_sink_input *si;
    uint32_t idx;
    int mute;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink input by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(v = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a mute switch setting (0/1).\n");
        return -1;
    }

    if ((mute = pa_parse_boolean(v)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse mute switch.\n");
        return -1;
    }

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, (uint32_t) idx))) {
        pa_strbuf_puts(buf, "No sink input found with this index.\n");
        return -1;
    }

    pa_sink_input_set_mute(si, mute, TRUE);
    return 0;
}

static int pa_cli_command_sink_default(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n;
    pa_sink *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink either by its name or its index.\n");
        return -1;
    }

    if ((s = pa_namereg_get(c, n, PA_NAMEREG_SINK)))
        pa_namereg_set_default_sink(c, s);
    else
        pa_strbuf_printf(buf, "Sink %s does not exist.\n", n);

    return 0;
}

static int pa_cli_command_source_default(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n;
    pa_source *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source either by its name or its index.\n");
        return -1;
    }

    if ((s = pa_namereg_get(c, n, PA_NAMEREG_SOURCE)))
        pa_namereg_set_default_source(c, s);
    else
        pa_strbuf_printf(buf, "Source %s does not exist.\n", n);
    return 0;
}

static int pa_cli_command_kill_client(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n;
    pa_client *client;
    uint32_t idx;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a client by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(client = pa_idxset_get_by_index(c->clients, idx))) {
        pa_strbuf_puts(buf, "No client found by this index.\n");
        return -1;
    }

    pa_client_kill(client);
    return 0;
}

static int pa_cli_command_kill_sink_input(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n;
    pa_sink_input *sink_input;
    uint32_t idx;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink input by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(sink_input = pa_idxset_get_by_index(c->sink_inputs, idx))) {
        pa_strbuf_puts(buf, "No sink input found by this index.\n");
        return -1;
    }

    pa_sink_input_kill(sink_input);
    return 0;
}

static int pa_cli_command_kill_source_output(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n;
    pa_source_output *source_output;
    uint32_t idx;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source output by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(source_output = pa_idxset_get_by_index(c->source_outputs, idx))) {
        pa_strbuf_puts(buf, "No source output found by this index.\n");
        return -1;
    }

    pa_source_output_kill(source_output);
    return 0;
}

static int pa_cli_command_scache_list(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    char *s;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_assert_se(s = pa_scache_list_to_string(c));
    pa_strbuf_puts(buf, s);
    pa_xfree(s);

    return 0;
}

static int pa_cli_command_scache_play(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *sink_name;
    pa_sink *sink;
    uint32_t idx;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1)) || !(sink_name = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a sample name and a sink name.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, sink_name, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink by that name.\n");
        return -1;
    }

    if (pa_scache_play_item(c, n, sink, PA_VOLUME_NORM, NULL, &idx) < 0) {
        pa_strbuf_puts(buf, "Failed to play sample.\n");
        return -1;
    }

    pa_strbuf_printf(buf, "Playing on sink input #%i\n", idx);

    return 0;
}

static int pa_cli_command_scache_remove(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sample name.\n");
        return -1;
    }

    if (pa_scache_remove_item(c, n) < 0) {
        pa_strbuf_puts(buf, "Failed to remove sample.\n");
        return -1;
    }

    return 0;
}

static int pa_cli_command_scache_load(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *fname, *n;
    int r;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(fname = pa_tokenizer_get(t, 2)) || !(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a file name and a sample name.\n");
        return -1;
    }

    if (strstr(pa_tokenizer_get(t, 0), "lazy"))
        r = pa_scache_add_file_lazy(c, n, fname, NULL);
    else
        r = pa_scache_add_file(c, n, fname, NULL);

    if (r < 0)
        pa_strbuf_puts(buf, "Failed to load sound file.\n");

    return 0;
}

static int pa_cli_command_scache_load_dir(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *pname;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(pname = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a path name.\n");
        return -1;
    }

    if (pa_scache_add_directory_lazy(c, pname) < 0) {
        pa_strbuf_puts(buf, "Failed to load directory.\n");
        return -1;
    }

    return 0;
}

static int pa_cli_command_play_file(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *fname, *sink_name;
    pa_sink *sink;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(fname = pa_tokenizer_get(t, 1)) || !(sink_name = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a file name and a sink name.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, sink_name, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink by that name.\n");
        return -1;
    }


    return pa_play_file(sink, fname, NULL);
}

static int pa_cli_command_list_shared_props(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_shared_dump(c, buf);
    return 0;
}

static int pa_cli_command_vacuum(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    pa_mempool_vacuum(c->mempool);

    return 0;
}

static int pa_cli_command_move_sink_input(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *k;
    pa_sink_input *si;
    pa_sink *sink;
    uint32_t idx;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink input by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(k = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a sink.\n");
        return -1;
    }

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, (uint32_t) idx))) {
        pa_strbuf_puts(buf, "No sink input found with this index.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, k, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink found by this name or index.\n");
        return -1;
    }

    if (pa_sink_input_move_to(si, sink, TRUE) < 0) {
        pa_strbuf_puts(buf, "Moved failed.\n");
        return -1;
    }
    return 0;
}

static int pa_cli_command_move_source_output(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *k;
    pa_source_output *so;
    pa_source *source;
    uint32_t idx;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source output by its index.\n");
        return -1;
    }

    if ((idx = parse_index(n)) == PA_IDXSET_INVALID) {
        pa_strbuf_puts(buf, "Failed to parse index.\n");
        return -1;
    }

    if (!(k = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a source.\n");
        return -1;
    }

    if (!(so = pa_idxset_get_by_index(c->source_outputs, (uint32_t) idx))) {
        pa_strbuf_puts(buf, "No source output found with this index.\n");
        return -1;
    }

    if (!(source = pa_namereg_get(c, k, PA_NAMEREG_SOURCE))) {
        pa_strbuf_puts(buf, "No source found by this name or index.\n");
        return -1;
    }

    if (pa_source_output_move_to(so, source, TRUE) < 0) {
        pa_strbuf_puts(buf, "Moved failed.\n");
        return -1;
    }
    return 0;
}

static int pa_cli_command_suspend_sink(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *m;
    pa_sink *sink;
    int suspend, r;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink either by its name or its index.\n");
        return -1;
    }

    if (!(m = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a suspend switch setting (0/1).\n");
        return -1;
    }

    if ((suspend = pa_parse_boolean(m)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse suspend switch.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, n, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink found by this name or index.\n");
        return -1;
    }

    if ((r = pa_sink_suspend(sink, suspend, PA_SUSPEND_USER)) < 0)
        pa_strbuf_printf(buf, "Failed to resume/suspend sink: %s\n", pa_strerror(r));

    return 0;
}

static int pa_cli_command_suspend_source(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *m;
    pa_source *source;
    int suspend, r;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source either by its name or its index.\n");
        return -1;
    }

    if (!(m = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a suspend switch setting (0/1).\n");
        return -1;
    }

    if ((suspend = pa_parse_boolean(m)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse suspend switch.\n");
        return -1;
    }

    if (!(source = pa_namereg_get(c, n, PA_NAMEREG_SOURCE))) {
        pa_strbuf_puts(buf, "No source found by this name or index.\n");
        return -1;
    }

    if ((r = pa_source_suspend(source, suspend, PA_SUSPEND_USER)) < 0)
        pa_strbuf_printf(buf, "Failed to resume/suspend source: %s\n", pa_strerror(r));

    return 0;
}

static int pa_cli_command_suspend(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *m;
    int suspend, r;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(m = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a suspend switch setting (0/1).\n");
        return -1;
    }

    if ((suspend = pa_parse_boolean(m)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse suspend switch.\n");
        return -1;
    }

    if ((r = pa_sink_suspend_all(c, suspend, PA_SUSPEND_USER)) < 0)
        pa_strbuf_printf(buf, "Failed to resume/suspend all sinks: %s\n", pa_strerror(r));

    if ((r = pa_source_suspend_all(c, suspend, PA_SUSPEND_USER)) < 0)
        pa_strbuf_printf(buf, "Failed to resume/suspend all sources: %s\n", pa_strerror(r));

    return 0;
}

static int pa_cli_command_log_level(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *m;
    uint32_t level;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(m = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a log level (0..4).\n");
        return -1;
    }

    if (pa_atou(m, &level) < 0 || level >= PA_LOG_LEVEL_MAX) {
        pa_strbuf_puts(buf, "Failed to parse log level.\n");
        return -1;
    }

    pa_log_set_level(level);

    return 0;
}

static int pa_cli_command_log_meta(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *m;
    pa_bool_t b;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(m = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a boolean.\n");
        return -1;
    }

    if ((b = pa_parse_boolean(m)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse log meta switch.\n");
        return -1;
    }

    pa_log_set_flags(PA_LOG_PRINT_META, b ? PA_LOG_SET : PA_LOG_UNSET);

    return 0;
}

static int pa_cli_command_log_time(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *m;
    pa_bool_t b;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(m = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a boolean.\n");
        return -1;
    }

    if ((b = pa_parse_boolean(m)) < 0) {
        pa_strbuf_puts(buf, "Failed to parse log meta switch.\n");
        return -1;
    }

    pa_log_set_flags(PA_LOG_PRINT_TIME, b ? PA_LOG_SET : PA_LOG_UNSET);

    return 0;
}

static int pa_cli_command_log_backtrace(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *m;
    uint32_t nframes;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(m = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a backtrace level.\n");
        return -1;
    }

    if (pa_atou(m, &nframes) < 0 || nframes >= 1000) {
        pa_strbuf_puts(buf, "Failed to parse backtrace level.\n");
        return -1;
    }

    pa_log_set_show_backtrace(nframes);

    return 0;
}

static int pa_cli_command_card_profile(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *p;
    pa_card *card;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a card either by its name or its index.\n");
        return -1;
    }

    if (!(p = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a profile by its name.\n");
        return -1;
    }

    if (!(card = pa_namereg_get(c, n, PA_NAMEREG_CARD))) {
        pa_strbuf_puts(buf, "No card found by this name or index.\n");
        return -1;
    }

    if (pa_card_set_profile(card, p, TRUE) < 0) {
        pa_strbuf_printf(buf, "Failed to set card profile to '%s'.\n", p);
        return -1;
    }

    return 0;
}

static int pa_cli_command_sink_port(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *p;
    pa_sink *sink;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a sink either by its name or its index.\n");
        return -1;
    }

    if (!(p = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a profile by its name.\n");
        return -1;
    }

    if (!(sink = pa_namereg_get(c, n, PA_NAMEREG_SINK))) {
        pa_strbuf_puts(buf, "No sink found by this name or index.\n");
        return -1;
    }

    if (pa_sink_set_port(sink, p, TRUE) < 0) {
        pa_strbuf_printf(buf, "Failed to set sink port to '%s'.\n", p);
        return -1;
    }

    return 0;
}

static int pa_cli_command_source_port(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    const char *n, *p;
    pa_source *source;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    if (!(n = pa_tokenizer_get(t, 1))) {
        pa_strbuf_puts(buf, "You need to specify a source either by its name or its index.\n");
        return -1;
    }

    if (!(p = pa_tokenizer_get(t, 2))) {
        pa_strbuf_puts(buf, "You need to specify a profile by its name.\n");
        return -1;
    }

    if (!(source = pa_namereg_get(c, n, PA_NAMEREG_SOURCE))) {
        pa_strbuf_puts(buf, "No source found by this name or index.\n");
        return -1;
    }

    if (pa_source_set_port(source, p, TRUE) < 0) {
        pa_strbuf_printf(buf, "Failed to set source port to '%s'.\n", p);
        return -1;
    }

    return 0;
}

static int pa_cli_command_dump(pa_core *c, pa_tokenizer *t, pa_strbuf *buf, pa_bool_t *fail) {
    pa_module *m;
    pa_sink *sink;
    pa_source *source;
    pa_card *card;
    pa_bool_t nl;
    uint32_t idx;
    char txt[256];
    time_t now;

    pa_core_assert_ref(c);
    pa_assert(t);
    pa_assert(buf);
    pa_assert(fail);

    time(&now);

#ifdef HAVE_CTIME_R
    pa_strbuf_printf(buf, "### Configuration dump generated at %s\n", ctime_r(&now, txt));
#else
    pa_strbuf_printf(buf, "### Configuration dump generated at %s\n", ctime(&now));
#endif

    PA_IDXSET_FOREACH(m, c->modules, idx) {

        pa_strbuf_printf(buf, "load-module %s", m->name);

        if (m->argument)
            pa_strbuf_printf(buf, " %s", m->argument);

        pa_strbuf_puts(buf, "\n");
    }

    nl = FALSE;
    PA_IDXSET_FOREACH(sink, c->sinks, idx) {

        if (!nl) {
            pa_strbuf_puts(buf, "\n");
            nl = TRUE;
        }

        pa_strbuf_printf(buf, "set-sink-volume %s 0x%03x\n", sink->name, pa_cvolume_max(pa_sink_get_volume(sink, FALSE)));
        pa_strbuf_printf(buf, "set-sink-mute %s %s\n", sink->name, pa_yes_no(pa_sink_get_mute(sink, FALSE)));
        pa_strbuf_printf(buf, "suspend-sink %s %s\n", sink->name, pa_yes_no(pa_sink_get_state(sink) == PA_SINK_SUSPENDED));
    }

    nl = FALSE;
    PA_IDXSET_FOREACH(source, c->sources, idx) {

        if (!nl) {
            pa_strbuf_puts(buf, "\n");
            nl = TRUE;
        }

        pa_strbuf_printf(buf, "set-source-volume %s 0x%03x\n", source->name, pa_cvolume_max(pa_source_get_volume(source, FALSE)));
        pa_strbuf_printf(buf, "set-source-mute %s %s\n", source->name, pa_yes_no(pa_source_get_mute(source, FALSE)));
        pa_strbuf_printf(buf, "suspend-source %s %s\n", source->name, pa_yes_no(pa_source_get_state(source) == PA_SOURCE_SUSPENDED));
    }

    nl = FALSE;
    PA_IDXSET_FOREACH(card, c->cards, idx) {

        if (!nl) {
            pa_strbuf_puts(buf, "\n");
            nl = TRUE;
        }

        if (card->active_profile)
            pa_strbuf_printf(buf, "set-card-profile %s %s\n", card->name, card->active_profile->name);
    }

    nl = FALSE;
    if ((sink = pa_namereg_get_default_sink(c))) {
        if (!nl) {
            pa_strbuf_puts(buf, "\n");
            nl = TRUE;
        }

        pa_strbuf_printf(buf, "set-default-sink %s\n", sink->name);
    }

    if ((source = pa_namereg_get_default_source(c))) {
        if (!nl)
            pa_strbuf_puts(buf, "\n");

        pa_strbuf_printf(buf, "set-default-source %s\n", source->name);
    }

    pa_strbuf_puts(buf, "\n### EOF\n");

    return 0;
}

int pa_cli_command_execute_line_stateful(pa_core *c, const char *s, pa_strbuf *buf, pa_bool_t *fail, int *ifstate) {
    const char *cs;

    pa_assert(c);
    pa_assert(s);
    pa_assert(buf);

    cs = s+strspn(s, whitespace);

    if (*cs == '#' || !*cs)
        return 0;
    else if (*cs == '.') {
        if (!strcmp(cs, META_ELSE)) {
            if (!ifstate || *ifstate == IFSTATE_NONE) {
                pa_strbuf_printf(buf, "Meta command %s is not valid in this context\n", cs);
                return -1;
            } else if (*ifstate == IFSTATE_TRUE)
                *ifstate = IFSTATE_FALSE;
            else
                *ifstate = IFSTATE_TRUE;
            return 0;
        } else if (!strcmp(cs, META_ENDIF)) {
            if (!ifstate || *ifstate == IFSTATE_NONE) {
                pa_strbuf_printf(buf, "Meta command %s is not valid in this context\n", cs);
                return -1;
            } else
                *ifstate = IFSTATE_NONE;
            return 0;
        }
        if (ifstate && *ifstate == IFSTATE_FALSE)
            return 0;
        if (!strcmp(cs, META_FAIL))
            *fail = TRUE;
        else if (!strcmp(cs, META_NOFAIL))
            *fail = FALSE;
        else {
            size_t l;
            l = strcspn(cs, whitespace);

            if (l == sizeof(META_INCLUDE)-1 && !strncmp(cs, META_INCLUDE, l)) {
                const char *filename = cs+l+strspn(cs+l, whitespace);
                if (pa_cli_command_execute_file(c, filename, buf, fail) < 0)
                    if (*fail)
                        return -1;
            } else if (l == sizeof(META_IFEXISTS)-1 && !strncmp(cs, META_IFEXISTS, l)) {
                if (!ifstate) {
                    pa_strbuf_printf(buf, "Meta command %s is not valid in this context\n", cs);
                    return -1;
                } else if (*ifstate != IFSTATE_NONE) {
                    pa_strbuf_printf(buf, "Nested %s commands not supported\n", cs);
                    return -1;
                } else {
                    const char *filename = cs+l+strspn(cs+l, whitespace);

                    /* Search DL_SEARCH_PATH unless the filename is absolute */
                    if (filename[0] == PA_PATH_SEP_CHAR) {

                        *ifstate = access(filename, F_OK) == 0 ? IFSTATE_TRUE : IFSTATE_FALSE;
                        pa_log_debug("Checking for existance of '%s': %s", filename, *ifstate == IFSTATE_TRUE ? "success" : "failure");

                    } else {
                        const char *paths, *state = NULL;
                        char *p;

                        if (!(paths = lt_dlgetsearchpath()))
                            return -1;

                        while ((p = pa_split(paths, ":", &state))) {
                            char *pathname;

                            pathname = pa_sprintf_malloc("%s" PA_PATH_SEP "%s", p, filename);
                            pa_xfree(p);

                            *ifstate = access(pathname, F_OK) == 0 ? IFSTATE_TRUE : IFSTATE_FALSE;
                            pa_log_debug("Checking for existance of '%s': %s", pathname, *ifstate == IFSTATE_TRUE ? "success" : "failure");

                            pa_xfree(pathname);

                            if (*ifstate == IFSTATE_TRUE)
                                break;
                        }
                    }

                }
            } else {
                pa_strbuf_printf(buf, "Invalid meta command: %s\n", cs);
                if (*fail) return -1;
            }
        }
    } else {
        const struct command*command;
        int unknown = 1;
        size_t l;

        if (ifstate && *ifstate == IFSTATE_FALSE)
            return 0;

        l = strcspn(cs, whitespace);

        for (command = commands; command->name; command++)
            if (strlen(command->name) == l && !strncmp(cs, command->name, l)) {
                int ret;
                pa_tokenizer *t = pa_tokenizer_new(cs, command->args);
                pa_assert(t);
                ret = command->proc(c, t, buf, fail);
                pa_tokenizer_free(t);
                unknown = 0;

                if (ret < 0 && *fail)
                    return -1;

                break;
            }

        if (unknown) {
            pa_strbuf_printf(buf, "Unknown command: %s\n", cs);
            if (*fail)
                return -1;
        }
    }

    return 0;
}

int pa_cli_command_execute_line(pa_core *c, const char *s, pa_strbuf *buf, pa_bool_t *fail) {
    return pa_cli_command_execute_line_stateful(c, s, buf, fail, NULL);
}

int pa_cli_command_execute_file_stream(pa_core *c, FILE *f, pa_strbuf *buf, pa_bool_t *fail) {
    char line[1024];
    int ifstate = IFSTATE_NONE;
    int ret = -1;
    pa_bool_t _fail = TRUE;

    pa_assert(c);
    pa_assert(f);
    pa_assert(buf);

    if (!fail)
        fail = &_fail;

    while (fgets(line, sizeof(line), f)) {
        pa_strip_nl(line);

        if (pa_cli_command_execute_line_stateful(c, line, buf, fail, &ifstate) < 0 && *fail)
            goto fail;
    }

    ret = 0;

fail:

    return ret;
}

int pa_cli_command_execute_file(pa_core *c, const char *fn, pa_strbuf *buf, pa_bool_t *fail) {
    FILE *f = NULL;
    int ret = -1;
    pa_bool_t _fail = TRUE;

    pa_assert(c);
    pa_assert(fn);
    pa_assert(buf);

    if (!fail)
        fail = &_fail;

    if (!(f = fopen(fn, "r"))) {
        pa_strbuf_printf(buf, "open('%s') failed: %s\n", fn, pa_cstrerror(errno));
        if (!*fail)
            ret = 0;
        goto fail;
    }

    ret = pa_cli_command_execute_file_stream(c, f, buf, fail);

fail:
    if (f)
        fclose(f);

    return ret;
}

int pa_cli_command_execute(pa_core *c, const char *s, pa_strbuf *buf, pa_bool_t *fail) {
    const char *p;
    int ifstate = IFSTATE_NONE;
    pa_bool_t _fail = TRUE;

    pa_assert(c);
    pa_assert(s);
    pa_assert(buf);

    if (!fail)
        fail = &_fail;

    p = s;
    while (*p) {
        size_t l = strcspn(p, linebreak);
        char *line = pa_xstrndup(p, l);

        if (pa_cli_command_execute_line_stateful(c, line, buf, fail, &ifstate) < 0 && *fail) {
            pa_xfree(line);
            return -1;
        }
        pa_xfree(line);

        p += l;
        p += strspn(p, linebreak);
    }

    return 0;
}
