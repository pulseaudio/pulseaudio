# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# $Id$

# This file is part of polypaudio.
#
# polypaudio is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# polypaudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with polypaudio; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.

AC_PREREQ(2.57)
AC_INIT([polypaudio],[0.1],[mzcbylcnhqvb (at) 0pointer (dot) de])
AC_CONFIG_SRCDIR([polyp/main.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign -Wall])

AC_SUBST(PACKAGE_URL, [http://0pointer.de/lennart/projects/polypaudio/])

if type -p stow > /dev/null && test -d /usr/local/stow ; then
   AC_MSG_NOTICE([*** Found /usr/local/stow: default install prefix set to /usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION} ***])
   ac_default_prefix="/usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION}"
fi

# Checks for programs.
AC_PROG_CC
AC_LIBLTDL_INSTALLABLE
AC_SUBST(INCLTDL)
AC_SUBST(LIBLTDL)
AC_LIBTOOL_DLOPEN
AC_PROG_LIBTOOL

AC_C_BIGENDIAN

PKG_CHECK_MODULES(LIBSAMPLERATE, [ samplerate >= 0.1.0 ])
AC_SUBST(LIBSAMPLERATE_CFLAGS)
AC_SUBST(LIBSAMPLERATE_LIBS)

PKG_CHECK_MODULES(LIBSNDFILE, [ sndfile >= 1.0.0 ])
AC_SUBST(LIBSNDFILE_CFLAGS)
AC_SUBST(LIBSNDFILE_LIBS)

PKG_CHECK_MODULES(ASOUNDLIB, [ alsa >= 1.0.0 ])
AC_SUBST(ASOUNDLIB_CFLAGS)
AC_SUBST(ASOUNDLIB_LIBS)

PKG_CHECK_MODULES(GLIB20, [ glib-2.0 >= 2.4.0 ])
AC_SUBST(GLIB20_CFLAGS)
AC_SUBST(GLIB20_LIBS)

# If using GCC specifiy some additional parameters
if test "x$GCC" = "xyes" ; then
   CFLAGS="$CFLAGS -pipe -Wall -W -Wno-unused-parameter"
fi

# LYNX documentation generation
AC_ARG_ENABLE(lynx,
        AS_HELP_STRING(--disable-lynx,Turn off lynx usage for documentation generation),
[case "${enableval}" in
  yes) lynx=yes ;;
  no)  lynx=no ;;
  *) AC_MSG_ERROR(bad value ${enableval} for --disable-lynx) ;;
esac],[lynx=yes])

if test x$lynx = xyes ; then
   AC_CHECK_PROG(have_lynx, lynx, yes, no)

   if test x$have_lynx = xno ; then
     AC_MSG_ERROR([*** Sorry, you have to install lynx or use --disable-lynx ***])
   fi
fi

AM_CONDITIONAL([USE_LYNX], [test "x$lynx" = xyes])

AM_CONDITIONAL(BUILD_LIBPOLYPCORE, false)

AC_CONFIG_FILES([Makefile polyp/Makefile polyplib.pc polyplib-simple.pc polyplib-mainloop.pc polyplib-error.pc doc/Makefile doc/README.html doc/cli.html doc/daemon.html doc/modules.html doxygen/Makefile doxygen/doxygen.conf])
AC_OUTPUT
