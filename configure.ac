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

#### Checks for programs. ####

# CC

AC_PROG_CC
AC_PROG_GCC_TRADITIONAL
AC_GNU_SOURCE

# M4

AC_PATH_PROG([M4], [m4 gm4], [no])
if test "x$M4" = xno ; then
   AC_MSG_ERROR([m4 missing])
fi

# GCC flags

test_gcc_flag() {
    AC_LANG_CONFTEST([int main() {}])
    $CC -c conftest.c $CFLAGS $@ > /dev/null 2> /dev/null
    ret=$?
    rm -f conftest.o
    return $ret
}

# If using GCC specify some additional parameters
if test "x$GCC" = "xyes" ; then

    # We use gnu99 instead of c99 because many have interpreted the standard
    # in a way that int64_t isn't defined on non-64 bit platforms.
    DESIRED_FLAGS="-std=gnu99 -Wall -W -Wextra -pedantic -pipe -Wformat -Wold-style-definition -Wdeclaration-after-statement -Wfloat-equal -Wmissing-declarations -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls -Wmissing-noreturn -Wshadow -Wendif-labels -Wpointer-arith -Wbad-function-cast -Wcast-qual -Wcast-align -Wwrite-strings -Winline -Wno-unused-parameter"

    for flag in $DESIRED_FLAGS ; do
        AC_MSG_CHECKING([whether $CC accepts $flag])
        if test_gcc_flag $flag ; then 
           CFLAGS="$CFLAGS $flag"
           AC_MSG_RESULT([yes])
        else
           AC_MSG_RESULT([no])
        fi
    done 
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

AM_CONDITIONAL([USE_LYNX], [test "x$have_lynx" = xyes])

#### libtool stuff ####

AC_LTDL_ENABLE_INSTALL
AC_LIBLTDL_INSTALLABLE
AC_SUBST(LTDLINCL)
AC_SUBST(LIBLTDL)
AC_LIBTOOL_DLOPEN
AC_LIBTOOL_WIN32_DLL
AC_PROG_LIBTOOL
AC_CONFIG_SUBDIRS(libltdl)

if test "x$enable_ltdl_install" = "xno" && test "x$ac_cv_lib_ltdl_lt_dlinit" = "xno" ; then
    AC_MSG_ERROR([[

        *** Cannot find the libltdl development files.
        *** Maybe you need to install the libltdl-dev package.
        ]])
fi

#### Determine build environment ####

os_is_win32=0

case "$host_os" in
	mingw*)
        AC_DEFINE([OS_IS_WIN32], 1, [Build target is Windows.])
        os_is_win32=1
		;;
	esac

AM_CONDITIONAL(OS_IS_WIN32, test "x$os_is_win32" = "x1")

###################################
#   Basic environment checks      #
###################################

#### Checks for header files. ####

# ISO
AC_HEADER_STDC

# POSIX
AC_CHECK_HEADERS([arpa/inet.h glob.h grp.h netdb.h netinet/in.h \
    netinet/in_systm.h netinet/ip.h netinet/tcp.h pwd.h sched.h \
    sys/resource.h sys/select.h sys/socket.h sys/wait.h \
    syslog.h])
AC_CHECK_HEADERS([regex.h], [HAVE_REGEX=1], [HAVE_REGEX=0])
AC_CHECK_HEADERS([sys/un.h], [HAVE_AF_UNIX=1], [HAVE_AF_UNIX=0])

AM_CONDITIONAL(HAVE_REGEX, test "x$HAVE_REGEX" = "x1")
AM_CONDITIONAL(HAVE_AF_UNIX, test "x$HAVE_AF_UNIX" = "x1")

# XPG4-UNIX
AC_CHECK_HEADERS([sys/poll.h])

# Linux
AC_CHECK_HEADERS([linux/input.h], [HAVE_EVDEV=1], [HAVE_EVDEV=0])

AM_CONDITIONAL([HAVE_EVDEV], [test "x$HAVE_EVDEV" = "x1"])

# Windows
AC_CHECK_HEADERS([windows.h winsock2.h ws2tcpip.h])

# Other
AC_CHECK_HEADERS([sys/ioctl.h])

#### Typdefs, structures, etc. ####

AC_C_CONST
AC_C_BIGENDIAN
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_CHECK_TYPES(ssize_t, , [AC_DEFINE([ssize_t], [signed long],
    [Define ssize_t if it is not done by the standard libs.])])
AC_TYPE_OFF_T
AC_TYPE_SIGNAL
AC_TYPE_UID_T

AC_CHECK_DEFINE([SIGXCPU], [signal.h], [HAVE_SIGXCPU=1], [HAVE_SIGXCPU=0])
AM_CONDITIONAL(HAVE_SIGXCPU, test "x$HAVE_SIGXCPU" = "x1")

# Solaris lacks this
AC_CHECK_DEFINE([INADDR_NONE], [netinet/in.h], [],
    [AC_DEFINE([INADDR_NONE],  [0xffffffff], [Define INADDR_NONE if not found in <netinet/in.h>])])

#### Check for libs ####

# ISO
AC_CHECK_LIB([m], [pow])

# POSIX
AC_CHECK_LIB([rt], [sched_setscheduler])

# BSD
AC_CHECK_LIB([socket], [connect])

# Non-standard

# This magic is needed so we do not needlessly add static libs to the win32
# build, disabling its ability to make dlls.
AC_CHECK_FUNCS([getopt_long], [], [AC_CHECK_LIB([iberty], [getopt_long])])

#### Check for functions ####

# POSIX
AC_FUNC_FORK
AC_FUNC_GETGROUPS
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([getaddrinfo getgrgid_r getpwuid_r gettimeofday getuid \
    inet_ntop nanosleep setpgid setsid sigaction sleep])
AC_CHECK_FUNCS([mkfifo], [HAVE_MKFIFO=1], [HAVE_MKFIFO=0])

AM_CONDITIONAL(HAVE_MKFIFO, test "x$HAVE_MKFIFO" = "x1")

# X/OPEN
AC_CHECK_FUNCS([readlink])

# SUSv2
AC_CHECK_FUNCS([ctime_r usleep])

# BSD
AC_CHECK_FUNCS([lstat])

# Non-standard

AC_CHECK_FUNCS(setresuid)
AC_CHECK_FUNCS(setreuid)

#### POSIX threads ####

ACX_PTHREAD

###################################
#      External libraries         #
###################################

#### X11 (optional) ####

HAVE_X11=0

# The macro tests the host, not the build target
if test "x$os_is_win32" != "x1" ; then
    AC_PATH_XTRA
    test "x$no_x" != "xyes" && HAVE_X11=1
fi

AC_SUBST(HAVE_X11)
AM_CONDITIONAL(HAVE_X11, test "x$HAVE_X11" = "x1")
if test "x$HAVE_X11" = "x1" ; then
    AC_DEFINE([HAVE_X11], 1, [Have X11])
fi

#### Capabilities (optional) ####

CAP_LIBS=''

AC_ARG_WITH(
        [caps],
        AC_HELP_STRING([--without-caps],[Omit support for POSIX capabilities.]))

if test "x${with_caps}" != "xno"; then
    AC_CHECK_LIB(cap, cap_init, [CAP_LIBS='-lcap'], [CAP_LIBS=''])
    AC_CHECK_HEADERS([sys/capability.h])
fi
AC_SUBST(CAP_LIBS)

#### pkg-config ####

# Check for pkg-config manually first, as if its not installed the
# PKG_PROG_PKG_CONFIG macro won't be defined.
AC_CHECK_PROG(have_pkg_config, pkg-config, yes, no)

if test x"$have_pkg_config" = "xno"; then
    AC_MSG_ERROR(pkg-config is required to install this program)
fi

PKG_PROG_PKG_CONFIG

#### Sample rate conversion ####

PKG_CHECK_MODULES(LIBSAMPLERATE, [ samplerate >= 0.1.0 ])
AC_SUBST(LIBSAMPLERATE_CFLAGS)
AC_SUBST(LIBSAMPLERATE_LIBS)

#### Sound file ####

PKG_CHECK_MODULES(LIBSNDFILE, [ sndfile >= 1.0.10 ])
AC_SUBST(LIBSNDFILE_CFLAGS)
AC_SUBST(LIBSNDFILE_LIBS)

#### OSS support (optional) ####

AC_CHECK_HEADERS([sys/soundcard.h], [HAVE_OSS=1], [HAVE_OSS=0])
AC_SUBST(HAVE_OSS)
AM_CONDITIONAL([HAVE_OSS], [test "x$HAVE_OSS" = x1])

#### ALSA support (optional) ####

PKG_CHECK_MODULES(ASOUNDLIB, [ alsa >= 1.0.0 ], [HAVE_ALSA=1], [HAVE_ALSA=0])
AC_SUBST(ASOUNDLIB_CFLAGS)
AC_SUBST(ASOUNDLIB_LIBS) 
AC_SUBST(HAVE_ALSA)
AM_CONDITIONAL([HAVE_ALSA], [test "x$HAVE_ALSA" = x1])

#### Solaris audio support (optional) ####

AC_CHECK_HEADERS([sys/audio.h], [HAVE_SOLARIS=1], [HAVE_SOLARIS=0])
AC_SUBST(HAVE_SOLARIS)
AM_CONDITIONAL([HAVE_SOLARIS], [test "x$HAVE_SOLARIS" = x1])

#### GLib 2 support (optional) ####

PKG_CHECK_MODULES(GLIB20, [ glib-2.0 >= 2.4.0 ], HAVE_GLIB20=1, HAVE_GLIB20=0)
AC_SUBST(GLIB20_CFLAGS)
AC_SUBST(GLIB20_LIBS)
AC_SUBST(HAVE_GLIB20)
AM_CONDITIONAL([HAVE_GLIB20], [test "x$HAVE_GLIB20" = x1])

#### GLib 1 support (optional) ####

PKG_CHECK_MODULES(GLIB12, [ glib >= 1.2.0 ], HAVE_GLIB12=1, HAVE_GLIB12=0)
AC_SUBST(GLIB12_CFLAGS)
AC_SUBST(GLIB12_LIBS)
AC_SUBST(HAVE_GLIB12)
AM_CONDITIONAL([HAVE_GLIB12], [test "x$HAVE_GLIB12" = x1])

#### Howl support (optional) ####

PKG_CHECK_MODULES(HOWL, [ howl >= 0.9.8 ], HAVE_HOWL=1, HAVE_HOWL=0)
AC_SUBST(HOWL_CFLAGS)
AC_SUBST(HOWL_LIBS)
AC_SUBST(HAVE_HOWL)
AM_CONDITIONAL([HAVE_HOWL], [test "x$HAVE_HOWL" = x1])

#### Async DNS support (optional) ####

PKG_CHECK_MODULES(LIBASYNCNS, [ libasyncns >= 0.1 ], HAVE_LIBASYNCNS=1, HAVE_LIBASYNCNS=0)
AC_SUBST(LIBASYNCNS_CFLAGS)
AC_SUBST(LIBASYNCNS_LIBS)
AC_SUBST(HAVE_LIBASYNCNS)
AM_CONDITIONAL([HAVE_LIBASYNCNS], [test "x$HAVE_LIBASYNCNS" = x1])

if test "x$HAVE_LIBASYNCNS" != "x0" ; then
   AC_DEFINE([HAVE_LIBASYNCNS], 1, [Have libasyncns?])
fi

#### TCP wrappers (optional) ####

ACX_LIBWRAP
AC_SUBST(LIBWRAP_LIBS)

#### LIRC support (optional) ####

ACX_LIRC
AC_SUBST(LIRC_CFLAGS)
AC_SUBST(LIRC_LIBS)
AM_CONDITIONAL([HAVE_LIRC], [test "x$HAVE_LIRC" = x1])

###################################
#            Output               #
###################################

AC_ARG_ENABLE(
        [static-bins],
        AC_HELP_STRING([--enable-static-bins],[Statically link executables.]),
        [STATIC_BINS=1], [STATIC_BINS=0])
AM_CONDITIONAL([STATIC_BINS], [test "x$STATIC_BINS" = "x1"])

AC_ARG_WITH(
        [preopen-mods],
        AC_HELP_STRING([--with-preopen-mods],[Modules to preopen in daemon (default: all).]),
        [PREOPEN_MODS=$withval], [PREOPEN_MODS="all"])
AM_CONDITIONAL([PREOPEN_MODS], [test "x$PREOPEN_MODS" != "xall"])
if test "x$PREOPEN_MODS" != "xall" ; then
    tmpLIBS=""
    for mod in $PREOPEN_MODS; do
        tmpLIBS="$tmpLIBS module-$mod.la"
    done
    PREOPEN_MODS="$tmpLIBS"
    AC_SUBST(PREOPEN_MODS)
fi

AC_CONFIG_FILES([
Makefile
polyp/Makefile
polyplib.pc
polyplib-simple.pc
polyplib-mainloop.pc
polyplib-browse.pc
polyplib-error.pc
polyplib-glib-mainloop.pc
polyplib-glib12-mainloop.pc
doc/Makefile
doc/README.html
doc/cli.html
doc/daemon.html
doc/modules.html
doxygen/Makefile
doxygen/doxygen.conf
polyp/polyplib-version.h
doc/FAQ.html
])
AC_OUTPUT
