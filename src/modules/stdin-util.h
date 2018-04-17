#ifndef foostdinutilhfoo
#define foostdinutilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

#include <pulsecore/module.h>
#include <pulsecore/typedefs.h>

#define MAX_MODULES 10
#define BUF_MAX 2048

struct userdata;

struct module_item {
    char *name;
    char *args;
    uint32_t index;
};

struct pa_module_info {
    struct userdata *userdata;
    char *name;

    struct module_item items[MAX_MODULES];
    unsigned n_items;
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_hashmap *module_infos;

    pid_t pid;

    int fd;
    int fd_type;
    pa_io_event *io_event;

    char buf[BUF_MAX];
    size_t buf_fill;
};

int fill_buf(struct userdata *u);
int read_byte(struct userdata *u);
char *read_string(struct userdata *u);
void unload_one_module(struct pa_module_info *m, unsigned i);
void unload_all_modules(struct pa_module_info *m);
void load_module(
  struct pa_module_info *m,
  unsigned i,
  const char *name,
  const char *args,
  bool is_new);
void module_info_free(void *p);
int handle_event(struct userdata *u);
void io_event_cb(
  pa_mainloop_api*a,
  pa_io_event* e,
  int fd,
  pa_io_event_flags_t events,
  void *userdata);

#endif
