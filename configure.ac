# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# $Id$

# This file is part of polypaudio.
#
# polypaudio is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# polypaudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with polypaudio; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.

AC_PREREQ(2.57)
AC_INIT([polypaudio],[0.8],[mzcbylcnhqvb (at) 0pointer (dot) de])
AC_CONFIG_SRCDIR([polyp/main.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign -Wall])

AC_SUBST(PA_MAJORMINOR, "$PACKAGE_VERSION")
AC_SUBST(PACKAGE_URL, [http://0pointer.de/lennart/projects/polypaudio/])

AC_SUBST(PA_API_VERSION, 8)

if type -p stow > /dev/null && test -d /usr/local/stow ; then
   AC_MSG_NOTICE([*** Found /usr/local/stow: default install prefix set to /usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION} ***])
   ac_default_prefix="/usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION}"
fi

# Checks for programs.
AC_PROG_CC

# libtool stuff
AC_LIBLTDL_INSTALLABLE
AC_SUBST(LTDLINCL)
AC_SUBST(LIBLTDL)
AC_LIBTOOL_DLOPEN
AC_PROG_LIBTOOL
AC_CONFIG_SUBDIRS(libltdl)

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([arpa/inet.h fcntl.h inttypes.h limits.h malloc.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h unistd.h syslog.h])

ACX_PTHREAD
AC_PATH_XTRA

HAVE_X11=0
test "x$no_x" != "xyes" && HAVE_X11=1
AC_SUBST(HAVE_X11)
AM_CONDITIONAL(HAVE_X11, test "x$no_x" != "xyes")
if test "x$no_x" != "xyes" ; then
   AC_DEFINE([HAVE_X11], 1, [Have X11])
fi

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_OFF_T
AC_HEADER_TIME

# Checks for library functions.
AC_FUNC_FORK
AC_PROG_GCC_TRADITIONAL
AC_FUNC_LSTAT
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_FUNC_SETPGRP
AC_FUNC_VPRINTF
AC_FUNC_CLOSEDIR_VOID
AC_FUNC_SELECT_ARGTYPES
AC_TYPE_SIGNAL
AC_TYPE_UID_T
AC_CHECK_FUNCS([gethostname gettimeofday memchr memmove memset mkdir mkfifo munmap rmdir socket strcspn strerror strrchr strspn strstr strtol strtoul strcasecmp putenv strchr strpbrk strdup getgrgid_r getpwuid_r regcomp ftruncate select])
AC_CHECK_LIB(m, pow)
AC_CHECK_FUNCS(pow)
AC_FUNC_STAT
AC_HEADER_SYS_WAIT
AC_HEADER_DIRENT

AC_C_BIGENDIAN
AC_FUNC_GETGROUPS

AC_CHECK_LIB(cap, cap_init, [CAP_LIBS='-lcap'], [CAP_LIBS=''])
AC_SUBST(CAP_LIBS)

AC_CHECK_HEADERS(sys/capability.h)

AC_CHECK_FUNCS(setresuid)
AC_CHECK_FUNCS(setreuid)

PKG_CHECK_MODULES(LIBSAMPLERATE, [ samplerate >= 0.1.0 ])
AC_SUBST(LIBSAMPLERATE_CFLAGS)
AC_SUBST(LIBSAMPLERATE_LIBS)

PKG_CHECK_MODULES(LIBSNDFILE, [ sndfile >= 1.0.10 ])
AC_SUBST(LIBSNDFILE_CFLAGS)
AC_SUBST(LIBSNDFILE_LIBS)

PKG_CHECK_MODULES(ASOUNDLIB, [ alsa >= 1.0.0 ], [HAVE_ALSA=1], [HAVE_ALSA=0])
AC_SUBST(ASOUNDLIB_CFLAGS)
AC_SUBST(ASOUNDLIB_LIBS) 
AC_SUBST(HAVE_ALSA)
AM_CONDITIONAL([HAVE_ALSA], [test "x$HAVE_ALSA" = x1])

PKG_CHECK_MODULES(GLIB20, [ glib-2.0 >= 2.4.0 ], HAVE_GLIB20=1, HAVE_GLIB20=0)
AC_SUBST(GLIB20_CFLAGS)
AC_SUBST(GLIB20_LIBS)
AC_SUBST(HAVE_GLIB20)
AM_CONDITIONAL([HAVE_GLIB20], [test "x$HAVE_GLIB20" = x1])

PKG_CHECK_MODULES(GLIB12, [ glib >= 1.2.0 ], HAVE_GLIB12=1, HAVE_GLIB12=0)
AC_SUBST(GLIB12_CFLAGS)
AC_SUBST(GLIB12_LIBS)
AC_SUBST(HAVE_GLIB12)
AM_CONDITIONAL([HAVE_GLIB12], [test "x$HAVE_GLIB12" = x1])

PKG_CHECK_MODULES(HOWL, [ howl >= 0.9.8 ], HAVE_HOWL=1, HAVE_HOWL=0)
AC_SUBST(HOWL_CFLAGS)
AC_SUBST(HOWL_LIBS)
AC_SUBST(HAVE_HOWL)
AM_CONDITIONAL([HAVE_HOWL], [test "x$HAVE_HOWL" = x1])

PKG_CHECK_MODULES(LIBASYNCNS, [ libasyncns >= 0.1 ], HAVE_LIBASYNCNS=1, HAVE_LIBASYNCNS=0)
AC_SUBST(LIBASYNCNS_CFLAGS)
AC_SUBST(LIBASYNCNS_LIBS)
AC_SUBST(HAVE_LIBASYNCNS)
AM_CONDITIONAL([HAVE_LIBASYNCNS], [test "x$HAVE_LIBASYNCNS" = x1])

if test "x$HAVE_LIBASYNCNS" != "x0" ; then
   AC_DEFINE([HAVE_LIBASYNCNS], 1, [Have libasyncns?])
fi

AC_PATH_PROG([M4], [m4 gm4], [no])
if test "x$M4" = xno ; then
   AC_MSG_ERROR([m4 missing])
fi

AC_MSG_CHECKING([for tcpwrap library and headers])
LIBWRAP_LIBS=
saved_LIBS="$LIBS"
LIBS="$LIBS -lwrap"
AC_LINK_IFELSE(
AC_LANG_PROGRAM(
[#include <tcpd.h>
#include <syslog.h>
int allow_severity = LOG_INFO;
int deny_severity = LOG_WARNING;],
[struct request_info *req; 
return hosts_access (req);]),
[AC_DEFINE(HAVE_LIBWRAP, [], [Have tcpwrap?])
LIBWRAP_LIBS="-lwrap"
AC_MSG_RESULT(yes)],
[AC_MSG_RESULT(no)])
AC_SUBST(LIBWRAP_LIBS)
LIBS="$saved_LIBS"

LIRC_CFLAGS=
LIRC_LIBS=
AC_CHECK_HEADER(lirc/lirc_client.h,[AC_CHECK_LIB(lirc_client,lirc_init,[HAVE_LIRC=1
LIRC_LIBS=-llirc_client],HAVE_LIRC=0)],HAVE_LIRC=0)
AC_SUBST(LIRC_CFLAGS)
AC_SUBST(LIRC_LIBS)
AM_CONDITIONAL([HAVE_LIRC], [test "x$HAVE_LIRC" = x1])

AC_CHECK_HEADER(linux/input.h,HAVE_EVDEV=1,HAVE_EVDEV=0)
AM_CONDITIONAL([HAVE_EVDEV], [test "x$HAVE_EVDEV" = x1])

# If using GCC specify some additional parameters
if test "x$GCC" = "xyes" ; then
   CFLAGS="$CFLAGS -pipe -W -Wall -pedantic"

   AC_LANG_CONFTEST([int main() {}])
   $CC -c conftest.c -std=c99 -Wno-unused-parameter $CFLAGS > /dev/null 2> /dev/null && CFLAGS="$CFLAGS -std=c99 -Wno-unused-parameter"
   rm -f conftest.o
fi

# LYNX documentation generation
AC_ARG_ENABLE(lynx,
        AC_HELP_STRING(--disable-lynx,Turn off lynx usage for documentation generation),
[case "${enableval}" in
  yes) lynx=yes ;;
  no)  lynx=no ;;
  *) AC_MSG_ERROR(bad value ${enableval} for --disable-lynx) ;;
esac],[lynx=yes])

if test x$lynx = xyes ; then
   AC_CHECK_PROG(have_lynx, lynx, yes, no)

   if test x$have_lynx = xno ; then
     AC_MSG_WARN([*** lynx not found, plain text README will not be built ***])
   fi
fi

AM_CONDITIONAL([USE_LYNX], [test "x$lynx" = xyes])

AM_CONDITIONAL(BUILD_LIBPOLYPCORE, false)

AC_CONFIG_FILES([Makefile polyp/Makefile polyplib.pc polyplib-simple.pc polyplib-mainloop.pc polyplib-browse.pc polyplib-error.pc polyplib-glib-mainloop.pc polyplib-glib12-mainloop.pc doc/Makefile doc/README.html doc/cli.html doc/daemon.html doc/modules.html doxygen/Makefile doxygen/doxygen.conf polyp/polyplib-version.h doc/FAQ.html])
AC_OUTPUT
