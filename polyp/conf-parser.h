#ifndef fooconfparserhfoo
#define fooconfparserhfoo

/* $Id$ */

/***
  This file is part of polypaudio.

  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

struct pa_config_item {
    const char *lvalue;
    int (*parse)(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata);
    void *data;
};

int pa_config_parse(const char *filename, const struct pa_config_item *t, void *userdata);

int pa_config_parse_int(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata);
int pa_config_parse_bool(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata);
int pa_config_parse_string(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata);

#endif
