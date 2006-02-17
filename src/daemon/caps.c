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

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#include <polypcore/log.h>

#include "caps.h"

#ifdef HAVE_GETUID

/* Drop root rights when called SUID root */
void pa_drop_root(void) {
    uid_t uid = getuid();
    
    if (uid == 0 || geteuid() != 0)
        return;

    pa_log_info(__FILE__": dropping root rights.\n");

#if defined(HAVE_SETRESUID)
    setresuid(uid, uid, uid);
#elif defined(HAVE_SETREUID)
    setreuid(uid, uid);
#else
    setuid(uid);
    seteuid(uid);
#endif
}

#else

void pa_drop_root(void) {
}

#endif

#ifdef HAVE_SYS_CAPABILITY_H

/* Limit capabilities set to CAPSYS_NICE */
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

    pa_log_info(__FILE__": dropped capabilities successfully.\n"); 
    
    r = 0;

fail:
    cap_free (caps);
    
    return r;
}

/* Drop all capabilities, effectively becoming a normal user */
int pa_drop_caps(void) {
    cap_t caps;
    int r = -1;

    caps = cap_init();
    assert(caps);

    cap_clear(caps);

    if (cap_set_proc(caps) < 0) {
        pa_log(__FILE__": failed to drop capabilities: %s\n", strerror(errno));
        goto fail;
    }
    
    r = 0;

fail:
    cap_free (caps);
    
    return r;
}

#else

/* NOOPs in case capabilities are not available. */
int pa_limit_caps(void) {
    return 0;
}

int pa_drop_caps(void) {
    pa_drop_root();
    return 0;
}

#endif

