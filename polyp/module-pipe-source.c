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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "iochannel.h"
#include "source.h"
#include "module.h"
#include "util.h"
#include "modargs.h"
#include "xmalloc.h"
#include "log.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("UNIX pipe source")
PA_MODULE_VERSION(PACKAGE_VERSION)

#define DEFAULT_FIFO_NAME "/tmp/music.input"
#define DEFAULT_SOURCE_NAME "fifo_input"

struct userdata {
    struct pa_core *core;

    char *filename;
    
    struct pa_source *source;
    struct pa_iochannel *io;
    struct pa_module *module;
    struct pa_memchunk chunk;
};

static const char* const valid_modargs[] = {
    "file",
    "rate",
    "channels",
    "format",
    "source_name",
    NULL
};

static void do_read(struct userdata *u) {
    ssize_t r;
    struct pa_memchunk chunk;
    assert(u);

    if (!pa_iochannel_is_readable(u->io))
        return;

    pa_module_set_used(u->module, pa_idxset_ncontents(u->source->outputs));

    if (!u->chunk.memblock) {
        u->chunk.memblock = pa_memblock_new(1024, u->core->memblock_stat);
        u->chunk.index = chunk.length = 0;
    }

    assert(u->chunk.memblock && u->chunk.memblock->length > u->chunk.index);
    if ((r = pa_iochannel_read(u->io, (uint8_t*) u->chunk.memblock->data + u->chunk.index, u->chunk.memblock->length - u->chunk.index)) <= 0) {
        pa_log(__FILE__": read() failed: %s\n", strerror(errno));
        return;
    }

    u->chunk.length = r;
    pa_source_post(u->source, &u->chunk);
    u->chunk.index += r;

    if (u->chunk.index >= u->chunk.memblock->length) {
        u->chunk.index = u->chunk.length = 0;
        pa_memblock_unref(u->chunk.memblock);
        u->chunk.memblock = NULL;
    }
}

static void io_callback(struct pa_iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_read(u);
}

int pa__init(struct pa_core *c, struct pa_module*m) {
    struct userdata *u = NULL;
    struct stat st;
    const char *p;
    int fd = -1;
    struct pa_sample_spec ss;
    struct pa_modargs *ma = NULL;
    assert(c && m);
    
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": failed to parse module arguments\n");
        goto fail;
    }

    ss = c->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log(__FILE__": invalid sample format specification\n");
        goto fail;
    }
    
    mkfifo(p = pa_modargs_get_value(ma, "file", DEFAULT_FIFO_NAME), 0777);

    if ((fd = open(p, O_RDWR)) < 0) {
        pa_log(__FILE__": open('%s'): %s\n", p, strerror(errno));
        goto fail;
    }

    pa_fd_set_cloexec(fd, 1);
    
    if (fstat(fd, &st) < 0) {
        pa_log(__FILE__": fstat('%s'): %s\n", p, strerror(errno));
        goto fail;
    }

    if (!S_ISFIFO(st.st_mode)) {
        pa_log(__FILE__": '%s' is not a FIFO.\n", p);
        goto fail;
    }

    u = pa_xmalloc0(sizeof(struct userdata));

    u->filename = pa_xstrdup(p);
    u->core = c;
    
    if (!(u->source = pa_source_new(c, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME), 0, &ss))) {
        pa_log(__FILE__": failed to create source.\n");
        goto fail;
    }
    u->source->userdata = u;
    pa_source_set_owner(u->source, m);
    u->source->description = pa_sprintf_malloc("Unix FIFO source '%s'", p);
    assert(u->source->description);

    u->io = pa_iochannel_new(c->mainloop, fd, -1);
    assert(u->io);
    pa_iochannel_set_callback(u->io, io_callback, u);

    u->chunk.memblock = NULL;
    u->chunk.index = u->chunk.length = 0;
    
    u->module = m;
    m->userdata = u;

    pa_modargs_free(ma);
    
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
        
    if (fd >= 0)
        close(fd);

    pa__done(c, m);

    return -1;
}

void pa__done(struct pa_core *c, struct pa_module*m) {
    struct userdata *u;
    assert(c && m);

    if (!(u = m->userdata))
        return;
    
    if (u->chunk.memblock)
        pa_memblock_unref(u->chunk.memblock);
        
    pa_source_disconnect(u->source);
    pa_source_unref(u->source);
    pa_iochannel_free(u->io);

    assert(u->filename);
    unlink(u->filename);
    pa_xfree(u->filename);
    
    pa_xfree(u);
}
