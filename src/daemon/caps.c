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

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#include <pulse/i18n.h>

#include <pulsecore/macro.h>
#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include "caps.h"

/* Glibc <= 2.2 has broken unistd.h */
#if defined(linux) && (__GLIBC__ <= 2 && __GLIBC_MINOR__ <= 2)
int setresgid(gid_t r, gid_t e, gid_t s);
int setresuid(uid_t r, uid_t e, uid_t s);
#endif

/* Drop root rights when called SUID root */
void pa_drop_root(void) {

#ifdef HAVE_GETUID
    uid_t uid;
    gid_t gid;

    pa_log_debug(_("Cleaning up privileges."));
    uid = getuid();
    gid = getgid();

#if defined(HAVE_SETRESUID)
    pa_assert_se(setresuid(uid, uid, uid) >= 0);
    pa_assert_se(setresgid(gid, gid, gid) >= 0);
#elif defined(HAVE_SETREUID)
    pa_assert_se(setreuid(uid, uid) >= 0);
    pa_assert_se(setregid(gid, gid) >= 0);
#else
    pa_assert_se(setuid(uid) >= 0);
    pa_assert_se(seteuid(uid) >= 0);
    pa_assert_se(setgid(gid) >= 0);
    pa_assert_se(setegid(gid) >= 0);
#endif

    pa_assert_se(getuid() == uid);
    pa_assert_se(geteuid() == uid);
    pa_assert_se(getgid() == gid);
    pa_assert_se(getegid() == gid);
#endif

#ifdef HAVE_SYS_PRCTL_H
    pa_assert_se(prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) == 0);
#endif

#ifdef HAVE_SYS_CAPABILITY_H
    if (uid != 0) {
        cap_t caps;
        pa_assert_se(caps = cap_init());
        pa_assert_se(cap_clear(caps) == 0);
        pa_assert_se(cap_set_proc(caps) == 0);
        pa_assert_se(cap_free(caps) == 0);
    }
#endif
}
