/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "conf-parser.h"

#define WHITESPACE " \t\n"
#define COMMENTS "#;\n"

struct parser_state {
    const char *filename;
    unsigned lineno;
    char *section;
    const pa_config_item *item_table;
    char buf[4096];
    void *userdata;

    char *lvalue;
    char *rvalue;
};

/* Run the user supplied parser for an assignment */
static int next_assignment(struct parser_state *state) {
    const pa_config_item *item;

    pa_assert(state);

    for (item = state->item_table; item->parse; item++) {

        if (item->lvalue && !pa_streq(state->lvalue, item->lvalue))
            continue;

        if (item->section && !state->section)
            continue;

        if (item->section && !pa_streq(state->section, item->section))
            continue;

        return item->parse(state->filename, state->lineno, state->section, state->lvalue, state->rvalue, item->data, state->userdata);
    }

    pa_log("[%s:%u] Unknown lvalue '%s' in section '%s'.", state->filename, state->lineno, state->lvalue, pa_strna(state->section));

    return -1;
}

/* Parse a variable assignment line */
static int parse_line(struct parser_state *state) {
    char *c;

    state->lvalue = state->buf + strspn(state->buf, WHITESPACE);

    if ((c = strpbrk(state->lvalue, COMMENTS)))
        *c = 0;

    if (!*state->lvalue)
        return 0;

    if (pa_startswith(state->lvalue, ".include ")) {
        char *path = NULL, *fn;
        int r;

        fn = pa_strip(state->lvalue + 9);
        if (!pa_is_path_absolute(fn)) {
            const char *k;
            if ((k = strrchr(state->filename, '/'))) {
                char *dir = pa_xstrndup(state->filename, k - state->filename);
                fn = path = pa_sprintf_malloc("%s" PA_PATH_SEP "%s", dir, fn);
                pa_xfree(dir);
            }
        }

        r = pa_config_parse(fn, NULL, state->item_table, state->userdata);
        pa_xfree(path);
        return r;
    }

    if (*state->lvalue == '[') {
        size_t k;

        k = strlen(state->lvalue);
        pa_assert(k > 0);

        if (state->lvalue[k-1] != ']') {
            pa_log("[%s:%u] Invalid section header.", state->filename, state->lineno);
            return -1;
        }

        pa_xfree(state->section);
        state->section = pa_xstrndup(state->lvalue + 1, k-2);
        return 0;
    }

    if (!(state->rvalue = strchr(state->lvalue, '='))) {
        pa_log("[%s:%u] Missing '='.", state->filename, state->lineno);
        return -1;
    }

    *state->rvalue = 0;
    state->rvalue++;

    state->lvalue = pa_strip(state->lvalue);
    state->rvalue = pa_strip(state->rvalue);

    return next_assignment(state);
}

/* Go through the file and parse each line */
int pa_config_parse(const char *filename, FILE *f, const pa_config_item *t, void *userdata) {
    int r = -1;
    pa_bool_t do_close = !f;
    struct parser_state state;

    pa_assert(filename);
    pa_assert(t);

    if (!f && !(f = pa_fopen_cloexec(filename, "r"))) {
        if (errno == ENOENT) {
            pa_log_debug("Failed to open configuration file '%s': %s", filename, pa_cstrerror(errno));
            r = 0;
            goto finish;
        }

        pa_log_warn("Failed to open configuration file '%s': %s", filename, pa_cstrerror(errno));
        goto finish;
    }

    pa_zero(state);
    state.filename = filename;
    state.item_table = t;
    state.userdata = userdata;

    while (!feof(f)) {
        if (!fgets(state.buf, sizeof(state.buf), f)) {
            if (feof(f))
                break;

            pa_log_warn("Failed to read configuration file '%s': %s", filename, pa_cstrerror(errno));
            goto finish;
        }

        state.lineno++;

        if (parse_line(&state) < 0)
            goto finish;
    }

    r = 0;

finish:
    pa_xfree(state.section);

    if (do_close && f)
        fclose(f);

    return r;
}

int pa_config_parse_int(const char *filename, unsigned line, const char *section, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    int *i = data;
    int32_t k;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atoi(rvalue, &k) < 0) {
        pa_log("[%s:%u] Failed to parse numeric value: %s", filename, line, rvalue);
        return -1;
    }

    *i = (int) k;
    return 0;
}

int pa_config_parse_unsigned(const char *filename, unsigned line, const char *section, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    unsigned *u = data;
    uint32_t k;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atou(rvalue, &k) < 0) {
        pa_log("[%s:%u] Failed to parse numeric value: %s", filename, line, rvalue);
        return -1;
    }

    *u = (unsigned) k;
    return 0;
}

int pa_config_parse_size(const char *filename, unsigned line, const char *section, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    size_t *i = data;
    uint32_t k;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if (pa_atou(rvalue, &k) < 0) {
        pa_log("[%s:%u] Failed to parse numeric value: %s", filename, line, rvalue);
        return -1;
    }

    *i = (size_t) k;
    return 0;
}

int pa_config_parse_bool(const char *filename, unsigned line, const char *section, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    int k;
    pa_bool_t *b = data;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if ((k = pa_parse_boolean(rvalue)) < 0) {
        pa_log("[%s:%u] Failed to parse boolean value: %s", filename, line, rvalue);
        return -1;
    }

    *b = !!k;

    return 0;
}

int pa_config_parse_not_bool(
        const char *filename, unsigned line,
        const char *section,
        const char *lvalue, const char *rvalue,
        void *data, void *userdata) {

    int k;
    pa_bool_t *b = data;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    if ((k = pa_parse_boolean(rvalue)) < 0) {
        pa_log("[%s:%u] Failed to parse boolean value: %s", filename, line, rvalue);
        return -1;
    }

    *b = !k;

    return 0;
}

int pa_config_parse_string(const char *filename, unsigned line, const char *section, const char *lvalue, const char *rvalue, void *data, void *userdata) {
    char **s = data;

    pa_assert(filename);
    pa_assert(lvalue);
    pa_assert(rvalue);
    pa_assert(data);

    pa_xfree(*s);
    *s = *rvalue ? pa_xstrdup(rvalue) : NULL;
    return 0;
}
