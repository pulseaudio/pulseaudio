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

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#include "log.h"
#include "caps.h"

void pa_drop_root(void) {
    if (getuid() != 0 && geteuid() == 0) {
        pa_log(__FILE__": Started SUID root, dropping root rights.\n");
        setuid(getuid());
        seteuid(getuid());
    }
}

#ifdef HAVE_SYS_CAPABILITY_H
int pa_limit_caps(void) {
    int r = -1;
    cap_t caps;
    cap_value_t nice_cap = CAP_SYS_NICE;

    caps = cap_init();
    assert(caps);

    cap_clear(caps);
    cap_set_flag(caps, CAP_EFFECTIVE, 1, &nice_cap, CAP_SET);
    cap_set_flag(caps, CAP_PERMITTED, 1, &nice_cap, CAP_SET);

    if (cap_set_proc(caps) < 0)
        goto fail;

    pa_log(__FILE__": Started SUID root, capabilities limited.\n");

    r = 0;

fail:
    cap_free (caps);
    
    return r;
}

int pa_drop_caps(void) {
    cap_t caps;
    int r = -1;

    caps = cap_init();
    assert(caps);

    cap_clear(caps);

    if (cap_set_proc(caps) < 0)
        goto fail;

    pa_drop_root();
    
    r = 0;

fail:
    cap_free (caps);
    
    return r;
}

#else

int pa_limit_caps(void) {
    return 0;
}

int pa_drop_caps(void) {
    pa_drop_root();
    return 0;
}

#endif

