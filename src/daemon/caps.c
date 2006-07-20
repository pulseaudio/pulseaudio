/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#include <pulsecore/core-error.h>

#include <pulsecore/log.h>

#include "caps.h"

/* Glibc <= 2.2 has broken unistd.h */
#if defined(linux) && (__GLIBC__ <= 2 && __GLIBC_MINOR__ <= 2)
int setresgid(gid_t r, gid_t e, gid_t s);
int setresuid(uid_t r, uid_t e, uid_t s);
#endif

#ifdef HAVE_GETUID

/* Drop root rights when called SUID root */
void pa_drop_root(void) {
    uid_t uid = getuid();
    
    if (uid == 0 || geteuid() != 0)
        return;

    pa_log_info(__FILE__": dropping root rights.");

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

    /* Only drop caps when called SUID */
    if (getuid() != 0)
        return 0;

    caps = cap_init();
    assert(caps);

    cap_clear(caps);

    cap_set_flag(caps, CAP_EFFECTIVE, 1, &nice_cap, CAP_SET);
    cap_set_flag(caps, CAP_PERMITTED, 1, &nice_cap, CAP_SET);

    if (cap_set_proc(caps) < 0)
        goto fail;

    pa_log_info(__FILE__": dropped capabilities successfully."); 
    
    r = 0;

fail:
    cap_free (caps);
    
    return r;
}

/* Drop all capabilities, effectively becoming a normal user */
int pa_drop_caps(void) {
    cap_t caps;
    int r = -1;

    /* Only drop caps when called SUID */
    if (getuid() != 0)
        return 0;

    caps = cap_init();
    assert(caps);

    cap_clear(caps);

    if (cap_set_proc(caps) < 0) {
        pa_log(__FILE__": failed to drop capabilities: %s", pa_cstrerror(errno));
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

