# -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# This file is part of PulseAudio.
#
# Copyright 2004-2008 Lennart Poettering
# Copyright 2006-2007 Pierre Ossman <ossman@cendio.se> for Cendio AB
#
# PulseAudio is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# PulseAudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with PulseAudio; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.

AC_PREREQ(2.62)

m4_define(PA_MAJOR, [0])
m4_define(PA_MINOR, [9])
m4_define(PA_MICRO, [14])

AC_INIT([pulseaudio],[PA_MAJOR.PA_MINOR.PA_MICRO],[mzchyfrnhqvb (at) 0pointer (dot) net])
AC_CONFIG_SRCDIR([src/daemon/main.c])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign 1.10 -Wall])

AC_SUBST(PA_MAJORMINOR, "PA_MAJOR.PA_MINOR")
AC_SUBST(PACKAGE_URL, [http://pulseaudio.org/])

AC_SUBST(PA_API_VERSION, 12)
AC_SUBST(PA_PROTOCOL_VERSION, 14)

# The stable ABI for client applications, for the version info x:y:z
# always will hold y=z
AC_SUBST(LIBPULSE_VERSION_INFO, [7:1:7])

# A simplified, synchronous, ABI-stable interface for client
# applications, for the version info x:y:z always will hold y=z
AC_SUBST(LIBPULSE_SIMPLE_VERSION_INFO, [0:2:0])

# The ABI-stable network browsing interface for client applications,
# for the version info x:y:z always will hold y=z
AC_SUBST(LIBPULSE_BROWSE_VERSION_INFO, [1:1:1])

# The ABI-stable GLib adapter for client applications, for the version
# info x:y:z always will hold y=z
AC_SUBST(LIBPULSE_MAINLOOP_GLIB_VERSION_INFO, [0:4:0])

# An internally used, ABI-unstable library that contains the
# PulseAudio core, SONAMEs are bumped on every release, version info
# suffix will always be 0:0
AC_SUBST(LIBPULSECORE_VERSION_INFO, [9:0:0])

AC_CANONICAL_HOST
AC_DEFINE_UNQUOTED([CANONICAL_HOST], "$host", [Canonical host string.])

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

# mkdir -p

AC_PROG_MKDIR_P

# CC

AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_CC_C_O
AC_PROG_GCC_TRADITIONAL
AC_USE_SYSTEM_EXTENSIONS

# M4

AC_PATH_PROG([M4], [m4 gm4], [no])
if test "x$M4" = xno ; then
   AC_MSG_ERROR([m4 missing])
fi

dnl Compiler flags
DESIRED_FLAGS="-Wall -W -Wextra -pedantic -pipe -Wno-long-long -Wvla -Wno-overlength-strings -Wconversion -Wundef -Wformat -Wlogical-op -Wpacked -Wformat-security -Wmissing-include-dirs -Wformat-nonliteral -Wold-style-definition -Wdeclaration-after-statement -Wfloat-equal -Wmissing-declarations -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls -Wmissing-noreturn -Wshadow -Wendif-labels -Wpointer-arith -Wcast-align -Wwrite-strings -Wno-unused-parameter -ffast-math"

for flag in $DESIRED_FLAGS ; do
  CC_CHECK_CFLAGS([$flag], [CFLAGS="$CFLAGS $flag"])
done

# Native atomic operation support
AC_ARG_ENABLE([atomic-arm-linux-helpers],
    AS_HELP_STRING([--disable-atomic-arm-linux-helpers],[use inline asm or libatomic_ops instead]),
        [
            case "${enableval}" in
                yes) atomic_arm_linux_helpers=yes ;;
                no) atomic_arm_linux_helpers=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-atomic-arm-linux-helpers) ;;
            esac
        ],
        [atomic_arm_linux_helpers=auto])

AC_ARG_ENABLE([atomic-arm-memory-barrier],
    AS_HELP_STRING([--enable-atomic-arm-memory-barrier],[only really needed in SMP arm systems]),
        [
            case "${enableval}" in
                yes) AC_DEFINE_UNQUOTED(ATOMIC_ARM_MEMORY_BARRIER_ENABLED, 1, [Enable memory barriers]) ;;
                no) ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-atomic-arm-linux-helpers) ;;
            esac
        ],)

AC_MSG_CHECKING([target operating system])
case $host in
    	*-*-linux*)
	    AC_MSG_RESULT([linux])
	    pulse_target_os=linux
    	;;
	*)
	    AC_MSG_RESULT([unknown])
	    pulse_target_os=unknown
	;;
esac

# If everything else fails use libatomic_ops
need_libatomic_ops=yes

AC_CACHE_CHECK([whether $CC knows __sync_bool_compare_and_swap()],
  pulseaudio_cv_sync_bool_compare_and_swap,
  [AC_LINK_IFELSE(
     AC_LANG_PROGRAM([], [[int a = 4; __sync_bool_compare_and_swap(&a, 4, 5);]]),
     [pulseaudio_cv_sync_bool_compare_and_swap=yes],
     [pulseaudio_cv_sync_bool_compare_and_swap=no])
  ])

if test "$pulseaudio_cv_sync_bool_compare_and_swap" = "yes" ; then
    AC_DEFINE([HAVE_ATOMIC_BUILTINS], 1, [Have __sync_bool_compare_and_swap() and friends.])
    need_libatomic_ops=no
else
    # HW specific atomic ops stuff
    AC_MSG_CHECKING([architecture for native atomic operations])
    case $host_cpu in
        arm*)
	    AC_MSG_RESULT([arm])
	    AC_MSG_CHECKING([whether we can use Linux kernel helpers])
	    # The Linux kernel helper functions have been there since 2.6.16. However
  	    # compile time checking for kernel version in cross compile environment
	    # (which is usually the case for arm cpu) is tricky (or impossible).
	    if test "x$pulse_target_os" = "xlinux" && test "x$atomic_arm_linux_helpers" != "xno"; then
	        AC_MSG_RESULT([yes])
        	AC_DEFINE_UNQUOTED(ATOMIC_ARM_LINUX_HELPERS, 1, [special arm linux implementation])
    		need_libatomic_ops=no
	    else
	       AC_MSG_RESULT([no])
	       AC_CACHE_CHECK([compiler support for arm inline asm atomic operations],
	         pulseaudio_cv_support_arm_atomic_ops,
		 [AC_COMPILE_IFELSE(
		    AC_LANG_PROGRAM([],
		      [[volatile int a=0;
	                int o=0, n=1, r;
	                asm volatile ("ldrex	%0, [%1]\n"
		        	 	 "subs	%0, %0, %2\n"
		        	 	 "strexeq %0, %3, [%1]\n"
		        	 	 : "=&r" (r)
		        	 	 : "r" (&a), "Ir" (o), "r" (n)
		        	 	 : "cc");
                        return (a==1 ? 0 : -1);
		      ]]),
		    [pulseaudio_cv_support_arm_atomic_ops=yes],
		    [pulseaudio_cv_support_arm_atomic_ops=no])
		 ])
	       AS_IF([test "$pulseaudio_cv_support_arm_atomic_ops" = "yes"], [
	       	   AC_DEFINE([ATOMIC_ARM_INLINE_ASM], 1, [Have ARMv6 instructions.])
		   need_libatomic_ops=no
	         ])
	   fi
      	;;
        *)
	    AC_MSG_RESULT([unknown])
        ;;
    esac
fi

CC_CHECK_TLS

AC_CACHE_CHECK([whether $CC knows _Bool],
  pulseaudio_cv__Bool,
  [AC_COMPILE_IFELSE(
     AC_LANG_PROGRAM([], [[_Bool b;]]),
     [pulseaudio_cv__Bool=yes],
     [pulseaudio_cv__Bool=no])
  ])

AS_IF([test "$pulseaudio_cv__Bool" = "yes"], [
    AC_DEFINE([HAVE_STD_BOOL], 1, [Have _Bool.])
  ])

#### libtool stuff ####

AC_LTDL_ENABLE_INSTALL
AC_LIBLTDL_INSTALLABLE
AC_LIBTOOL_DLOPEN
AC_LIBTOOL_WIN32_DLL
AC_PROG_LIBTOOL
AC_SUBST(LTDLINCL)
AC_SUBST(LIBLTDL)
AC_CONFIG_SUBDIRS(libltdl)

old_LIBS=$LIBS
LIBS="$LIBS $LIBLTDL"
AC_CHECK_FUNCS([lt_dlmutex_register])
LIBS=$old_LIBS
AC_CHECK_TYPES([struct lt_user_dlloader, lt_dladvise], , , [#include <ltdl.h>])

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
    netinet/in_systm.h netinet/tcp.h poll.h pwd.h sched.h \
    sys/mman.h sys/resource.h sys/select.h sys/socket.h sys/wait.h \
    syslog.h sys/dl.h dlfcn.h linux/sockios.h])
AC_CHECK_HEADERS([netinet/ip.h], [], [],
		 [#include <sys/types.h>
		  #if HAVE_NETINET_IN_H
		  # include <netinet/in.h>
		  #endif
		  #if HAVE_NETINET_IN_SYSTM_H
		  # include <netinet/in_systm.h>
		  #endif
		 ])
AC_CHECK_HEADERS([regex.h], [HAVE_REGEX=1], [HAVE_REGEX=0])
AC_CHECK_HEADERS([sys/un.h], [HAVE_AF_UNIX=1], [HAVE_AF_UNIX=0])

AM_CONDITIONAL(HAVE_REGEX, test "x$HAVE_REGEX" = "x1")
AM_CONDITIONAL(HAVE_AF_UNIX, test "x$HAVE_AF_UNIX" = "x1")

# Linux
AC_CHECK_HEADERS([linux/input.h], [HAVE_EVDEV=1], [HAVE_EVDEV=0])

AM_CONDITIONAL([HAVE_EVDEV], [test "x$HAVE_EVDEV" = "x1"])

AC_CHECK_HEADERS([sys/prctl.h])

# Solaris
AC_CHECK_HEADERS([sys/filio.h])

# Windows
AC_CHECK_HEADERS([windows.h winsock2.h ws2tcpip.h])

# Other
AC_CHECK_HEADERS([sys/ioctl.h])
AC_CHECK_HEADERS([byteswap.h])
AC_CHECK_HEADERS([sys/syscall.h])
AC_CHECK_HEADERS([sys/eventfd.h])

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
AC_CHECK_DECLS(environ)

AC_CHECK_DEFINE([SIGXCPU], [signal.h], [
HAVE_SIGXCPU=1
AC_DEFINE([HAVE_SIGXCPU], 1, [Have SIGXCPU?])
], [HAVE_SIGXCPU=0])
AM_CONDITIONAL(HAVE_SIGXCPU, test "x$HAVE_SIGXCPU" = "x1")

# Solaris lacks this
AC_CHECK_DEFINE([INADDR_NONE], [netinet/in.h], [],
    [AC_CHECK_DEFINE([INADDR_NONE], [winsock2.h], [],
        [AC_DEFINE([INADDR_NONE],  [0xffffffff], [Define INADDR_NONE if not found in <netinet/in.h>])])])

#### POSIX threads ####

ACX_PTHREAD

#### Check for libs ####

# ISO
AC_SEARCH_LIBS([pow], [m])

# POSIX
AC_SEARCH_LIBS([sched_setscheduler], [rt])
AC_SEARCH_LIBS([dlopen], [dl])
AC_SEARCH_LIBS([shm_open], [rt])
AC_SEARCH_LIBS([inet_ntop], [nsl])
AC_SEARCH_LIBS([timer_create], [rt])

# BSD
AC_SEARCH_LIBS([connect], [socket])

# Non-standard

# This magic is needed so we do not needlessly add static libs to the win32
# build, disabling its ability to make dlls.
AC_CHECK_FUNCS([getopt_long], [], [AC_CHECK_LIB([iberty], [getopt_long])])

AC_CHECK_LIB(gdbm, gdbm_open)
AC_CHECK_HEADERS(gdbm.h, [], [AC_MSG_ERROR([gdbm.h not found])])

#### Check for functions ####

# ISO
AC_CHECK_FUNCS([lrintf strtof])

# POSIX
AC_FUNC_FORK
AC_FUNC_GETGROUPS
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([chmod chown clock_gettime getaddrinfo getgrgid_r \
    getpwuid_r gettimeofday getuid inet_ntop inet_pton mlock nanosleep \
    pipe posix_fadvise posix_madvise posix_memalign setpgid setsid shm_open \
    sigaction sleep sysconf])
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

AC_CHECK_FUNCS([setresuid setresgid setreuid setregid seteuid setegid ppoll strsignal sig2str strtof_l])

AC_FUNC_ALLOCA

AC_CACHE_CHECK([for PTHREAD_PRIO_INHERIT],
  pulseaudio_cv_PTHREAD_PRIO_INHERIT,
  [save_CC=$CC; CC=$PTHREAD_CC
   save_CFLAGS=$CFLAGS; CFLAGS=$PTHREAD_CFLAGS
   save_LIBS=$LIBS; LIBS=$PTHREAD_LIBS
   AC_LINK_IFELSE(
     AC_LANG_PROGRAM(
       [[
         #include <pthread.h>
       ]],
       [[int i = PTHREAD_PRIO_INHERIT;]]),
     [pulseaudio_cv_PTHREAD_PRIO_INHERIT=yes],
     [pulseaudio_cv_PTHREAD_PRIO_INHERIT=no])
   CC=$save_CC
   CFLAGS=$save_CFLAGS
   LIBS=$save_LIBS
  ])

AS_IF([test "$pulseaudio_cv_PTHREAD_PRIO_INHERIT" = "yes"], [
    AC_DEFINE([HAVE_PTHREAD_PRIO_INHERIT], 1, [Have PTHREAD_PRIO_INHERIT.])
  ])

AC_DEFINE_UNQUOTED(PA_CFLAGS,"$CFLAGS", [The CFLAGS used during compilation])

#### Large File-Support (LFS) ####

AC_SYS_LARGEFILE

# Check for open64 to know if the current system does have open64() and similar functions
AC_CHECK_FUNCS([open64])

#### [lib]iconv ####

AM_ICONV

IT_PROG_INTLTOOL([0.35.0])
GETTEXT_PACKAGE=pulseaudio
AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE],["$GETTEXT_PACKAGE"],[Gettext package])
AM_GLIB_GNU_GETTEXT

pulselocaledir='${prefix}/${DATADIRNAME}/locale'
AC_SUBST(pulselocaledir)

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
        AS_HELP_STRING([--without-caps],[Omit support for POSIX capabilities.]))

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

#### Valgrind (optional) ####

AC_CHECK_HEADERS([valgrind/memcheck.h])

#### pkg-config ####

# Check for pkg-config manually first, as if its not installed the
# PKG_PROG_PKG_CONFIG macro won't be defined.
AC_CHECK_PROG(have_pkg_config, pkg-config, yes, no)

if test x"$have_pkg_config" = "xno"; then
    AC_MSG_ERROR(pkg-config is required to install this program)
fi

PKG_PROG_PKG_CONFIG

#### Sound file ####

PKG_CHECK_MODULES(LIBSNDFILE, [ sndfile >= 1.0.10 ])
AC_SUBST(LIBSNDFILE_CFLAGS)
AC_SUBST(LIBSNDFILE_LIBS)

PKG_CHECK_MODULES(LIBSPEEX, [ speexdsp >= 1.2 ])
AC_SUBST(LIBSPEEX_CFLAGS)
AC_SUBST(LIBSPEEX_LIBS)

#### atomic-ops ###

AC_MSG_CHECKING([whether we need libatomic_ops])
if test "x$need_libatomic_ops" = "xyes"; then
   AC_MSG_RESULT([yes])
   AC_CHECK_HEADERS([atomic_ops.h], [], [
   AC_MSG_ERROR([*** libatomic-ops headers not found])
   ])

   # Win32 does not need the lib and breaks horribly if we try to include it
   if test "x$os_is_win32" != "x1" ; then
       LIBS="$LIBS -latomic_ops"
   fi
else
   AC_MSG_RESULT([no])
fi

#### Libsamplerate support (optional) ####

AC_ARG_ENABLE([samplerate],
    AS_HELP_STRING([--disable-samplerate],[Disable optional libsamplerate support]),
        [
            case "${enableval}" in
                yes) samplerate=yes ;;
                no) samplerate=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-samplerate) ;;
            esac
        ],
        [samplerate=auto])

if test "x${samplerate}" != xno ; then
    PKG_CHECK_MODULES(LIBSAMPLERATE, [ samplerate >= 0.1.0 ],
        HAVE_LIBSAMPLERATE=1,
        [
            HAVE_LIBSAMPLERATE=0
            if test "x$samplerate" = xyes ; then
                AC_MSG_ERROR([*** Libsamplerate not found])
            fi
        ])
else
    HAVE_LIBSAMPLERATE=0
fi

if test "x${HAVE_LIBSAMPLERATE}" = x1 ; then
   AC_DEFINE([HAVE_LIBSAMPLERATE], 1, [Have libsamplerate?])
fi

AC_SUBST(LIBSAMPLERATE_CFLAGS)
AC_SUBST(LIBSAMPLERATE_LIBS)
AC_SUBST(HAVE_LIBSAMPLERATE)
AM_CONDITIONAL([HAVE_LIBSAMPLERATE], [test "x$HAVE_LIBSAMPLERATE" = x1])

#### OSS support (optional) ####

AC_ARG_ENABLE([oss],
    AS_HELP_STRING([--disable-oss],[Disable optional OSS support]),
        [
            case "${enableval}" in
                yes) oss=yes ;;
                no) oss=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-oss) ;;
            esac
        ],
        [oss=auto])

if test "x${oss}" != xno ; then
    AC_CHECK_HEADERS([sys/soundcard.h],
        [
            HAVE_OSS=1
            AC_DEFINE([HAVE_OSS], 1, [Have OSS?])
        ],
        [
            HAVE_OSS=0
            if test "x$oss" = xyes ; then
                AC_MSG_ERROR([*** OSS support not found])
            fi
        ])
else
    HAVE_OSS=0
fi

AC_SUBST(HAVE_OSS)
AM_CONDITIONAL([HAVE_OSS], [test "x$HAVE_OSS" = x1])


#### ALSA support (optional) ####

AC_ARG_ENABLE([alsa],
    AS_HELP_STRING([--disable-alsa],[Disable optional ALSA support]),
        [
            case "${enableval}" in
                yes) alsa=yes ;;
                no) alsa=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-alsa) ;;
            esac
        ],
        [alsa=auto])

if test "x${alsa}" != xno ; then
    PKG_CHECK_MODULES(ASOUNDLIB, [ alsa >= 1.0.17 ],
        [
            HAVE_ALSA=1
            AC_DEFINE([HAVE_ALSA], 1, [Have ALSA?])
        ],
        [
            HAVE_ALSA=0
            if test "x$alsa" = xyes ; then
                AC_MSG_ERROR([*** Needed alsa >= 1.0.17 support not found])
            fi
        ])
else
    HAVE_ALSA=0
fi

AC_SUBST(ASOUNDLIB_CFLAGS)
AC_SUBST(ASOUNDLIB_LIBS)
AC_SUBST(HAVE_ALSA)
AM_CONDITIONAL([HAVE_ALSA], [test "x$HAVE_ALSA" = x1])

#### Solaris audio support (optional) ####

AC_ARG_ENABLE([solaris],
    AS_HELP_STRING([--disable-solaris],[Disable optional Solaris audio support]),
        [
            case "${enableval}" in
                yes) solaris=yes ;;
                no) solaris=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-solaris) ;;
            esac
        ],
        [solaris=auto])

if test "x${solaris}" != xno ; then
    AC_CHECK_HEADERS([sys/audio.h],
        [
            HAVE_SOLARIS=1
            AC_DEFINE([HAVE_SOLARIS], 1, [Have Solaris audio?])
        ],
        [
            HAVE_SOLARIS=0
            if test "x$solaris" = xyes ; then
                AC_MSG_ERROR([*** Solaris audio support not found])
            fi
        ])
else
    HAVE_SOLARIS=0
fi

AC_SUBST(HAVE_SOLARIS)
AM_CONDITIONAL([HAVE_SOLARIS], [test "x$HAVE_SOLARIS" = x1])

#### GLib 2 support (optional) ####

AC_ARG_ENABLE([glib2],
    AS_HELP_STRING([--disable-glib2],[Disable optional GLib 2 support]),
        [
            case "${enableval}" in
                yes) glib2=yes ;;
                no) glib2=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-glib2) ;;
            esac
        ],
        [glib2=auto])

if test "x${glib2}" != xno ; then
    PKG_CHECK_MODULES(GLIB20, [ glib-2.0 >= 2.4.0 ],
        HAVE_GLIB20=1,
        [
            HAVE_GLIB20=0
            if test "x$glib2" = xyes ; then
                AC_MSG_ERROR([*** GLib 2 support not found])
            fi
        ])
else
    HAVE_GLIB20=0
fi

AC_SUBST(GLIB20_CFLAGS)
AC_SUBST(GLIB20_LIBS)
AC_SUBST(HAVE_GLIB20)
AM_CONDITIONAL([HAVE_GLIB20], [test "x$HAVE_GLIB20" = x1])

#### GConf support (optional) ####

AC_ARG_ENABLE([gconf],
    AS_HELP_STRING([--disable-gconf],[Disable optional GConf support]),
        [
            case "${enableval}" in
                yes) gconf=yes ;;
                no) gconf=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-gconf) ;;
            esac
        ],
        [gconf=auto])

if test "x${gconf}" != xno ; then
    PKG_CHECK_MODULES(GCONF, [ gconf-2.0 >= 2.4.0 ],
        HAVE_GCONF=1,
        [
            HAVE_GCONF=0
            if test "x$gconf" = xyes ; then
                AC_MSG_ERROR([*** GConf support not found])
            fi
        ])
else
    HAVE_GCONF=0
fi

AC_SUBST(GCONF_CFLAGS)
AC_SUBST(GCONF_LIBS)
AC_SUBST(HAVE_GCONF)
AM_CONDITIONAL([HAVE_GCONF], [test "x$HAVE_GCONF" = x1])

#### Avahi support (optional) ####

AC_ARG_ENABLE([avahi],
    AS_HELP_STRING([--disable-avahi],[Disable optional Avahi support]),
        [
            case "${enableval}" in
                yes) avahi=yes ;;
                no) avahi=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-avahi) ;;
            esac
        ],
        [avahi=auto])

if test "x${avahi}" != xno ; then
    PKG_CHECK_MODULES(AVAHI, [ avahi-client >= 0.6.0 ],
        HAVE_AVAHI=1,
        [
                HAVE_AVAHI=0
                if test "x$avahi" = xyes ; then
                        AC_MSG_ERROR([*** Avahi support not found])
                fi
        ])
else
    HAVE_AVAHI=0
fi

AC_SUBST(AVAHI_CFLAGS)
AC_SUBST(AVAHI_LIBS)
AC_SUBST(HAVE_AVAHI)
AM_CONDITIONAL([HAVE_AVAHI], [test "x$HAVE_AVAHI" = x1])

### LIBOIL ####

PKG_CHECK_MODULES(LIBOIL, [ liboil-0.3 >= 0.3.0 ])
AC_SUBST(LIBOIL_CFLAGS)
AC_SUBST(LIBOIL_LIBS)

### JACK (optional) ####

AC_ARG_ENABLE([jack],
    AS_HELP_STRING([--disable-jack],[Disable optional JACK support]),
        [
            case "${enableval}" in
                yes) jack=yes ;;
                no) jack=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-jack) ;;
            esac
        ],
        [jack=auto])

if test "x${jack}" != xno ; then
    PKG_CHECK_MODULES(JACK, [ jack >= 0.100 ],
        HAVE_JACK=1,
        [
            HAVE_JACK=0
            if test "x$jack" = xyes ; then
                AC_MSG_ERROR([*** JACK support not found])
            fi
        ])
else
    HAVE_JACK=0
fi

AC_SUBST(JACK_CFLAGS)
AC_SUBST(JACK_LIBS)
AC_SUBST(HAVE_JACK)
AM_CONDITIONAL([HAVE_JACK], [test "x$HAVE_JACK" = x1])

#### Async DNS support (optional) ####

AC_ARG_ENABLE([asyncns],
    AS_HELP_STRING([--disable-asyncns],[Disable optional Async DNS support]),
        [
            case "${enableval}" in
                yes) asyncns=yes ;;
                no) asyncns=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-asyncns) ;;
            esac
        ],
        [asyncns=auto])

if test "x${asyncns}" != xno ; then
    PKG_CHECK_MODULES(LIBASYNCNS, [ libasyncns >= 0.1 ],
        HAVE_LIBASYNCNS=1,
        [
            HAVE_LIBASYNCNS=0
            if test "x$asyncns" = xyes ; then
                AC_MSG_ERROR([*** Async DNS support not found])
            fi
        ])
else
    HAVE_LIBASYNCNS=0
fi

AC_SUBST(LIBASYNCNS_CFLAGS)
AC_SUBST(LIBASYNCNS_LIBS)
AC_SUBST(HAVE_LIBASYNCNS)
AM_CONDITIONAL([HAVE_LIBASYNCNS], [test "x$HAVE_LIBASYNCNS" = x1])

if test "x$HAVE_LIBASYNCNS" != "x0" ; then
   AC_DEFINE([HAVE_LIBASYNCNS], 1, [Have libasyncns?])
fi

#### TCP wrappers (optional) ####

AC_ARG_ENABLE([tcpwrap],
    AS_HELP_STRING([--disable-tcpwrap],[Disable optional TCP wrappers support]),
        [
            case "${enableval}" in
                yes) tcpwrap=yes ;;
                no) tcpwrap=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-tcpwrap) ;;
            esac
        ],
        [tcpwrap=auto])

if test "x${tcpwrap}" != xno ; then
    ACX_LIBWRAP
    if test "x${LIBWRAP_LIBS}" = x && test "x$tcpwrap" = xyes ; then
        AC_MSG_ERROR([*** TCP wrappers support not found])
    fi
else
    LIBWRAP_LIBS=
fi

AC_SUBST(LIBWRAP_LIBS)

#### LIRC support (optional) ####

AC_ARG_ENABLE([lirc],
    AS_HELP_STRING([--disable-lirc],[Disable optional LIRC support]),
        [
            case "${enableval}" in
                yes) lirc=yes ;;
                no) lirc=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-lirc) ;;
            esac
        ],
        [lirc=auto])

if test "x${lirc}" != xno ; then
    ACX_LIRC
    if test "x${HAVE_LIRC}" = x0 && test "x$lirc" = xyes ; then
        AC_MSG_ERROR([*** LIRC support not found])
    fi
else
    HAVE_LIRC=0
fi

AC_SUBST(LIRC_CFLAGS)
AC_SUBST(LIRC_LIBS)
AM_CONDITIONAL([HAVE_LIRC], [test "x$HAVE_LIRC" = x1])

#### HAL support (optional) ####

AC_ARG_ENABLE([hal],
    AS_HELP_STRING([--disable-hal],[Disable optional HAL support]),
        [
            case "${enableval}" in
                yes) hal=yes ;;
                no) hal=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-hal) ;;
            esac
        ],
        [hal=auto])
if test "x${hal}" != xno -a \( "x$HAVE_OSS" = "x1" -o "x$HAVE_ALSA" = "x1" \) ; then
    PKG_CHECK_MODULES(HAL, [ hal >= 0.5.7 ],
        HAVE_HAL=1,
        [
            HAVE_HAL=0
            if test "x$hal" = xyes ; then
                AC_MSG_ERROR([*** HAL support not found])
            fi
        ])
else
    HAVE_HAL=0
fi

AC_SUBST(HAL_CFLAGS)
AC_SUBST(HAL_LIBS)
AC_SUBST(HAVE_HAL)
AM_CONDITIONAL([HAVE_HAL], [test "x$HAVE_HAL" = x1])

#### BlueZ support (optional) ####

AC_ARG_ENABLE([bluez],
    AS_HELP_STRING([--disable-bluez],[Disable optional BlueZ support]),
        [
            case "${enableval}" in
                yes) bluez=yes ;;
                no) bluez=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-bluez) ;;
            esac
        ],
        [bluez=auto])
if test "x${bluez}" != xno ; then
    PKG_CHECK_MODULES(BLUEZ, [ bluez >= 3.0 ],
        HAVE_BLUEZ=1,
        [
            HAVE_BLUEZ=0
            if test "x$bluez" = xyes ; then
                AC_MSG_ERROR([*** BLUEZ support not found])
            fi
        ])
else
    HAVE_BLUEZ=0
fi

AC_SUBST(BLUEZ_CFLAGS)
AC_SUBST(BLUEZ_LIBS)
AC_SUBST(HAVE_BLUEZ)
AM_CONDITIONAL([HAVE_BLUEZ], [test "x$HAVE_BLUEZ" = x1])

#### D-Bus support (optional) ####

AC_ARG_ENABLE([dbus],
    AS_HELP_STRING([--disable-dbus],[Disable optional D-Bus support]),
        [
            case "${enableval}" in
                yes) dbus=yes ;;
                no) dbus=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-dbus) ;;
            esac
        ],
        [dbus=auto])

if test "x$HAVE_HAL" = x1 ; then
   dbus=yes
fi

if test "x${dbus}" != xno || test "x${bluez}" != xno || "x${hal}" != xno ; then

    PKG_CHECK_MODULES(DBUS, [ dbus-1 >= 1.0.0 ],
        [
            HAVE_DBUS=1
            saved_LIBS="$LIBS"
            LIBS="$LIBS $DBUS_LIBS"
            AC_CHECK_FUNCS(dbus_watch_get_unix_fd)
            LIBS="$saved_LIBS"
            AC_DEFINE([HAVE_DBUS], 1, [Have D-Bus.])
        ],
        [
            HAVE_DBUS=0
            if test "x$dbus" = xyes ; then
                AC_MSG_ERROR([*** D-Bus support not found])
            fi
        ])
else
    HAVE_DBUS=0
fi

AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)
AC_SUBST(HAVE_DBUS)
AM_CONDITIONAL([HAVE_DBUS], [test "x$HAVE_DBUS" = x1])

#### PolicyKit support (optional) ####

AC_ARG_ENABLE([polkit],
    AS_HELP_STRING([--disable-polkit],[Disable optional PolicyKit support]),
        [
            case "${enableval}" in
                yes) polkit=yes ;;
                no) polkit=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-polkit) ;;
            esac
        ],
        [polkit=auto])

if test "x${polkit}" != xno ; then

    PKG_CHECK_MODULES(POLKIT, [ polkit-dbus >= 0.7 ],
        [
            HAVE_POLKIT=1
            AC_DEFINE([HAVE_POLKIT], 1, [Have PolicyKit])
            policydir=`pkg-config polkit-dbus --variable prefix`/share/PolicyKit/policy/
            AC_SUBST(policydir)
        ],
        [
            HAVE_POLKIT=0
            if test "x$polkit" = xyes ; then
                AC_MSG_ERROR([*** PolicyKit support not found])
            fi
        ])
else
    HAVE_POLKIT=0
fi

AC_SUBST(POLKIT_CFLAGS)
AC_SUBST(POLKIT_LIBS)
AC_SUBST(HAVE_POLKIT)
AM_CONDITIONAL([HAVE_POLKIT], [test "x$HAVE_POLKIT" = x1])

### Build and Install man pages ###
AC_ARG_ENABLE(manpages,
        AS_HELP_STRING([--disable-manpages],[Disable building and installation of man pages]),
[case "${enableval}" in
  yes) manpages=yes ;;
  no)  manpages=no ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --disable-manpages]) ;;
esac],[manpages=yes])

AM_CONDITIONAL([BUILD_MANPAGES], [test "x$manpages" = xyes])

#### PulseAudio system group & user  #####

AC_ARG_WITH(system_user, AS_HELP_STRING([--with-system-user=<user>],[User for running the PulseAudio daemon as a system-wide instance (pulse)]))
if test -z "$with_system_user" ; then
    PA_SYSTEM_USER=pulse
else
    PA_SYSTEM_USER=$with_system_user
fi
AC_SUBST(PA_SYSTEM_USER)
AC_DEFINE_UNQUOTED(PA_SYSTEM_USER,"$PA_SYSTEM_USER", [User for running the PulseAudio system daemon])

AC_ARG_WITH(system_group,AS_HELP_STRING([--with-system-group=<group>],[Group for running the PulseAudio daemon as a system-wide instance (pulse)]))
if test -z "$with_system_group" ; then
    PA_SYSTEM_GROUP=pulse
else
    PA_SYSTEM_GROUP=$with_system_group
fi
AC_SUBST(PA_SYSTEM_GROUP)
AC_DEFINE_UNQUOTED(PA_SYSTEM_GROUP,"$PA_SYSTEM_GROUP", [Group for the PulseAudio system daemon])

AC_ARG_WITH(realtime_group,AS_HELP_STRING([--with-realtime-group=<group>],[Group for users that are allowed to start the PulseAudio daemon with realtime scheduling (realtime)]))
if test -z "$with_realtime_group" ; then
    PA_REALTIME_GROUP=pulse-rt
else
    PA_REALTIME_GROUP=$with_realtime_group
fi
AC_SUBST(PA_REALTIME_GROUP)
AC_DEFINE_UNQUOTED(PA_REALTIME_GROUP,"$PA_REALTIME_GROUP", [Realtime group])

AC_ARG_WITH(access_group,AS_HELP_STRING([--with-access-group=<group>],[Group which is allowed access to a system-wide PulseAudio daemon (pulse-access)]))
if test -z "$with_access_group" ; then
    PA_ACCESS_GROUP=pulse-access
else
    PA_ACCESS_GROUP=$with_access_group
fi
AC_SUBST(PA_ACCESS_GROUP)
AC_DEFINE_UNQUOTED(PA_ACCESS_GROUP,"$PA_ACCESS_GROUP", [Access group])

AC_ARG_ENABLE(
        per_user_esound_socket,
        AS_HELP_STRING([--disable-per-user-esound-socket], [Use global esound socket directory /tmp/.esd/socket.]),
        [
            case "${enableval}" in
                yes) per_user_esound_socket=1 ;;
                no) per_user_esound_socket=0 ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-per-user-esound-socket) ;;
            esac
        ],
        [per_user_esound_socket=1])

if test "x$per_user_esound_socket" = "x1"; then
   AC_DEFINE([USE_PER_USER_ESOUND_SOCKET], [1], [Define this if you want per-user esound socket directories])
fi

#### PulseAudio system runtime dir ####
PA_SYSTEM_RUNTIME_PATH="${localstatedir}/run/pulse"
AC_SUBST(PA_SYSTEM_RUNTIME_PATH)
PA_SYSTEM_CONFIG_PATH="${localstatedir}/lib/pulse"
AC_SUBST(PA_SYSTEM_CONFIG_PATH)
PA_SYSTEM_STATE_PATH="${localstatedir}/lib/pulse"
AC_SUBST(PA_SYSTEM_STATE_PATH)

###################################
#            Output               #
###################################

AC_ARG_ENABLE(
        [static-bins],
        AS_HELP_STRING([--enable-static-bins],[Statically link executables.]),
        [STATIC_BINS=1], [STATIC_BINS=0])
AM_CONDITIONAL([STATIC_BINS], [test "x$STATIC_BINS" = "x1"])

AC_ARG_WITH(
        [preopen-mods],
        AS_HELP_STRING([--with-preopen-mods],[Modules to preopen in daemon (default: all).]),
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
        AS_HELP_STRING([--with-module-dir],[Directory where to install the modules to (defaults to ${libdir}/pulse-${PA_MAJORMINOR}/modules/]),
        [modlibexecdir=$withval], [modlibexecdir="${libdir}/pulse-${PA_MAJORMINOR}/modules/"])

AC_SUBST(modlibexecdir)

AC_ARG_ENABLE(
        [force-preopen],
        AS_HELP_STRING([--enable-force-preopen],[Preopen modules, even when dlopen() is supported.]),
        [FORCE_PREOPEN=1], [FORCE_PREOPEN=0])
AM_CONDITIONAL([FORCE_PREOPEN], [test "x$FORCE_PREOPEN" = "x1"])

AC_CONFIG_FILES([
Makefile
src/Makefile
man/Makefile
libpulse.pc
libpulse-simple.pc
libpulse-browse.pc
libpulse-mainloop-glib.pc
doxygen/Makefile
doxygen/doxygen.conf
src/pulse/version.h
po/Makefile.in
])
AC_OUTPUT

# ==========================================================================
ENABLE_X11=no
if test "x$HAVE_X11" = "x1" ; then
   ENABLE_X11=yes
fi

ENABLE_OSS=no
if test "x$HAVE_OSS" = "x1" ; then
   ENABLE_OSS=yes
fi

ENABLE_ALSA=no
if test "x$HAVE_ALSA" = "x1" ; then
   ENABLE_ALSA=yes
fi

ENABLE_SOLARIS=no
if test "x$HAVE_SOLARIS" = "x1" ; then
   ENABLE_SOLARIS=yes
fi

ENABLE_GLIB20=no
if test "x$HAVE_GLIB20" = "x1" ; then
   ENABLE_GLIB20=yes
fi

ENABLE_GCONF=no
if test "x$HAVE_GCONF" = "x1" ; then
   ENABLE_GCONF=yes
fi

ENABLE_AVAHI=no
if test "x$HAVE_AVAHI" = "x1" ; then
   ENABLE_AVAHI=yes
fi

ENABLE_JACK=no
if test "x$HAVE_JACK" = "x1" ; then
   ENABLE_JACK=yes
fi

ENABLE_LIBASYNCNS=no
if test "x$HAVE_LIBASYNCNS" = "x1" ; then
   ENABLE_LIBASYNCNS=yes
fi

ENABLE_LIRC=no
if test "x$HAVE_LIRC" = "x1" ; then
   ENABLE_LIRC=yes
fi

ENABLE_HAL=no
if test "x$HAVE_HAL" = "x1" ; then
   ENABLE_HAL=yes
fi

ENABLE_TCPWRAP=no
if test "x${LIBWRAP_LIBS}" != x ; then
   ENABLE_TCPWRAP=yes
fi

ENABLE_LIBSAMPLERATE=no
if test "x${HAVE_LIBSAMPLERATE}" = "x1" ; then
   ENABLE_LIBSAMPLERATE=yes
fi

ENABLE_BLUEZ=no
if test "x${HAVE_BLUEZ}" = "x1" ; then
   ENABLE_BLUEZ=yes
fi

ENABLE_POLKIT=no
if test "x${HAVE_POLKIT}" = "x1" ; then
   ENABLE_POLKIT=yes
fi

ENABLE_PER_USER_ESOUND_SOCKET=no
if test "x$per_user_esound_socket" = "x1" ; then
   ENABLE_PER_USER_ESOUND_SOCKET=yes
fi

echo "
 ---{ $PACKAGE_NAME $VERSION }---

    prefix:                        ${prefix}
    sysconfdir:                    ${sysconfdir}
    localstatedir:                 ${localstatedir}
    System Runtime Path:           ${PA_SYSTEM_RUNTIME_PATH}
    System State Path:             ${PA_SYSTEM_STATE_PATH}
    System Config Path:            ${PA_SYSTEM_CONFIG_PATH}
    Compiler:                      ${CC}
    CFLAGS:                        ${CFLAGS}
    Have X11:                      ${ENABLE_X11}
    Enable OSS:                    ${ENABLE_OSS}
    Enable Alsa:                   ${ENABLE_ALSA}
    Enable Solaris:                ${ENABLE_SOLARIS}
    Enable GLib 2.0:               ${ENABLE_GLIB20}
    Enable GConf:                  ${ENABLE_GCONF}
    Enable Avahi:                  ${ENABLE_AVAHI}
    Enable Jack:                   ${ENABLE_JACK}
    Enable Async DNS:              ${ENABLE_LIBASYNCNS}
    Enable LIRC:                   ${ENABLE_LIRC}
    Enable HAL:                    ${ENABLE_HAL}
    Enable BlueZ:                  ${ENABLE_BLUEZ}
    Enable TCP Wrappers:           ${ENABLE_TCPWRAP}
    Enable libsamplerate:          ${ENABLE_LIBSAMPLERATE}
    Enable PolicyKit:              ${ENABLE_POLKIT}
    System User:                   ${PA_SYSTEM_USER}
    System Group:                  ${PA_SYSTEM_GROUP}
    Realtime Group:                ${PA_REALTIME_GROUP}
    Access Group:                  ${PA_ACCESS_GROUP}
    Enable per-user EsounD socket: ${ENABLE_PER_USER_ESOUND_SOCKET}
"
