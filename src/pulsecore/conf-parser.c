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

/* Run the user supplied parser for an assignment */
static int next_assignment(
        const char *filename,
        unsigned line,
        const char *section,
        const pa_config_item *t,
        const char *lvalue,
        const char *rvalue,
        void *userdata) {

    pa_assert(filename);
    pa_assert(t);
    pa_assert(lvalue);
    pa_assert(rvalue);

    for (; t->parse; t++) {

        if (t->lvalue && !pa_streq(lvalue, t->lvalue))
            continue;

        if (t->section && !section)
            continue;

        if (t->section && !pa_streq(section, t->section))
            continue;

        return t->parse(filename, line, section, lvalue, rvalue, t->data, userdata);
    }

    pa_log("[%s:%u] Unknown lvalue '%s' in section '%s'.", filename, line, lvalue, pa_strna(section));

    return -1;
}

/* Returns non-zero when c is contained in s */
static int in_string(char c, const char *s) {
    pa_assert(s);

    for (; *s; s++)
        if (*s == c)
            return 1;

    return 0;
}

/* Remove all whitepsapce from the beginning and the end of *s. *s may
 * be modified. */
static char *strip(char *s) {
    char *b = s+strspn(s, WHITESPACE);
    char *e, *l = NULL;

    for (e = b; *e; e++)
        if (!in_string(*e, WHITESPACE))
            l = e;

    if (l)
        *(l+1) = 0;

    return b;
}

/* Parse a variable assignment line */
static int parse_line(const char *filename, unsigned line, char **section, const pa_config_item *t, char *l, void *userdata) {
    char *e, *c, *b;

    b = l+strspn(l, WHITESPACE);

    if ((c = strpbrk(b, COMMENTS)))
        *c = 0;

    if (!*b)
        return 0;

    if (pa_startswith(b, ".include ")) {
        char *path = NULL, *fn;
        int r;

        fn = strip(b+9);
        if (!pa_is_path_absolute(fn)) {
            const char *k;
            if ((k = strrchr(filename, '/'))) {
                char *dir = pa_xstrndup(filename, k-filename);
                fn = path = pa_sprintf_malloc("%s" PA_PATH_SEP "%s", dir, fn);
                pa_xfree(dir);
            }
        }

        r = pa_config_parse(fn, NULL, t, userdata);
        pa_xfree(path);
        return r;
    }

    if (*b == '[') {
        size_t k;

        k = strlen(b);
        pa_assert(k > 0);

        if (b[k-1] != ']') {
            pa_log("[%s:%u] Invalid section header.", filename, line);
            return -1;
        }

        pa_xfree(*section);
        *section = pa_xstrndup(b+1, k-2);
        return 0;
    }

    if (!(e = strchr(b, '='))) {
        pa_log("[%s:%u] Missing '='.", filename, line);
        return -1;
    }

    *e = 0;
    e++;

    return next_assignment(filename, line, *section, t, strip(b), strip(e), userdata);
}

/* Go through the file and parse each line */
int pa_config_parse(const char *filename, FILE *f, const pa_config_item *t, void *userdata) {
    int r = -1;
    unsigned line = 0;
    pa_bool_t do_close = !f;
    char *section = NULL;

    pa_assert(filename);
    pa_assert(t);

    if (!f && !(f = fopen(filename, "r"))) {
        if (errno == ENOENT) {
            pa_log_debug("Failed to open configuration file '%s': %s", filename, pa_cstrerror(errno));
            r = 0;
            goto finish;
        }

        pa_log_warn("Failed to open configuration file '%s': %s", filename, pa_cstrerror(errno));
        goto finish;
    }

    while (!feof(f)) {
        char l[4096];

        if (!fgets(l, sizeof(l), f)) {
            if (feof(f))
                break;

            pa_log_warn("Failed to read configuration file '%s': %s", filename, pa_cstrerror(errno));
            goto finish;
        }

        if (parse_line(filename, ++line, &section, t, l, userdata) < 0)
            goto finish;
    }

    r = 0;

finish:
    pa_xfree(section);

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
