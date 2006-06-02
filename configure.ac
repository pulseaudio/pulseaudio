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

m4_define(PA_MAJOR, [0])
m4_define(PA_MINOR, [9])
m4_define(PA_MICRO, [1])

AC_INIT([polypaudio], PA_MAJOR.PA_MINOR.PA_MICRO,[mzcbylcnhqvb (at) 0pointer (dot) de])
AC_CONFIG_SRCDIR([src/daemon/main.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign -Wall])

AC_SUBST(PA_MAJORMINOR, "PA_MAJOR.PA_MINOR")
AC_SUBST(PACKAGE_URL, [http://0pointer.de/lennart/projects/polypaudio/])

AC_SUBST(PA_API_VERSION, 9)
AC_SUBST(PA_PROTOCOL_VERSION, 9)

AC_SUBST(LIBPOLYP_VERSION_INFO, [0:0:0])
AC_SUBST(LIBPOLYPCORE_VERSION_INFO, [0:1:0])
AC_SUBST(LIBPOLYP_SIMPLE_VERSION_INFO, [0:0:0])
AC_SUBST(LIBPOLYP_BROWSE_VERSION_INFO, [0:0:0])
AC_SUBST(LIBPOLYP_MAINLOOP_GLIB_VERSION_INFO, [0:0:0])

if type -p stow > /dev/null && test -d /usr/local/stow ; then
   AC_MSG_NOTICE([*** Found /usr/local/stow: default install prefix set to /usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION} ***])
   ac_default_prefix="/usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION}"
fi

#### Platform hacks ####

case $host in
   *-*-solaris* )
      AC_DEFINE(_XOPEN_SOURCE_EXTENDED, 1, Needed to get declarations for msg_control and msg_controllen on Solaris)
      AC_DEFINE(_XOPEN_SOURCE,          2, Needed to get declarations for msg_control and msg_controllen on Solaris)
      AC_DEFINE(__EXTENSIONS__,         1, Needed to get declarations for msg_control and msg_controllen on Solaris)
      ;;
esac

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
    DESIRED_FLAGS="-std=gnu99 -Wall -W -Wextra -pedantic -pipe -Wformat -Wold-style-definition -Wdeclaration-after-statement -Wfloat-equal -Wmissing-declarations -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls -Wmissing-noreturn -Wshadow -Wendif-labels -Wpointer-arith -Wcast-align -Wwrite-strings -Winline -Wno-unused-parameter"

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
esac],[lynx=auto])

if test x$lynx != xno ; then
   AC_CHECK_PROG(have_lynx, lynx, yes, no)

   if test x$have_lynx = xno ; then
       if test x$lynx = xyes ; then
           AC_MSG_ERROR([*** lynx not found])
       else
           AC_MSG_WARN([*** lynx not found, plain text README will not be built ***])
       fi
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
AC_CHECK_HEADERS([arpa/inet.h glob.h grp.h iconv.h netdb.h netinet/in.h \
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

# Solaris
AC_CHECK_HEADERS([sys/filio.h])

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
AC_SEARCH_LIBS([pow], [m])

# POSIX
AC_SEARCH_LIBS([sched_setscheduler], [rt])
AC_SEARCH_LIBS([dlopen], [dl])

# BSD
AC_SEARCH_LIBS([connect], [socket])

# Non-standard

# This magic is needed so we do not needlessly add static libs to the win32
# build, disabling its ability to make dlls.
AC_CHECK_FUNCS([getopt_long], [], [AC_CHECK_LIB([iberty], [getopt_long])])

#### Check for functions ####

# POSIX
AC_FUNC_FORK
AC_FUNC_GETGROUPS
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([chmod chown getaddrinfo getgrgid_r getpwuid_r gettimeofday \
    getuid inet_ntop inet_pton nanosleep pipe setpgid setsid sigaction sleep])
AC_CHECK_FUNCS([mkfifo], [HAVE_MKFIFO=1], [HAVE_MKFIFO=0])

AM_CONDITIONAL(HAVE_MKFIFO, test "x$HAVE_MKFIFO" = "x1")

# X/OPEN
AC_CHECK_FUNCS([readlink])

# SUSv2
AC_CHECK_FUNCS([ctime_r usleep])

# SUSv3
AC_CHECK_FUNCS([strerror_r])

# BSD
AC_CHECK_FUNCS([lstat])

# Non-standard

AC_CHECK_FUNCS(setresuid)
AC_CHECK_FUNCS(setreuid)

#### POSIX threads ####

ACX_PTHREAD

#### Large File-Support (LFS) ####

AC_SYS_LARGEFILE

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
    AC_SEARCH_LIBS([cap_init], [cap], [], [
                    if test "x${with_caps}" = "xyes" ; then
                        AC_MSG_ERROR([*** POSIX caps libraries not found])
                    fi])
    AC_CHECK_HEADERS([sys/capability.h], [], [
                    if test "x${with_caps}" = "xyes" ; then
                        AC_MSG_ERROR([*** POSIX caps headers not found])
                    fi])
fi

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

AC_ARG_ENABLE([oss], 
    AC_HELP_STRING([--disable-oss], [Disable optional OSS support]), 
    [case "${enableval}" in yes) oss=yes ;; no) oss=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-oss) ;; esac],[oss=auto])
if test "x${oss}" != xno ; then
    AC_CHECK_HEADERS([sys/soundcard.h], [
    HAVE_OSS=1
    AC_DEFINE([HAVE_OSS], 1, [Have OSS?])
    ], [HAVE_OSS=0 ; if test "x$oss" = xyes ; then AC_MSG_ERROR([*** OSS support not found]) ; fi])
else HAVE_OSS=0; fi
AC_SUBST(HAVE_OSS)
AM_CONDITIONAL([HAVE_OSS], [test "x$HAVE_OSS" = x1])


#### ALSA support (optional) ####

AC_ARG_ENABLE([alsa], 
    AC_HELP_STRING([--disable-alsa], [Disable optional ALSA support]), 
    [case "${enableval}" in yes) alsa=yes ;; no) alsa=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-alsa) ;; esac],[alsa=auto])
if test "x${alsa}" != xno ; then
    PKG_CHECK_MODULES(ASOUNDLIB, [ alsa >= 1.0.0 ], [
    HAVE_ALSA=1
    AC_DEFINE([HAVE_ALSA], 1, [Have ALSA?])
    ], [HAVE_ALSA=0 ; if test "x$alsa" = xyes ; then AC_MSG_ERROR([*** ALSA support not found]) ; fi])
else HAVE_ALSA=0 ; fi
AC_SUBST(ASOUNDLIB_CFLAGS)
AC_SUBST(ASOUNDLIB_LIBS) 
AC_SUBST(HAVE_ALSA)
AM_CONDITIONAL([HAVE_ALSA], [test "x$HAVE_ALSA" = x1])

#### Solaris audio support (optional) ####

AC_ARG_ENABLE([solaris], 
    AC_HELP_STRING([--disable-solaris], [Disable optional Solaris audio support]), 
    [case "${enableval}" in yes) solaris=yes ;; no) solaris=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-solaris) ;; esac],[solaris=auto])
if test "x${solaris}" != xno ; then
    AC_CHECK_HEADERS([sys/audio.h], [
    HAVE_SOLARIS=1
    AC_DEFINE([HAVE_SOLARIS], 1, [Have Solaris audio?])
    ], [HAVE_SOLARIS=0 ; if test "x$solaris" = xyes ; then AC_MSG_ERROR([*** Solaris audio support not found]) ; fi])
else HAVE_SOLARIS=0 ; fi
AC_SUBST(HAVE_SOLARIS)
AM_CONDITIONAL([HAVE_SOLARIS], [test "x$HAVE_SOLARIS" = x1])

#### GLib 2 support (optional) ####

AC_ARG_ENABLE([glib2], 
    AC_HELP_STRING([--disable-glib2], [Disable optional GLib 2 support]), 
    [case "${enableval}" in yes) glib2=yes ;; no) glib2=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-glib2) ;; esac],[glib2=auto])
if test "x${glib2}" != xno ; then
    PKG_CHECK_MODULES(GLIB20, [ glib-2.0 >= 2.4.0 ], HAVE_GLIB20=1, [HAVE_GLIB20=0 ; if test "x$glib2" = xyes ; then AC_MSG_ERROR([*** GLib 2 support not found]) ; fi])
else HAVE_GLIB20=0 ; fi
AC_SUBST(GLIB20_CFLAGS)
AC_SUBST(GLIB20_LIBS)
AC_SUBST(HAVE_GLIB20)
AM_CONDITIONAL([HAVE_GLIB20], [test "x$HAVE_GLIB20" = x1])

#### GLib 1 support (optional) ####

AC_ARG_ENABLE([glib1], 
    AC_HELP_STRING([--disable-glib1], [Disable optional GLib 1 support]), 
    [case "${enableval}" in yes) glib1=yes ;; no) glib1=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-glib1) ;; esac],[glib1=auto])
if test "x${glib1}" != xno ; then
    PKG_CHECK_MODULES(GLIB12, [ glib >= 1.2.0 ], HAVE_GLIB12=1, [HAVE_GLIB12=0 ; if test "x$glib1" = xyes ; then AC_MSG_ERROR([*** GLib 1 support not found]) ; fi])
else HAVE_GLIB12=0 ; fi
AC_SUBST(GLIB12_CFLAGS)
AC_SUBST(GLIB12_LIBS)
AC_SUBST(HAVE_GLIB12)
AM_CONDITIONAL([HAVE_GLIB12], [test "x$HAVE_GLIB12" = x1])

#### Howl support (optional) ####

AC_ARG_ENABLE([howl], 
    AC_HELP_STRING([--disable-howl], [Disable optional Howl support]), 
    [case "${enableval}" in yes) howl=yes ;; no) howl=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-howl) ;; esac],[howl=auto])
if test "x${howl}" != xno ; then
    PKG_CHECK_MODULES(HOWL, [ howl >= 0.9.8 ], HAVE_HOWL=1, PKG_CHECK_MODULES(HOWL, [ avahi-compat-howl >= 0.9.8 ], HAVE_HOWL=1, [HAVE_HOWL=0 ; if test "x$howl" = xyes ; then AC_MSG_ERROR([*** Howl support not found]) ; fi]))
else HAVE_HOWL=0 ; fi
AC_SUBST(HOWL_CFLAGS)
AC_SUBST(HOWL_LIBS)
AC_SUBST(HAVE_HOWL)
AM_CONDITIONAL([HAVE_HOWL], [test "x$HAVE_HOWL" = x1])

### LIBOIL ####

PKG_CHECK_MODULES(LIBOIL, [ liboil-0.3 >= 0.3.0 ])
AC_SUBST(LIBOIL_CFLAGS)
AC_SUBST(LIBOIL_LIBS)

### JACK (optional) ####

AC_ARG_ENABLE([jack], 
    AC_HELP_STRING([--disable-jack], [Disable optional JACK support]), 
    [case "${enableval}" in yes) jack=yes ;; no) jack=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-jack) ;; esac],[jack=auto])
if test "x${jack}" != xno ; then
    PKG_CHECK_MODULES(JACK, [ jack >= 0.100 ], HAVE_JACK=1, [HAVE_JACK=0 ; if test "x$jack" = xyes ; then AC_MSG_ERROR([*** JACK support not found]) ; fi])
else HAVE_JACK=0 ; fi
AC_SUBST(JACK_CFLAGS)
AC_SUBST(JACK_LIBS)
AC_SUBST(HAVE_JACK)
AM_CONDITIONAL([HAVE_JACK], [test "x$HAVE_JACK" = x1])

#### Async DNS support (optional) ####

AC_ARG_ENABLE([asyncns], 
    AC_HELP_STRING([--disable-asyncns], [Disable optional Async DNS support]), 
    [case "${enableval}" in yes) asyncns=yes ;; no) asyncns=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-asyncns) ;; esac],[asyncns=auto])
if test "x${asyncns}" != xno ; then
    PKG_CHECK_MODULES(LIBASYNCNS, [ libasyncns >= 0.1 ], HAVE_LIBASYNCNS=1, [HAVE_LIBASYNCNS=0 ; if test "x$asyncns" = xyes ; then AC_MSG_ERROR([*** Async DNS support not found]) ; fi])
else HAVE_LIBASYNCNS=0 ; fi
AC_SUBST(LIBASYNCNS_CFLAGS)
AC_SUBST(LIBASYNCNS_LIBS)
AC_SUBST(HAVE_LIBASYNCNS)
AM_CONDITIONAL([HAVE_LIBASYNCNS], [test "x$HAVE_LIBASYNCNS" = x1])

if test "x$HAVE_LIBASYNCNS" != "x0" ; then
   AC_DEFINE([HAVE_LIBASYNCNS], 1, [Have libasyncns?])
fi

#### TCP wrappers (optional) ####

AC_ARG_ENABLE([tcpwrap], 
    AC_HELP_STRING([--disable-tcpwrap], [Disable optional TCP wrappers support]), 
    [case "${enableval}" in yes) tcpwrap=yes ;; no) tcpwrap=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-tcpwrap) ;; esac],[tcpwrap=auto])
if test "x${tcpwrap}" != xno ; then
    ACX_LIBWRAP
    if test "x${LIBWRAP_LIBS}" = x && test "x$tcpwrap" = xyes ; then
        AC_MSG_ERROR([*** TCP wrappers support not found])
    fi
else LIBWRAP_LIBS= ; fi
AC_SUBST(LIBWRAP_LIBS)

#### LIRC support (optional) ####

AC_ARG_ENABLE([lirc], 
    AC_HELP_STRING([--disable-lirc], [Disable optional LIRC support]), 
    [case "${enableval}" in yes) lirc=yes ;; no) lirc=no ;; *) AC_MSG_ERROR(bad value ${enableval} for --disable-lirc) ;; esac],[lirc=auto])
if test "x${lirc}" != xno ; then
    ACX_LIRC
    if test "x${HAVE_LIRC}" = x0 && test "x$lirc" = xyes ; then
        AC_MSG_ERROR([*** LIRC support not found])
    fi
else HAVE_LIRC=0 ; fi
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

AC_ARG_WITH(
        [module-dir],
        AC_HELP_STRING([--with-module-dir],[Directory where to install the modules to (defaults to ${libdir}/polypaudio-${PA_MAJORMINOR}/modules/]),
        [modlibexecdir=$withval], [modlibexecdir="${libdir}/polypaudio-${PA_MAJORMINOR}/modules/"])

AC_SUBST(modlibexecdir)

AC_ARG_ENABLE(
        [force-preopen],
        AC_HELP_STRING([--enable-force-preopen],[Preopen modules, even when dlopen() is supported.]),
        [FORCE_PREOPEN=1], [FORCE_PREOPEN=0])
AM_CONDITIONAL([FORCE_PREOPEN], [test "x$FORCE_PREOPEN" = "x1"])

AC_CONFIG_FILES([
Makefile
src/Makefile
polyplib.pc
polyplib-simple.pc
polyplib-browse.pc
polyplib-glib-mainloop.pc
polyplib-glib12-mainloop.pc
doc/Makefile
doc/README.html
doc/cli.html
doc/daemon.html
doc/modules.html
doxygen/Makefile
doxygen/doxygen.conf
src/polyp/version.h
doc/FAQ.html
])
AC_OUTPUT
