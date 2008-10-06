# This file is part of PulseAudio.
#
# Copyright 2004-2006 Lennart Poettering
# Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB
# Copyright 2006 Diego Pettenò
#
# PulseAudio is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# PulseAudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with PulseAudio; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
# USA.


###################################
#       Extra directories         #
###################################

pulseincludedir=$(includedir)/pulse
pulsecoreincludedir=$(includedir)/pulsecore
pulseconfdir=$(sysconfdir)/pulse
pulselibexecdir=$(libexecdir)/pulse
xdgautostartdir=$(sysconfdir)/xdg/autostart

###################################
#            Defines              #
###################################

PA_BINARY=$(bindir)/pulseaudio$(EXEEXT)
if OS_IS_WIN32
PA_DEFAULT_CONFIG_DIR=%PULSE_ROOT%
else
PA_DEFAULT_CONFIG_DIR=$(pulseconfdir)
endif

###################################
#     Compiler/linker flags       #
###################################

AM_CFLAGS = -I$(top_srcdir)/src -I$(top_builddir)/src/modules -I$(top_builddir)/src/modules/rtp -I$(top_builddir)/src/modules/gconf -I$(top_builddir)/src/modules/bluetooth
AM_CFLAGS += $(PTHREAD_CFLAGS) -D_POSIX_PTHREAD_SEMANTICS
AM_CFLAGS += $(LTDLINCL)
AM_CFLAGS += $(LIBSAMPLERATE_CFLAGS) $(LIBSNDFILE_CFLAGS) $(LIBSPEEX_CFLAGS)
AM_CFLAGS += -DPA_DLSEARCHPATH=\"$(modlibexecdir)\"
AM_CFLAGS += -DPA_DEFAULT_CONFIG_DIR=\"$(PA_DEFAULT_CONFIG_DIR)\"
AM_CFLAGS += -DPA_BINARY=\"$(PA_BINARY)\"
AM_CFLAGS += -DPA_SYSTEM_RUNTIME_PATH=\"$(PA_SYSTEM_RUNTIME_PATH)\"
AM_CFLAGS += -DPA_SYSTEM_CONFIG_PATH=\"$(PA_SYSTEM_CONFIG_PATH)\"
AM_CFLAGS += -DPA_SYSTEM_STATE_PATH=\"$(PA_SYSTEM_STATE_PATH)\"
AM_CFLAGS += -DAO_REQUIRE_CAS
AM_CFLAGS += -DPULSE_LOCALEDIR=\"$(pulselocaledir)\"
AM_CFLAGS += -DPA_MACHINE_ID=\"$(localstatedir)/lib/dbus/machine-id\"

# This cool debug trap works on i386/gcc only
AM_CFLAGS += '-DDEBUG_TRAP=__asm__("int $$3")'

AM_LIBADD = $(PTHREAD_LIBS) $(INTLLIBS)
AM_LDADD = $(PTHREAD_LIBS) $(INTLLIBS)

# Only required on some platforms but defined for all to avoid errors
AM_LDFLAGS = -Wl,-no-undefined -Wl,--gc-sections

if STATIC_BINS
BINLDFLAGS = -static
endif

if OS_IS_WIN32
AM_LDFLAGS+=-Wl,--export-all-symbols
WINSOCK_LIBS=-lwsock32 -lws2_32 -lwininet
endif

if OS_IS_WIN32
PA_THREAD_OBJS = \
		pulsecore/mutex-win32.c pulsecore/mutex.h \
		pulsecore/thread-win32.c pulsecore/thread.h \
		pulsecore/semaphore-win32.c pulsecore/semaphore.h
else
PA_THREAD_OBJS = \
		pulsecore/mutex-posix.c pulsecore/mutex.h \
		pulsecore/thread-posix.c pulsecore/thread.h \
		pulsecore/semaphore-posix.c pulsecore/semaphore.h
endif

###################################
#          Extra files            #
###################################

EXTRA_DIST = \
		pulse/client.conf.in \
		pulse/version.h.in \
		daemon/daemon.conf.in \
		daemon/default.pa.in \
		daemon/system.pa.in \
		daemon/default.pa.win32 \
		depmod.py \
		daemon/esdcompat.in \
		daemon/start-pulseaudio-x11.in \
		utils/padsp \
		modules/module-defs.h.m4 \
		daemon/pulseaudio.desktop \
		map-file \
		daemon/org.pulseaudio.policy

pulseconf_DATA = \
		default.pa \
		system.pa \
		daemon.conf \
		client.conf

if HAVE_X11
xdgautostart_DATA = \
		daemon/pulseaudio.desktop
endif

BUILT_SOURCES = \
		pulse/version.h

###################################
#          Main daemon            #
###################################

bin_PROGRAMS = pulseaudio

pulseaudio_SOURCES = \
		daemon/caps.h daemon/caps.c \
		daemon/cmdline.c daemon/cmdline.h \
		daemon/cpulimit.c daemon/cpulimit.h \
		daemon/daemon-conf.c daemon/daemon-conf.h \
		daemon/dumpmodules.c daemon/dumpmodules.h \
		daemon/ltdl-bind-now.c daemon/ltdl-bind-now.h \
		daemon/main.c

pulseaudio_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS) $(LIBSAMPLERATE_CFLAGS) $(LIBSPEEX_CFLAGS) $(LIBSNDFILE_CFLAGS) $(CAP_CFLAGS) $(LIBOIL_CFLAGS) $(DBUS_CFLAGS)
pulseaudio_CPPFLAGS = $(AM_CPPFLAGS)
pulseaudio_LDADD = $(AM_LDADD) libpulsecore.la $(LIBLTDL) $(LIBSAMPLERATE_LIBS) $(LIBSPEEX_LIBS) $(LIBSNDFILE_LIBS) $(CAP_LIBS) $(LIBOIL_LIBS) $(DBUS_LIBS)
# This is needed because automake doesn't properly expand the foreach below
pulseaudio_DEPENDENCIES = libpulsecore.la $(PREOPEN_LIBS)

if PREOPEN_MODS
PREOPEN_LIBS = $(PREOPEN_MODS)
else
PREOPEN_LIBS = $(modlibexec_LTLIBRARIES)
endif

if FORCE_PREOPEN
pulseaudio_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) -dlpreopen force $(foreach f,$(PREOPEN_LIBS),-dlpreopen $(f))
else
pulseaudio_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) -dlopen force $(foreach f,$(PREOPEN_LIBS),-dlopen $(f))
endif

if HAVE_POLKIT

policy_DATA = daemon/org.pulseaudio.policy

pulseaudio_SOURCES += daemon/polkit.c daemon/polkit.h
pulseaudio_CFLAGS += $(POLKIT_CFLAGS)
pulseaudio_LDADD += $(POLKIT_LIBS)


endif

###################################
#       Utility programs          #
###################################

bin_PROGRAMS += \
		pacat \
		pactl \
		paplay \
		pasuspender

if HAVE_AF_UNIX
bin_PROGRAMS += pacmd
endif

if HAVE_X11
bin_PROGRAMS += pax11publish
endif

if HAVE_AVAHI
bin_PROGRAMS += pabrowse
endif

bin_SCRIPTS = esdcompat start-pulseaudio-x11

pacat_SOURCES = utils/pacat.c
pacat_LDADD = $(AM_LDADD) libpulse.la
pacat_CFLAGS = $(AM_CFLAGS)
pacat_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

paplay_SOURCES = utils/paplay.c
paplay_LDADD = $(AM_LDADD) libpulse.la $(LIBSNDFILE_LIBS)
paplay_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)
paplay_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pactl_SOURCES = utils/pactl.c pulsecore/core-util.c pulsecore/core-util.h pulsecore/core-error.c pulsecore/core-error.h pulsecore/log.c pulsecore/log.h pulsecore/rtclock.c pulsecore/rtclock.h pulsecore/once.c pulsecore/once.h $(PA_THREAD_OBJS)
pactl_LDADD = $(AM_LDADD) libpulse.la $(LIBSNDFILE_LIBS)
pactl_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)
pactl_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pasuspender_SOURCES = utils/pasuspender.c pulsecore/core-util.c pulsecore/core-util.h pulsecore/core-error.c pulsecore/core-error.h pulsecore/log.c pulsecore/log.h pulsecore/rtclock.c pulsecore/rtclock.h pulsecore/once.c pulsecore/once.h $(PA_THREAD_OBJS)
pasuspender_LDADD = $(AM_LDADD) libpulse.la $(LIBSNDFILE_LIBS)
pasuspender_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)
pasuspender_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pacmd_SOURCES = utils/pacmd.c pulsecore/pid.c pulsecore/pid.h pulsecore/core-util.c pulsecore/core-util.h pulsecore/core-error.c pulsecore/core-error.h pulsecore/log.c pulsecore/log.h pulsecore/rtclock.c pulsecore/rtclock.h pulsecore/once.c pulsecore/once.h $(PA_THREAD_OBJS)
pacmd_CFLAGS = $(AM_CFLAGS)
pacmd_LDADD = $(AM_LDADD) libpulse.la
pacmd_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pax11publish_SOURCES = utils/pax11publish.c pulsecore/x11prop.c pulsecore/x11prop.h pulse/client-conf.c pulse/client-conf.h pulsecore/authkey.h pulsecore/authkey.c pulsecore/random.h pulsecore/random.c pulsecore/conf-parser.c pulsecore/rtclock.c pulsecore/rtclock.h pulsecore/conf-parser.h pulsecore/core-util.c pulsecore/core-util.h pulsecore/core-error.c pulsecore/core-error.h  pulsecore/log.c pulsecore/log.h pulsecore/once.c pulsecore/once.h $(PA_THREAD_OBJS)
pax11publish_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
pax11publish_LDADD = $(AM_LDADD) libpulse.la $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)
pax11publish_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pabrowse_SOURCES = utils/pabrowse.c
pabrowse_LDADD = $(AM_LDADD) libpulse.la libpulse-browse.la
pabrowse_CFLAGS = $(AM_CFLAGS)
pabrowse_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

###################################
#         Test programs           #
###################################

noinst_PROGRAMS = \
		mainloop-test \
		mcalign-test \
		pacat-simple \
		parec-simple \
		strlist-test \
		close-test \
		voltest \
		memblockq-test \
		sync-playback \
		interpol-test \
		channelmap-test \
		thread-mainloop-test \
		utf8-test \
		get-binary-name-test \
		ipacl-test \
		hook-list-test \
		memblock-test \
		thread-test \
		flist-test \
		asyncq-test \
		asyncmsgq-test \
		queue-test \
		rtpoll-test \
		sig2str-test \
		resampler-test \
		smoother-test \
		mix-test \
		remix-test \
		envelope-test \
		proplist-test \
		rtstutter \
		stripnul \
		lock-autospawn-test \
		prioq-test

if HAVE_SIGXCPU
noinst_PROGRAMS += \
		cpulimit-test \
		cpulimit-test2
endif

if HAVE_GLIB20
noinst_PROGRAMS += \
		mainloop-test-glib
endif

mainloop_test_SOURCES = tests/mainloop-test.c
mainloop_test_CFLAGS = $(AM_CFLAGS)
mainloop_test_LDADD = $(AM_LDADD) libpulse.la
mainloop_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

thread_mainloop_test_SOURCES = tests/thread-mainloop-test.c
thread_mainloop_test_CFLAGS = $(AM_CFLAGS)
thread_mainloop_test_LDADD = $(AM_LDADD) libpulsecore.la libpulse.la
thread_mainloop_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

utf8_test_SOURCES = tests/utf8-test.c
utf8_test_CFLAGS = $(AM_CFLAGS)
utf8_test_LDADD = $(AM_LDADD) libpulsecore.la
utf8_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

get_binary_name_test_SOURCES = tests/get-binary-name-test.c
get_binary_name_test_CFLAGS = $(AM_CFLAGS)
get_binary_name_test_LDADD = $(AM_LDADD) libpulse.la
get_binary_name_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

ipacl_test_SOURCES = tests/ipacl-test.c \
		pulsecore/ipacl.c pulsecore/ipacl.h \
		pulsecore/inet_pton.c pulsecore/inet_pton.h
ipacl_test_CFLAGS = $(AM_CFLAGS)
ipacl_test_LDADD = $(AM_LDADD) libpulsecore.la
ipacl_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

hook_list_test_SOURCES = tests/hook-list-test.c
hook_list_test_CFLAGS = $(AM_CFLAGS)
hook_list_test_LDADD = $(AM_LDADD) libpulsecore.la
hook_list_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

memblock_test_SOURCES = tests/memblock-test.c
memblock_test_CFLAGS = $(AM_CFLAGS)
memblock_test_LDADD = $(AM_LDADD) libpulsecore.la
memblock_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

thread_test_SOURCES = tests/thread-test.c
thread_test_CFLAGS = $(AM_CFLAGS)
thread_test_LDADD = $(AM_LDADD) libpulsecore.la
thread_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

flist_test_SOURCES = tests/flist-test.c
flist_test_CFLAGS = $(AM_CFLAGS)
flist_test_LDADD = $(AM_LDADD) libpulsecore.la
flist_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

asyncq_test_SOURCES = tests/asyncq-test.c
asyncq_test_CFLAGS = $(AM_CFLAGS)
asyncq_test_LDADD = $(AM_LDADD) libpulsecore.la
asyncq_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

asyncmsgq_test_SOURCES = tests/asyncmsgq-test.c
asyncmsgq_test_CFLAGS = $(AM_CFLAGS)
asyncmsgq_test_LDADD = $(AM_LDADD) libpulsecore.la
asyncmsgq_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

queue_test_SOURCES = tests/queue-test.c
queue_test_CFLAGS = $(AM_CFLAGS)
queue_test_LDADD = $(AM_LDADD) libpulsecore.la
queue_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

rtpoll_test_SOURCES = tests/rtpoll-test.c
rtpoll_test_CFLAGS = $(AM_CFLAGS)
rtpoll_test_LDADD = $(AM_LDADD) libpulsecore.la
rtpoll_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mcalign_test_SOURCES = tests/mcalign-test.c
mcalign_test_CFLAGS = $(AM_CFLAGS)
mcalign_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS) libpulsecore.la
mcalign_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pacat_simple_SOURCES = tests/pacat-simple.c
pacat_simple_LDADD = $(AM_LDADD) libpulse.la libpulse-simple.la
pacat_simple_CFLAGS = $(AM_CFLAGS)
pacat_simple_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

parec_simple_SOURCES = tests/parec-simple.c
parec_simple_LDADD = $(AM_LDADD) libpulse.la libpulse-simple.la
parec_simple_CFLAGS = $(AM_CFLAGS)
parec_simple_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

strlist_test_SOURCES = tests/strlist-test.c
strlist_test_CFLAGS = $(AM_CFLAGS)
strlist_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS) libpulsecore.la libstrlist.la
strlist_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

close_test_SOURCES = tests/close-test.c
close_test_CFLAGS = $(AM_CFLAGS)
close_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS) libpulsecore.la libstrlist.la
close_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

voltest_SOURCES = tests/voltest.c
voltest_CFLAGS = $(AM_CFLAGS)
voltest_LDADD = $(AM_LDADD) libpulse.la
voltest_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

channelmap_test_SOURCES = tests/channelmap-test.c
channelmap_test_CFLAGS = $(AM_CFLAGS)
channelmap_test_LDADD = $(AM_LDADD) libpulse.la
channelmap_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

cpulimit_test_SOURCES = tests/cpulimit-test.c daemon/cpulimit.c daemon/cpulimit.h
cpulimit_test_CFLAGS = $(AM_CFLAGS)
cpulimit_test_LDADD = $(AM_LDADD) libpulsecore.la
cpulimit_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

cpulimit_test2_SOURCES = tests/cpulimit-test.c daemon/cpulimit.c daemon/cpulimit.h
cpulimit_test2_CFLAGS = $(AM_CFLAGS) -DTEST2
cpulimit_test2_LDADD = $(AM_LDADD) libpulsecore.la
cpulimit_test2_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mainloop_test_glib_SOURCES = $(mainloop_test_SOURCES)
mainloop_test_glib_CFLAGS = $(mainloop_test_CFLAGS) $(GLIB20_CFLAGS) -DGLIB_MAIN_LOOP
mainloop_test_glib_LDADD = $(mainloop_test_LDADD) $(GLIB20_LIBS) libpulse-mainloop-glib.la
mainloop_test_glib_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

memblockq_test_SOURCES = tests/memblockq-test.c
memblockq_test_CFLAGS = $(AM_CFLAGS)
memblockq_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS) libpulsecore.la
memblockq_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

sync_playback_SOURCES = tests/sync-playback.c
sync_playback_LDADD = $(AM_LDADD) libpulse.la
sync_playback_CFLAGS = $(AM_CFLAGS)
sync_playback_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

interpol_test_SOURCES = tests/interpol-test.c
interpol_test_LDADD = $(AM_LDADD) libpulse.la libpulsecore.la
interpol_test_CFLAGS = $(AM_CFLAGS)
interpol_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

sig2str_test_SOURCES = tests/sig2str-test.c
sig2str_test_LDADD = $(AM_LDADD) libpulsecore.la
sig2str_test_CFLAGS = $(AM_CFLAGS)
sig2str_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

resampler_test_SOURCES = tests/resampler-test.c
resampler_test_LDADD = $(AM_LDADD) libpulsecore.la
resampler_test_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
resampler_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

mix_test_SOURCES = tests/mix-test.c
mix_test_LDADD = $(AM_LDADD) libpulsecore.la
mix_test_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
mix_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

remix_test_SOURCES = tests/remix-test.c
remix_test_LDADD = $(AM_LDADD) libpulsecore.la
remix_test_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
remix_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

smoother_test_SOURCES = tests/smoother-test.c
smoother_test_LDADD = $(AM_LDADD) libpulsecore.la
smoother_test_CFLAGS = $(AM_CFLAGS)
smoother_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

envelope_test_SOURCES = tests/envelope-test.c
envelope_test_LDADD = $(AM_LDADD) libpulsecore.la
envelope_test_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
envelope_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

proplist_test_SOURCES = tests/proplist-test.c
proplist_test_LDADD = $(AM_LDADD) libpulsecore.la
proplist_test_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
proplist_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

rtstutter_SOURCES = tests/rtstutter.c
rtstutter_LDADD = $(AM_LDADD) libpulsecore.la
rtstutter_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
rtstutter_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

stripnul_SOURCES = tests/stripnul.c
stripnul_LDADD = $(AM_LDADD) libpulsecore.la
stripnul_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
stripnul_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

lock_autospawn_test_SOURCES = tests/lock-autospawn-test.c
lock_autospawn_test_LDADD = $(AM_LDADD) libpulsecore.la
lock_autospawn_test_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
lock_autospawn_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

prioq_test_SOURCES = tests/prioq-test.c
prioq_test_LDADD = $(AM_LDADD) libpulsecore.la
prioq_test_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
prioq_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(LIBOIL_LIBS)

###################################
#         Client library          #
###################################

pulseinclude_HEADERS = \
		pulse/cdecl.h \
		pulse/channelmap.h \
		pulse/context.h \
		pulse/def.h \
		pulse/error.h \
		pulse/introspect.h \
		pulse/mainloop.h \
		pulse/mainloop-api.h \
		pulse/mainloop-signal.h \
		pulse/operation.h \
		pulse/pulseaudio.h \
		pulse/sample.h \
		pulse/scache.h \
		pulse/simple.h \
		pulse/stream.h \
		pulse/subscribe.h \
		pulse/thread-mainloop.h \
		pulse/timeval.h \
		pulse/utf8.h \
		pulse/util.h \
		pulse/version.h \
		pulse/volume.h \
		pulse/xmalloc.h \
		pulse/proplist.h \
		pulse/gccmacro.h \
		pulse/ext-stream-restore.h

if HAVE_AVAHI
pulseinclude_HEADERS += \
		pulse/browser.h
endif

if HAVE_GLIB20
pulseinclude_HEADERS += \
		pulse/glib-mainloop.h
endif

lib_LTLIBRARIES = \
		libpulse.la \
		libpulse-simple.la

if HAVE_AVAHI
lib_LTLIBRARIES += \
		libpulse-browse.la
endif

if HAVE_GLIB20
lib_LTLIBRARIES += \
		libpulse-mainloop-glib.la
endif

# Public interface
libpulse_la_SOURCES = \
		pulse/cdecl.h \
		pulse/channelmap.c pulse/channelmap.h \
		pulse/client-conf.c pulse/client-conf.h \
		pulse/context.c pulse/context.h \
		pulse/def.h \
		pulse/error.c pulse/error.h \
		pulse/internal.h \
		pulse/introspect.c pulse/introspect.h \
		pulse/mainloop.c pulse/mainloop.h \
		pulse/mainloop-api.c pulse/mainloop-api.h \
		pulse/mainloop-signal.c pulse/mainloop-signal.h \
		pulse/operation.c pulse/operation.h \
		pulse/pulseaudio.h \
		pulse/sample.c pulse/sample.h \
		pulse/scache.c pulse/scache.h \
		pulse/stream.c pulse/stream.h \
		pulse/subscribe.c pulse/subscribe.h \
		pulse/thread-mainloop.c pulse/thread-mainloop.h \
		pulse/timeval.c pulse/timeval.h \
		pulse/utf8.c pulse/utf8.h \
		pulse/util.c pulse/util.h \
		pulse/volume.c pulse/volume.h \
		pulse/xmalloc.c pulse/xmalloc.h \
		pulse/proplist.c pulse/proplist.h \
		pulse/ext-stream-restore.c pulse/ext-stream-restore.h \
		pulse/i18n.c pulse/i18n.h

# Internal stuff that is shared with libpulsecore
libpulse_la_SOURCES += \
		pulsecore/authkey.c pulsecore/authkey.h \
		pulsecore/conf-parser.c pulsecore/conf-parser.h \
		pulsecore/core-util.c pulsecore/core-util.h \
		pulsecore/dynarray.c pulsecore/dynarray.h \
		pulsecore/hashmap.c pulsecore/hashmap.h \
		pulsecore/idxset.c pulsecore/idxset.h \
		pulsecore/inet_ntop.c pulsecore/inet_ntop.h \
		pulsecore/iochannel.c pulsecore/iochannel.h \
		pulsecore/llist.h \
		pulsecore/log.c pulsecore/log.h \
		pulsecore/mcalign.c pulsecore/mcalign.h \
		pulsecore/memblock.c pulsecore/memblock.h \
		pulsecore/memblockq.c pulsecore/memblockq.h \
		pulsecore/memchunk.c pulsecore/memchunk.h \
		pulsecore/native-common.h \
		pulsecore/packet.c pulsecore/packet.h \
		pulsecore/parseaddr.c pulsecore/parseaddr.h \
		pulsecore/pdispatch.c pulsecore/pdispatch.h \
		pulsecore/pipe.c pulsecore/pipe.h \
		pulsecore/poll.c pulsecore/poll.h \
		pulsecore/pstream.c pulsecore/pstream.h \
		pulsecore/pstream-util.c pulsecore/pstream-util.h \
		pulsecore/queue.c pulsecore/queue.h \
		pulsecore/random.c pulsecore/random.h \
		pulsecore/socket-client.c pulsecore/socket-client.h \
		pulsecore/socket-util.c pulsecore/socket-util.h \
		pulsecore/strbuf.c pulsecore/strbuf.h \
		pulsecore/strlist.c pulsecore/strlist.h \
		pulsecore/tagstruct.c pulsecore/tagstruct.h \
		pulsecore/core-error.c pulsecore/core-error.h \
		pulsecore/winsock.h pulsecore/creds.h \
		pulsecore/shm.c pulsecore/shm.h \
		pulsecore/flist.c pulsecore/flist.h \
		pulsecore/object.c pulsecore/object.h \
		pulsecore/msgobject.c pulsecore/msgobject.h \
		pulsecore/once.c pulsecore/once.h \
		pulsecore/rtclock.c pulsecore/rtclock.h \
		pulsecore/time-smoother.c pulsecore/time-smoother.h \
		pulsecore/proplist-util.c pulsecore/proplist-util.h \
		$(PA_THREAD_OBJS)

if OS_IS_WIN32
libpulse_la_SOURCES += \
		pulsecore/dllmain.c
endif

if HAVE_X11
libpulse_la_SOURCES += \
		pulse/client-conf-x11.c pulse/client-conf-x11.h \
		pulsecore/x11prop.c pulsecore/x11prop.h
endif

libpulse_la_CFLAGS = $(AM_CFLAGS)
libpulse_la_LDFLAGS = -version-info $(LIBPULSE_VERSION_INFO) -Wl,-version-script=$(srcdir)/map-file
libpulse_la_LIBADD = $(AM_LIBADD) $(WINSOCK_LIBS) $(LTLIBICONV)

if HAVE_X11
libpulse_la_CFLAGS += $(X_CFLAGS)
libpulse_la_LDFLAGS += $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)
endif

if HAVE_LIBASYNCNS
libpulse_la_CFLAGS += $(LIBASYNCNS_CFLAGS)
libpulse_la_LIBADD += $(LIBASYNCNS_LIBS)
endif

libpulse_simple_la_SOURCES = \
		pulse/simple.c pulse/simple.h	\
		pulsecore/log.c pulsecore/log.h \
		pulsecore/core-util.c pulsecore/core-util.h  \
		pulsecore/core-error.c pulsecore/core-error.h \
		pulsecore/once.c pulsecore/once.h \
		pulsecore/rtclock.c pulsecore/rtclock.h \
		$(PA_THREAD_OBJS)

libpulse_simple_la_CFLAGS = $(AM_CFLAGS)
libpulse_simple_la_LIBADD = $(AM_LIBADD) libpulse.la
libpulse_simple_la_LDFLAGS = -version-info $(LIBPULSE_SIMPLE_VERSION_INFO) -Wl,-version-script=$(srcdir)/map-file

libpulse_browse_la_SOURCES = pulse/browser.c pulse/browser.h pulsecore/avahi-wrap.c pulsecore/avahi-wrap.h pulsecore/core-util.c pulsecore/core-util.h pulsecore/core-error.c pulsecore/core-error.h  pulsecore/log.c pulsecore/log.h pulsecore/rtclock.c pulsecore/rtclock.h pulsecore/once.c pulsecore/once.h $(PA_THREAD_OBJS)
libpulse_browse_la_CFLAGS = $(AM_CFLAGS) $(AVAHI_CFLAGS)
libpulse_browse_la_LIBADD = $(AM_LIBADD) libpulse.la $(AVAHI_LIBS)
libpulse_browse_la_LDFLAGS = -version-info $(LIBPULSE_BROWSE_VERSION_INFO) -Wl,-version-script=$(srcdir)/map-file

libpulse_mainloop_glib_la_SOURCES = \
		pulse/glib-mainloop.h pulse/glib-mainloop.c \
		pulsecore/log.c pulsecore/log.h \
		pulsecore/rtclock.c pulsecore/rtclock.h \
		pulsecore/core-util.c pulsecore/core-util.h  \
		pulsecore/core-error.c pulsecore/core-error.h \
		pulsecore/once.c pulsecore/once.h \
		$(PA_THREAD_OBJS)
libpulse_mainloop_glib_la_CFLAGS = $(AM_CFLAGS) $(GLIB20_CFLAGS)
libpulse_mainloop_glib_la_LIBADD = $(AM_LIBADD) libpulse.la $(GLIB20_LIBS)
libpulse_mainloop_glib_la_LDFLAGS = -version-info $(LIBPULSE_MAINLOOP_GLIB_VERSION_INFO) -Wl,-version-script=$(srcdir)/map-file

###################################
#         OSS emulation           #
###################################

if HAVE_OSS

lib_LTLIBRARIES += libpulsedsp.la

bin_SCRIPTS += utils/padsp

endif

libpulsedsp_la_SOURCES = utils/padsp.c pulsecore/core-util.c pulsecore/core-util.h pulsecore/core-error.c pulsecore/core-error.h  pulsecore/log.c pulsecore/log.h pulsecore/rtclock.c pulsecore/rtclock.h pulsecore/once.c pulsecore/once.h $(PA_THREAD_OBJS)
libpulsedsp_la_CFLAGS = $(AM_CFLAGS)
libpulsedsp_la_LIBADD = $(AM_LIBADD) libpulse.la
libpulsedsp_la_LDFLAGS = -avoid-version

###################################
#      ffmpeg resampler           #
###################################

noinst_LTLIBRARIES = libffmpeg-resampler.la

libffmpeg_resampler_la_CPPFLAGS = $(AM_CPPFLAGS)
libffmpeg_resampler_la_SOURCES = pulsecore/ffmpeg/resample2.c pulsecore/ffmpeg/avcodec.h pulsecore/ffmpeg/dsputil.h

###################################
#      Daemon core library        #
###################################

#pulsecoreinclude_HEADERS =
noinst_HEADERS = \
		pulsecore/autoload.h \
		pulsecore/atomic.h \
		pulsecore/cli-command.h \
		pulsecore/cli-text.h \
		pulsecore/client.h \
		pulsecore/core.h \
		pulsecore/core-scache.h \
		pulsecore/core-subscribe.h \
		pulsecore/conf-parser.h \
		pulsecore/core-util.h \
		pulsecore/dynarray.h \
		pulsecore/g711.h \
		pulsecore/hashmap.h \
		pulsecore/idxset.h \
		pulsecore/log.h \
		pulsecore/mcalign.h \
		pulsecore/memblock.h \
		pulsecore/memblockq.h \
		pulsecore/memchunk.h \
		pulsecore/modargs.h \
		pulsecore/modinfo.h \
		pulsecore/module.h \
		pulsecore/namereg.h \
		pulsecore/pid.h \
		pulsecore/play-memchunk.h \
		pulsecore/play-memblockq.h \
		pulsecore/shared.h \
		pulsecore/queue.h \
		pulsecore/random.h \
		pulsecore/resampler.h \
		pulsecore/sample-util.h \
		pulsecore/sconv.h \
		pulsecore/sink.h \
		pulsecore/sink-input.h \
		pulsecore/sioman.h \
		pulsecore/sound-file.h \
		pulsecore/sound-file-stream.h \
		pulsecore/source.h \
		pulsecore/source-output.h \
		pulsecore/strbuf.h \
		pulsecore/tokenizer.h \
		pulsecore/creds.h \
		pulsecore/shm.h \
		pulsecore/llist.h \
		pulsecore/refcnt.h \
		pulsecore/mutex.h \
		pulsecore/thread.h \
		pulsecore/semaphore.h \
		pulsecore/once.h

lib_LTLIBRARIES += libpulsecore.la

# Some public stuff is used even in the core
libpulsecore_la_SOURCES = \
		pulse/channelmap.c pulse/channelmap.h \
		pulse/error.c pulse/error.h \
		pulse/mainloop.c pulse/mainloop.h \
		pulse/mainloop-api.c pulse/mainloop-api.h \
		pulse/mainloop-signal.c pulse/mainloop-signal.h \
		pulse/sample.c pulse/sample.h \
		pulse/timeval.c pulse/timeval.h \
		pulse/utf8.c pulse/utf8.h \
		pulse/util.c pulse/util.h \
		pulse/volume.c pulse/volume.h \
		pulse/xmalloc.c pulse/xmalloc.h \
		pulse/proplist.c pulse/proplist.h \
		pulse/i18n.c pulse/i18n.h

# Pure core stuff (some are shared in libpulse though).
libpulsecore_la_SOURCES += \
		pulsecore/autoload.c pulsecore/autoload.h \
		pulsecore/cli-command.c pulsecore/cli-command.h \
		pulsecore/cli-text.c pulsecore/cli-text.h \
		pulsecore/client.c pulsecore/client.h \
		pulsecore/conf-parser.c pulsecore/conf-parser.h \
		pulsecore/core.c pulsecore/core.h \
		pulsecore/core-scache.c pulsecore/core-scache.h \
		pulsecore/core-subscribe.c pulsecore/core-subscribe.h \
		pulsecore/core-util.c pulsecore/core-util.h \
		pulsecore/dynarray.c pulsecore/dynarray.h \
		pulsecore/endianmacros.h \
		pulsecore/g711.c pulsecore/g711.h \
		pulsecore/hashmap.c pulsecore/hashmap.h \
		pulsecore/idxset.c pulsecore/idxset.h \
		pulsecore/prioq.c pulsecore/prioq.h \
		pulsecore/log.c pulsecore/log.h \
		pulsecore/mcalign.c pulsecore/mcalign.h \
		pulsecore/memblock.c pulsecore/memblock.h \
		pulsecore/memblockq.c pulsecore/memblockq.h \
		pulsecore/memchunk.c pulsecore/memchunk.h \
		pulsecore/modargs.c pulsecore/modargs.h \
		pulsecore/modinfo.c pulsecore/modinfo.h \
		pulsecore/ltdl-helper.c pulsecore/ltdl-helper.h \
		pulsecore/module.c pulsecore/module.h \
		pulsecore/namereg.c pulsecore/namereg.h \
		pulsecore/pid.c pulsecore/pid.h \
		pulsecore/pipe.c pulsecore/pipe.h \
		pulsecore/play-memchunk.c pulsecore/play-memchunk.h \
		pulsecore/play-memblockq.c pulsecore/play-memblockq.h \
		pulsecore/poll.c pulsecore/poll.h \
		pulsecore/shared.c pulsecore/shared.h \
		pulsecore/queue.c pulsecore/queue.h \
		pulsecore/random.c pulsecore/random.h \
		pulsecore/resampler.c pulsecore/resampler.h \
		pulsecore/sample-util.c pulsecore/sample-util.h \
		pulsecore/sconv.c pulsecore/sconv.h \
		pulsecore/sconv-s16be.c pulsecore/sconv-s16be.h \
		pulsecore/sconv-s16le.c pulsecore/sconv-s16le.h \
		pulsecore/sink.c pulsecore/sink.h \
		pulsecore/sink-input.c pulsecore/sink-input.h \
		pulsecore/sioman.c pulsecore/sioman.h \
		pulsecore/sound-file.c pulsecore/sound-file.h \
		pulsecore/sound-file-stream.c pulsecore/sound-file-stream.h \
		pulsecore/source.c pulsecore/source.h \
		pulsecore/source-output.c pulsecore/source-output.h \
		pulsecore/strbuf.c pulsecore/strbuf.h \
		pulsecore/tokenizer.c pulsecore/tokenizer.h \
		pulsecore/winsock.h \
		pulsecore/core-error.c pulsecore/core-error.h \
		pulsecore/hook-list.c pulsecore/hook-list.h \
		pulsecore/shm.c pulsecore/shm.h \
		pulsecore/flist.c pulsecore/flist.h \
		pulsecore/asyncmsgq.c pulsecore/asyncmsgq.h \
		pulsecore/asyncq.c pulsecore/asyncq.h \
		pulsecore/thread-mq.c pulsecore/thread-mq.h \
		pulsecore/fdsem.c pulsecore/fdsem.h \
		pulsecore/object.c pulsecore/object.h \
		pulsecore/msgobject.c pulsecore/msgobject.h \
		pulsecore/rtsig.c pulsecore/rtsig.h \
		pulsecore/rtpoll.c pulsecore/rtpoll.h \
		pulsecore/rtclock.c pulsecore/rtclock.h \
		pulsecore/macro.h \
		pulsecore/once.c pulsecore/once.h \
		pulsecore/time-smoother.c pulsecore/time-smoother.h \
		pulsecore/start-child.c pulsecore/start-child.h \
		pulsecore/envelope.c pulsecore/envelope.h \
		pulsecore/proplist-util.c pulsecore/proplist-util.h \
		pulsecore/lock-autospawn.c pulsecore/lock-autospawn.h \
		$(PA_THREAD_OBJS)

if OS_IS_WIN32
libpulsecore_la_SOURCES += \
		pulsecore/dllmain.c
endif

libpulsecore_la_CPPFLAGS = $(AM_CPPFLAGS) $(LIBOIL_CFLAGS)
libpulsecore_la_LDFLAGS = -version-info $(LIBPULSECORE_VERSION_INFO)
libpulsecore_la_LIBADD = $(AM_LIBADD) $(LIBLTDL) $(LIBSAMPLERATE_LIBS) $(LIBSNDFILE_LIBS) $(LIBSPEEX_LIBS) $(WINSOCK_LIBS) $(LIBOIL_LIBS) $(LTLIBICONV) libffmpeg-resampler.la

###################################
#   Plug-in support libraries     #
###################################

#pulsecoreinclude_HEADERS +=
noinst_HEADERS += \
		pulsecore/socket-util.h \
		pulsecore/iochannel.h \
		pulsecore/socket-server.h \
		pulsecore/ipacl.h \
		pulsecore/socket-client.h \
		pulsecore/parseaddr.h \
		pulsecore/packet.h \
		pulsecore/pstream.h \
		pulsecore/ioline.h \
		pulsecore/cli.h \
		pulsecore/protocol-cli.h \
		pulsecore/tagstruct.h \
		pulsecore/pstream-util.h \
		pulsecore/pdispatch.h \
		pulsecore/authkey.h \
		pulsecore/auth-cookie.h \
		pulsecore/strlist.h \
		pulsecore/protocol-simple.h \
		pulsecore/esound.h \
		pulsecore/protocol-esound.h \
		pulsecore/native-common.h \
		pulsecore/protocol-native.h \
		pulsecore/protocol-http.h

### Warning! Due to an obscure bug in libtool/automake it is required
### that the libraries in modlibexec_LTLIBRARIES are specified in-order,
### i.e. libraries near the end of the list depend on libraries near
### the head, and not the other way!

modlibexec_LTLIBRARIES = \
		libsocket-util.la \
		libiochannel.la \
		libsocket-server.la \
		libipacl.la \
		libparseaddr.la \
		libsocket-client.la \
		libpacket.la \
		libpstream.la \
		libioline.la \
		libcli.la \
		libprotocol-cli.la \
		libtagstruct.la \
		libpstream-util.la \
		libpdispatch.la \
		libauthkey.la \
		libauth-cookie.la \
		libstrlist.la \
		libprotocol-simple.la \
		libprotocol-http.la \
		libprotocol-native.la \
		libprotocol-esound.la

# We need to emulate sendmsg/recvmsg to support this on Win32
if !OS_IS_WIN32
modlibexec_LTLIBRARIES += \
		librtp.la
endif

if HAVE_X11
#pulsecoreinclude_HEADERS +=
noinst_HEADERS += \
		pulsecore/x11wrap.h \
		pulsecore/x11prop.h

modlibexec_LTLIBRARIES += \
		libx11wrap.la \
		libx11prop.la
endif

if HAVE_AVAHI
#pulsecoreinclude_HEADERS +=
noinst_HEADERS += \
		pulsecore/avahi-wrap.h

modlibexec_LTLIBRARIES += \
		libavahi-wrap.la
endif

libprotocol_simple_la_SOURCES = pulsecore/protocol-simple.c pulsecore/protocol-simple.h
libprotocol_simple_la_LDFLAGS = -avoid-version
libprotocol_simple_la_LIBADD = $(AM_LIBADD) libpulsecore.la libsocket-server.la libiochannel.la

libsocket_server_la_SOURCES = \
		pulsecore/inet_ntop.c pulsecore/inet_ntop.h \
		pulsecore/inet_pton.c pulsecore/inet_pton.h \
		pulsecore/socket-server.c pulsecore/socket-server.h
libsocket_server_la_LDFLAGS = -avoid-version
libsocket_server_la_LIBADD = $(AM_LIBADD) libpulsecore.la libiochannel.la libsocket-util.la $(LIBWRAP_LIBS) $(WINSOCK_LIBS)

libipacl_la_SOURCES = pulsecore/ipacl.h pulsecore/ipacl.c \
		pulsecore/inet_pton.c pulsecore/inet_pton.h
libipacl_la_LDFLAGS = -avoid-version
libipacl_la_LIBADD = $(AM_LIBADD) libpulsecore.la $(WINSOCK_LIBS)

libsocket_client_la_SOURCES = pulsecore/socket-client.c pulsecore/socket-client.h
libsocket_client_la_LDFLAGS = -avoid-version
libsocket_client_la_LIBADD = $(AM_LIBADD) libpulsecore.la libiochannel.la libsocket-util.la libparseaddr.la $(LIBASYNCNS_LIBS) $(WINSOCK_LIBS)
libsocket_client_la_CFLAGS = $(AM_CFLAGS) $(LIBASYNCNS_CFLAGS)

libparseaddr_la_SOURCES = pulsecore/parseaddr.c pulsecore/parseaddr.h
libparseaddr_la_LDFLAGS = -avoid-version
libparseaddr_la_LIBADD = $(AM_LIBADD) libpulsecore.la

libpstream_la_SOURCES = pulsecore/pstream.c pulsecore/pstream.h
libpstream_la_LDFLAGS = -avoid-version
libpstream_la_LIBADD = $(AM_LIBADD) libpulsecore.la libpacket.la libiochannel.la $(WINSOCK_LIBS)

libpstream_util_la_SOURCES = pulsecore/pstream-util.c pulsecore/pstream-util.h
libpstream_util_la_LDFLAGS = -avoid-version
libpstream_util_la_LIBADD = $(AM_LIBADD) libpacket.la libpstream.la libtagstruct.la libpulsecore.la

libpdispatch_la_SOURCES = pulsecore/pdispatch.c pulsecore/pdispatch.h
libpdispatch_la_LDFLAGS = -avoid-version
libpdispatch_la_LIBADD = $(AM_LIBADD) libtagstruct.la libpulsecore.la

libiochannel_la_SOURCES = pulsecore/iochannel.c pulsecore/iochannel.h
libiochannel_la_LDFLAGS = -avoid-version
libiochannel_la_LIBADD = $(AM_LIBADD) libsocket-util.la libpulsecore.la $(WINSOCK_LIBS)

libpacket_la_SOURCES = pulsecore/packet.c pulsecore/packet.h
libpacket_la_LDFLAGS = -avoid-version
libpacket_la_LIBADD = $(AM_LIBADD) libpulsecore.la

libioline_la_SOURCES = pulsecore/ioline.c pulsecore/ioline.h
libioline_la_LDFLAGS = -avoid-version
libioline_la_LIBADD = $(AM_LIBADD) libiochannel.la libpulsecore.la

libcli_la_SOURCES = pulsecore/cli.c pulsecore/cli.h
libcli_la_CPPFLAGS = $(AM_CPPFLAGS)
libcli_la_LDFLAGS = -avoid-version
libcli_la_LIBADD = $(AM_LIBADD) libiochannel.la libioline.la libpulsecore.la

libstrlist_la_SOURCES = pulsecore/strlist.c pulsecore/strlist.h
libstrlist_la_LDFLAGS = -avoid-version
libstrlist_la_LIBADD = $(AM_LIBADD) libpulsecore.la

libprotocol_cli_la_SOURCES = pulsecore/protocol-cli.c pulsecore/protocol-cli.h
libprotocol_cli_la_LDFLAGS = -avoid-version
libprotocol_cli_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libcli.la libpulsecore.la

libprotocol_http_la_SOURCES = pulsecore/protocol-http.c pulsecore/protocol-http.h
libprotocol_http_la_LDFLAGS = -avoid-version
libprotocol_http_la_LIBADD = $(AM_LIBADD) libsocket-server.la libioline.la libpulsecore.la libiochannel.la

libprotocol_native_la_SOURCES = pulsecore/protocol-native.c pulsecore/protocol-native.h pulsecore/native-common.h
libprotocol_native_la_LDFLAGS = -avoid-version
libprotocol_native_la_LIBADD = $(AM_LIBADD) libsocket-server.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauth-cookie.la libstrlist.la libpulsecore.la libiochannel.la libipacl.la

libtagstruct_la_SOURCES = pulsecore/tagstruct.c pulsecore/tagstruct.h
libtagstruct_la_LDFLAGS = -avoid-version
libtagstruct_la_LIBADD = $(AM_LIBADD) libpulsecore.la $(WINSOCK_LIBS)

libprotocol_esound_la_SOURCES = pulsecore/protocol-esound.c pulsecore/protocol-esound.h pulsecore/esound.h
libprotocol_esound_la_LDFLAGS = -avoid-version
libprotocol_esound_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libauthkey.la libauth-cookie.la libpulsecore.la libipacl.la

libauthkey_la_SOURCES = pulsecore/authkey.c pulsecore/authkey.h
libauthkey_la_LDFLAGS = -avoid-version
libauthkey_la_LIBADD = $(AM_LIBADD) libpulsecore.la

libauth_cookie_la_SOURCES = pulsecore/auth-cookie.c pulsecore/auth-cookie.h
libauth_cookie_la_LDFLAGS = -avoid-version
libauth_cookie_la_LIBADD = $(AM_LIBADD) libauthkey.la libpulsecore.la

libsocket_util_la_SOURCES = \
		pulsecore/inet_ntop.c pulsecore/inet_ntop.h \
		pulsecore/socket-util.c pulsecore/socket-util.h
libsocket_util_la_LDFLAGS = -avoid-version
libsocket_util_la_LIBADD = $(AM_LIBADD) $(WINSOCK_LIBS) libpulsecore.la

librtp_la_SOURCES = modules/rtp/rtp.c modules/rtp/rtp.h modules/rtp/sdp.c modules/rtp/sdp.h modules/rtp/sap.c modules/rtp/sap.h
librtp_la_LDFLAGS = -avoid-version
librtp_la_LIBADD = $(AM_LIBADD) libpulsecore.la

# X11

libx11wrap_la_SOURCES = pulsecore/x11wrap.c pulsecore/x11wrap.h
libx11wrap_la_LDFLAGS = -avoid-version
libx11wrap_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
libx11wrap_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libpulsecore.la

libx11prop_la_SOURCES = pulsecore/x11prop.c pulsecore/x11prop.h
libx11prop_la_LDFLAGS = -avoid-version
libx11prop_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
libx11prop_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)

# Avahi

libavahi_wrap_la_SOURCES = pulsecore/avahi-wrap.c pulsecore/avahi-wrap.h
libavahi_wrap_la_LDFLAGS = -avoid-version
libavahi_wrap_la_CFLAGS = $(AM_CFLAGS) $(AVAHI_CFLAGS)
libavahi_wrap_la_LIBADD = $(AM_LIBADD) $(AVAHI_CFLAGS) libpulsecore.la

###################################
#        Plug-in libraries        #
###################################

modlibexec_LTLIBRARIES += \
		module-cli.la \
		module-cli-protocol-tcp.la \
		module-simple-protocol-tcp.la \
		module-null-sink.la \
		module-detect.la \
		module-volume-restore.la \
		module-device-restore.la \
		module-stream-restore.la \
		module-default-device-restore.la \
		module-always-sink.la \
		module-rescue-streams.la \
		module-suspend-on-idle.la \
		module-http-protocol-tcp.la \
		module-sine.la \
		module-native-protocol-tcp.la \
		module-native-protocol-fd.la \
		module-esound-protocol-tcp.la \
		module-combine.la \
		module-remap-sink.la \
		module-ladspa-sink.la \
		module-esound-sink.la \
		module-tunnel-sink.la \
		module-tunnel-source.la \
		module-position-event-sounds.la


# See comment at librtp.la above
if !OS_IS_WIN32
modlibexec_LTLIBRARIES += \
		module-rtp-send.la \
		module-rtp-recv.la
endif

if HAVE_AF_UNIX
modlibexec_LTLIBRARIES += \
		module-cli-protocol-unix.la \
		module-simple-protocol-unix.la \
		module-http-protocol-unix.la \
		module-native-protocol-unix.la \
		module-esound-protocol-unix.la
endif

if HAVE_MKFIFO
modlibexec_LTLIBRARIES += \
		module-pipe-sink.la \
		module-pipe-source.la
endif

if !OS_IS_WIN32
modlibexec_LTLIBRARIES += \
		module-esound-compat-spawnfd.la \
		module-esound-compat-spawnpid.la
endif

if HAVE_REGEX
modlibexec_LTLIBRARIES += \
		module-match.la
endif

if HAVE_X11
modlibexec_LTLIBRARIES += \
		module-x11-bell.la \
		module-x11-publish.la \
		module-x11-xsmp.la
endif

if HAVE_OSS
modlibexec_LTLIBRARIES += \
		liboss-util.la \
		module-oss.la
endif

if HAVE_ALSA
modlibexec_LTLIBRARIES += \
		libalsa-util.la \
		module-alsa-sink.la \
		module-alsa-source.la
endif

if HAVE_SOLARIS
modlibexec_LTLIBRARIES += \
		module-solaris.la
endif

if HAVE_AVAHI
modlibexec_LTLIBRARIES += \
		module-zeroconf-publish.la \
		module-zeroconf-discover.la
endif

if HAVE_LIRC
modlibexec_LTLIBRARIES += \
		module-lirc.la
endif

if HAVE_EVDEV
modlibexec_LTLIBRARIES += \
		module-mmkbd-evdev.la
endif

if HAVE_JACK
modlibexec_LTLIBRARIES += \
		module-jack-sink.la \
		module-jack-source.la
endif

pulselibexec_PROGRAMS =

if HAVE_GCONF
modlibexec_LTLIBRARIES += \
		module-gconf.la

pulselibexec_PROGRAMS += \
		gconf-helper
endif

#if OS_IS_WIN32
#modlibexec_LTLIBRARIES += \
#		module-waveout.la
#endif

if HAVE_HAL
modlibexec_LTLIBRARIES += \
		libdbus-util.la \
		module-hal-detect.la
endif

if HAVE_DBUS
modlibexec_LTLIBRARIES += \
		libdbus-util.la \
		module-console-kit.la
endif

if HAVE_BLUEZ
modlibexec_LTLIBRARIES += \
		module-bluetooth-proximity.la \
		module-bluetooth-discover.la \
		libbluetooth-ipc.la \
		libbluetooth-sbc.la \
		module-bluetooth-device.la

pulselibexec_PROGRAMS += \
		proximity-helper
endif

# These are generated by a M4 script

SYMDEF_FILES = \
		modules/module-cli-symdef.h \
		modules/module-cli-protocol-tcp-symdef.h \
		modules/module-cli-protocol-unix-symdef.h \
		modules/module-pipe-sink-symdef.h \
		modules/module-pipe-source-symdef.h \
		modules/module-simple-protocol-tcp-symdef.h \
		modules/module-simple-protocol-unix-symdef.h \
		modules/module-esound-protocol-tcp-symdef.h \
		modules/module-esound-protocol-unix-symdef.h \
		modules/module-native-protocol-tcp-symdef.h \
		modules/module-native-protocol-unix-symdef.h \
		modules/module-native-protocol-fd-symdef.h \
		modules/module-sine-symdef.h \
		modules/module-combine-symdef.h \
		modules/module-remap-sink-symdef.h \
		modules/module-ladspa-sink-symdef.h \
		modules/module-esound-compat-spawnfd-symdef.h \
		modules/module-esound-compat-spawnpid-symdef.h \
		modules/module-match-symdef.h \
		modules/module-tunnel-sink-symdef.h \
		modules/module-tunnel-source-symdef.h \
		modules/module-null-sink-symdef.h \
		modules/module-esound-sink-symdef.h \
		modules/module-zeroconf-publish-symdef.h \
		modules/module-zeroconf-discover-symdef.h \
		modules/module-lirc-symdef.h \
		modules/module-mmkbd-evdev-symdef.h \
		modules/module-http-protocol-tcp-symdef.h \
		modules/module-http-protocol-unix-symdef.h \
		modules/module-x11-bell-symdef.h \
		modules/module-x11-publish-symdef.h \
		modules/module-x11-xsmp-symdef.h \
		modules/module-oss-symdef.h \
		modules/module-alsa-sink-symdef.h \
		modules/module-alsa-source-symdef.h \
		modules/module-solaris-symdef.h \
		modules/module-waveout-symdef.h \
		modules/module-detect-symdef.h \
		modules/rtp/module-rtp-send-symdef.h \
		modules/rtp/module-rtp-recv-symdef.h \
		modules/module-jack-sink-symdef.h \
		modules/module-jack-source-symdef.h \
		modules/module-volume-restore-symdef.h \
		modules/module-device-restore-symdef.h \
		modules/module-stream-restore-symdef.h \
		modules/module-default-device-restore-symdef.h \
		modules/module-always-sink-symdef.h \
		modules/module-rescue-streams-symdef.h \
		modules/module-suspend-on-idle-symdef.h \
		modules/module-hal-detect-symdef.h \
		modules/bluetooth/module-bluetooth-proximity-symdef.h \
		modules/bluetooth/module-bluetooth-discover-symdef.h \
		modules/bluetooth/module-bluetooth-device-symdef.h \
		modules/gconf/module-gconf-symdef.h \
		modules/module-position-event-sounds-symdef.h \
		modules/module-console-kit-symdef.h

EXTRA_DIST += $(SYMDEF_FILES)
BUILT_SOURCES += $(SYMDEF_FILES)

$(SYMDEF_FILES): modules/module-defs.h.m4
	$(MKDIR_P) modules
	$(MKDIR_P) modules/gconf
	$(MKDIR_P) modules/rtp
	$(MKDIR_P) modules/bluetooth
	$(M4) -Dfname="$@" $< > $@

# Simple protocol

module_simple_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_simple_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-simple.la libsocket-server.la

module_simple_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_simple_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_unix_la_LDFLAGS = -module -avoid-version
module_simple_protocol_unix_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-simple.la libsocket-server.la libsocket-util.la

# CLI protocol

module_cli_la_SOURCES = modules/module-cli.c
module_cli_la_LDFLAGS = -module -avoid-version
module_cli_la_LIBADD = $(AM_LIBADD) libcli.la libiochannel.la libpulsecore.la

module_cli_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_cli_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_cli_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-cli.la libsocket-server.la

module_cli_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_cli_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_unix_la_LDFLAGS = -module -avoid-version
module_cli_protocol_unix_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-cli.la libsocket-server.la libsocket-util.la

# HTTP protocol

module_http_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_http_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_HTTP $(AM_CFLAGS)
module_http_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_http_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-http.la libsocket-server.la

module_http_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_http_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_HTTP $(AM_CFLAGS)
module_http_protocol_unix_la_LDFLAGS = -module -avoid-version
module_http_protocol_unix_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-http.la libsocket-server.la libsocket-util.la

# Native protocol

module_native_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_native_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_native_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-native.la libsocket-server.la

module_native_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_native_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_unix_la_LDFLAGS = -module -avoid-version
module_native_protocol_unix_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-native.la libsocket-server.la libsocket-util.la

module_native_protocol_fd_la_SOURCES = modules/module-native-protocol-fd.c
module_native_protocol_fd_la_CFLAGS = $(AM_CFLAGS)
module_native_protocol_fd_la_LDFLAGS = -module -avoid-version
module_native_protocol_fd_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-native.la libsocket-server.la libsocket-util.la libiochannel.la

# EsounD protocol

module_esound_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_esound_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_esound_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-esound.la libsocket-server.la

module_esound_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_esound_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_unix_la_LDFLAGS = -module -avoid-version
module_esound_protocol_unix_la_LIBADD = $(AM_LIBADD) libpulsecore.la libprotocol-esound.la libsocket-server.la libsocket-util.la

module_esound_compat_spawnfd_la_SOURCES = modules/module-esound-compat-spawnfd.c
module_esound_compat_spawnfd_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnfd_la_LIBADD = $(AM_LIBADD) libpulsecore.la

module_esound_compat_spawnpid_la_SOURCES = modules/module-esound-compat-spawnpid.c
module_esound_compat_spawnpid_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnpid_la_LIBADD = $(AM_LIBADD) libpulsecore.la

module_esound_sink_la_SOURCES = modules/module-esound-sink.c
module_esound_sink_la_LDFLAGS = -module -avoid-version
module_esound_sink_la_LIBADD = $(AM_LIBADD) libpulsecore.la libiochannel.la libsocket-client.la libauthkey.la libsocket-util.la

# Pipes

module_pipe_sink_la_SOURCES = modules/module-pipe-sink.c
module_pipe_sink_la_LDFLAGS = -module -avoid-version
module_pipe_sink_la_LIBADD = $(AM_LIBADD) libpulsecore.la libiochannel.la

module_pipe_source_la_SOURCES = modules/module-pipe-source.c
module_pipe_source_la_LDFLAGS = -module -avoid-version
module_pipe_source_la_LIBADD = $(AM_LIBADD) libpulsecore.la libiochannel.la

# Fake sources/sinks

module_sine_la_SOURCES = modules/module-sine.c
module_sine_la_LDFLAGS = -module -avoid-version
module_sine_la_LIBADD = $(AM_LIBADD) libpulsecore.la

module_null_sink_la_SOURCES = modules/module-null-sink.c
module_null_sink_la_LDFLAGS = -module -avoid-version
module_null_sink_la_LIBADD = $(AM_LIBADD) libpulsecore.la

# Couplings

module_combine_la_SOURCES = modules/module-combine.c
module_combine_la_LDFLAGS = -module -avoid-version
module_combine_la_LIBADD = $(AM_LIBADD) libpulsecore.la

module_remap_sink_la_SOURCES = modules/module-remap-sink.c
module_remap_sink_la_LDFLAGS = -module -avoid-version
module_remap_sink_la_LIBADD = $(AM_LIBADD) libpulsecore.la

module_ladspa_sink_la_SOURCES = modules/module-ladspa-sink.c modules/ladspa.h
module_ladspa_sink_la_CFLAGS = -DLADSPA_PATH=\"$(libdir)/ladspa:/usr/local/lib/ladspa:/usr/lib/ladspa:/usr/local/lib64/ladspa:/usr/lib64/ladspa\" $(AM_CFLAGS)
module_ladspa_sink_la_LDFLAGS = -module -avoid-version
module_ladspa_sink_la_LIBADD = $(AM_LIBADD) $(LIBLTDL) libpulsecore.la

module_match_la_SOURCES = modules/module-match.c
module_match_la_LDFLAGS = -module -avoid-version
module_match_la_LIBADD = $(AM_LIBADD) libpulsecore.la

module_tunnel_sink_la_SOURCES = modules/module-tunnel.c
module_tunnel_sink_la_CFLAGS = -DTUNNEL_SINK=1 $(AM_CFLAGS)
module_tunnel_sink_la_LDFLAGS = -module -avoid-version
module_tunnel_sink_la_LIBADD = $(AM_LIBADD) libpulsecore.la libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauth-cookie.la libsocket-util.la libiochannel.la

module_tunnel_source_la_SOURCES = modules/module-tunnel.c
module_tunnel_source_la_LDFLAGS = -module -avoid-version
module_tunnel_source_la_LIBADD = $(AM_LIBADD) libpulsecore.la libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauth-cookie.la libsocket-util.la libiochannel.la

# X11

module_x11_bell_la_SOURCES = modules/module-x11-bell.c
module_x11_bell_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_bell_la_LDFLAGS = -module -avoid-version
module_x11_bell_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libx11wrap.la libpulsecore.la

module_x11_publish_la_SOURCES = modules/module-x11-publish.c
module_x11_publish_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_publish_la_LDFLAGS = -module -avoid-version
module_x11_publish_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libx11wrap.la libauthkey.la libauth-cookie.la libx11prop.la libstrlist.la libprotocol-native.la libpulsecore.la

module_x11_xsmp_la_SOURCES = modules/module-x11-xsmp.c
module_x11_xsmp_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_xsmp_la_LDFLAGS = -module -avoid-version
module_x11_xsmp_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libx11wrap.la libpulsecore.la

# OSS

liboss_util_la_SOURCES = modules/oss-util.c modules/oss-util.h
liboss_util_la_LDFLAGS = -avoid-version
liboss_util_la_LIBADD = libpulsecore.la

module_oss_la_SOURCES = modules/module-oss.c
module_oss_la_LDFLAGS = -module -avoid-version
module_oss_la_LIBADD = $(AM_LIBADD) libiochannel.la liboss-util.la libpulsecore.la

# ALSA

libalsa_util_la_SOURCES = modules/alsa-util.c modules/alsa-util.h
libalsa_util_la_LDFLAGS = -avoid-version
libalsa_util_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libpulsecore.la
libalsa_util_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_sink_la_SOURCES = modules/module-alsa-sink.c
module_alsa_sink_la_LDFLAGS = -module -avoid-version
module_alsa_sink_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la libpulsecore.la
module_alsa_sink_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_source_la_SOURCES = modules/module-alsa-source.c
module_alsa_source_la_LDFLAGS = -module -avoid-version
module_alsa_source_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la libpulsecore.la
module_alsa_source_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

# Solaris

module_solaris_la_SOURCES = modules/module-solaris.c
module_solaris_la_LDFLAGS = -module -avoid-version
module_solaris_la_LIBADD = $(AM_LIBADD) libiochannel.la libpulsecore.la

# Avahi

module_zeroconf_publish_la_SOURCES = modules/module-zeroconf-publish.c
module_zeroconf_publish_la_LDFLAGS = -module -avoid-version
module_zeroconf_publish_la_LIBADD = $(AM_LIBADD) $(AVAHI_LIBS) libavahi-wrap.la libpulsecore.la
module_zeroconf_publish_la_CFLAGS = $(AM_CFLAGS) $(AVAHI_CFLAGS)

module_zeroconf_discover_la_SOURCES = modules/module-zeroconf-discover.c
module_zeroconf_discover_la_LDFLAGS = -module -avoid-version
module_zeroconf_discover_la_LIBADD = $(AM_LIBADD) $(AVAHI_LIBS) libavahi-wrap.la libpulsecore.la
module_zeroconf_discover_la_CFLAGS = $(AM_CFLAGS) $(AVAHI_CFLAGS)

# LIRC

module_lirc_la_SOURCES = modules/module-lirc.c
module_lirc_la_LDFLAGS = -module -avoid-version
module_lirc_la_LIBADD = $(AM_LIBADD) $(LIRC_LIBS) libpulsecore.la
module_lirc_la_CFLAGS = $(AM_CFLAGS) $(LIRC_CFLAGS)

# Linux evdev

module_mmkbd_evdev_la_SOURCES = modules/module-mmkbd-evdev.c
module_mmkbd_evdev_la_LDFLAGS = -module -avoid-version
module_mmkbd_evdev_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_mmkbd_evdev_la_CFLAGS = $(AM_CFLAGS)

# Windows waveout

#module_waveout_la_SOURCES = modules/module-waveout.c
#module_waveout_la_LDFLAGS = -module -avoid-version
#module_waveout_la_LIBADD = $(AM_LIBADD) libpulsecore.la -lwinmm
#module_waveout_la_CFLAGS = $(AM_CFLAGS)

# Hardware autodetection module
module_detect_la_SOURCES = modules/module-detect.c
module_detect_la_LDFLAGS = -module -avoid-version
module_detect_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_detect_la_CFLAGS = $(AM_CFLAGS)

# Volume restore module
module_volume_restore_la_SOURCES = modules/module-volume-restore.c
module_volume_restore_la_LDFLAGS = -module -avoid-version
module_volume_restore_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_volume_restore_la_CFLAGS = $(AM_CFLAGS)

# Position event sounds in space
module_position_event_sounds_la_SOURCES = modules/module-position-event-sounds.c
module_position_event_sounds_la_LDFLAGS = -module -avoid-version
module_position_event_sounds_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_position_event_sounds_CFLAGS = $(AM_CFLAGS)

# Device volume/muted restore module
module_device_restore_la_SOURCES = modules/module-device-restore.c
module_device_restore_la_LDFLAGS = -module -avoid-version
module_device_restore_la_LIBADD = $(AM_LIBADD) libpulsecore.la -lgdbm
module_device_restore_la_CFLAGS = $(AM_CFLAGS)

# Stream volume/muted/device restore module
module_stream_restore_la_SOURCES = modules/module-stream-restore.c
module_stream_restore_la_LDFLAGS = -module -avoid-version
module_stream_restore_la_LIBADD = $(AM_LIBADD) libtagstruct.la libprotocol-native.la libpulsecore.la -lgdbm
module_stream_restore_la_CFLAGS = $(AM_CFLAGS)

# Default sink/source restore module
module_default_device_restore_la_SOURCES = modules/module-default-device-restore.c
module_default_device_restore_la_LDFLAGS = -module -avoid-version
module_default_device_restore_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_default_device_restore_la_CFLAGS = $(AM_CFLAGS)

# Always Sink module
module_always_sink_la_SOURCES = modules/module-always-sink.c
module_always_sink_la_LDFLAGS = -module -avoid-version
module_always_sink_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_always_sink_la_CFLAGS = $(AM_CFLAGS)

# Rescue streams module
module_rescue_streams_la_SOURCES = modules/module-rescue-streams.c
module_rescue_streams_la_LDFLAGS = -module -avoid-version
module_rescue_streams_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_rescue_streams_la_CFLAGS = $(AM_CFLAGS)

# Suspend-on-idle module
module_suspend_on_idle_la_SOURCES = modules/module-suspend-on-idle.c
module_suspend_on_idle_la_LDFLAGS = -module -avoid-version
module_suspend_on_idle_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_suspend_on_idle_la_CFLAGS = $(AM_CFLAGS)

# RTP modules
module_rtp_send_la_SOURCES = modules/rtp/module-rtp-send.c
module_rtp_send_la_LDFLAGS = -module -avoid-version
module_rtp_send_la_LIBADD = $(AM_LIBADD) libpulsecore.la librtp.la libsocket-util.la
module_rtp_send_la_CFLAGS = $(AM_CFLAGS)

module_rtp_recv_la_SOURCES = modules/rtp/module-rtp-recv.c
module_rtp_recv_la_LDFLAGS = -module -avoid-version
module_rtp_recv_la_LIBADD = $(AM_LIBADD) libpulsecore.la librtp.la
module_rtp_recv_la_CFLAGS = $(AM_CFLAGS)

# JACK

module_jack_sink_la_SOURCES = modules/module-jack-sink.c
module_jack_sink_la_LDFLAGS = -module -avoid-version
module_jack_sink_la_LIBADD = $(AM_LIBADD) libpulsecore.la $(JACK_LIBS)
module_jack_sink_la_CFLAGS = $(AM_CFLAGS) $(JACK_CFLAGS)

module_jack_source_la_SOURCES = modules/module-jack-source.c
module_jack_source_la_LDFLAGS = -module -avoid-version
module_jack_source_la_LIBADD = $(AM_LIBADD) libpulsecore.la $(JACK_LIBS)
module_jack_source_la_CFLAGS = $(AM_CFLAGS) $(JACK_CFLAGS)

# HAL/D-Bus
libdbus_util_la_SOURCES = modules/dbus-util.c modules/dbus-util.h
libdbus_util_la_LDFLAGS = -avoid-version
libdbus_util_la_LIBADD = $(AM_LIBADD) $(DBUS_LIBS) libpulsecore.la
libdbus_util_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS)

module_hal_detect_la_SOURCES = modules/module-hal-detect.c
module_hal_detect_la_LDFLAGS = -module -avoid-version
module_hal_detect_la_LIBADD = $(AM_LIBADD) $(HAL_LIBS) libpulsecore.la libdbus-util.la
module_hal_detect_la_CFLAGS = $(AM_CFLAGS) $(HAL_CFLAGS)

module_console_kit_la_SOURCES = modules/module-console-kit.c
module_console_kit_la_LDFLAGS = -module -avoid-version
module_console_kit_la_LIBADD = $(AM_LIBADD) $(DBUS_LIBS) libpulsecore.la libdbus-util.la
module_console_kit_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS)

# GConf support
module_gconf_la_SOURCES = modules/gconf/module-gconf.c
module_gconf_la_LDFLAGS = -module -avoid-version
module_gconf_la_LIBADD = $(AM_LIBADD) libpulsecore.la
module_gconf_la_CFLAGS = $(AM_CFLAGS) -DPA_GCONF_HELPER=\"$(pulselibexecdir)/gconf-helper\"

gconf_helper_SOURCES = modules/gconf/gconf-helper.c
gconf_helper_LDADD = $(AM_LDADD) $(GCONF_LIBS) libpulsecore.la
gconf_helper_CFLAGS = $(AM_CFLAGS) $(GCONF_CFLAGS)
gconf_helper_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

# Bluetooth proximity
module_bluetooth_proximity_la_SOURCES = modules/bluetooth/module-bluetooth-proximity.c
module_bluetooth_proximity_la_LDFLAGS = -module -avoid-version
module_bluetooth_proximity_la_LIBADD = $(AM_LIBADD) $(DBUS_LIBS) libpulsecore.la libdbus-util.la
module_bluetooth_proximity_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS) -DPA_BT_PROXIMITY_HELPER=\"$(pulselibexecdir)/proximity-helper\"

proximity_helper_SOURCES = modules/bluetooth/proximity-helper.c
proximity_helper_LDADD = $(AM_LDADD) $(BLUEZ_LIBS)
proximity_helper_CFLAGS = $(AM_CFLAGS) $(BLUEZ_CFLAGS)
proximity_helper_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

# Bluetooth sink / source
module_bluetooth_discover_la_SOURCES = modules/bluetooth/module-bluetooth-discover.c
module_bluetooth_discover_la_LDFLAGS = -module -avoid-version
module_bluetooth_discover_la_LIBADD = $(AM_LIBADD) $(DBUS_LIBS) libpulsecore.la libdbus-util.la
module_bluetooth_discover_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS)

libbluetooth_sbc_la_SOURCES = modules/bluetooth/sbc.c modules/bluetooth/sbc.h modules/bluetooth/sbc_tables.h modules/bluetooth/sbc_math.h
libbluetooth_sbc_la_LDFLAGS = -avoid-version
libbluetooth_sbc_la_LIBADD = $(AM_LIBADD)
libbluetooth_sbc_la_CFLAGS = $(AM_CFLAGS)

libbluetooth_ipc_la_SOURCES = modules/bluetooth/ipc.c modules/bluetooth/ipc.h
libbluetooth_ipc_la_LDFLAGS = -avoid-version
libbluetooth_ipc_la_LIBADD = $(AM_LIBADD)
libbluetooth_ipc_la_CFLAGS = $(AM_CFLAGS)

module_bluetooth_device_la_SOURCES = modules/bluetooth/module-bluetooth-device.c modules/bluetooth/rtp.h
module_bluetooth_device_la_LDFLAGS = -module -avoid-version
module_bluetooth_device_la_LIBADD = $(AM_LIBADD) $(DBUS_LIBS) libpulsecore.la libdbus-util.la libbluetooth-ipc.la libbluetooth-sbc.la libsocket-util.la
module_bluetooth_device_la_CFLAGS = $(AM_CFLAGS) $(DBUS_CFLAGS)

###################################
#        Some minor stuff         #
###################################

suid: pulseaudio .libs/lt-pulseaudio
	chown root $^
	chmod u+s $^

CLEANFILES = esdcompat client.conf default.pa system.pa daemon.conf start-pulseaudio-x11

esdcompat: daemon/esdcompat.in Makefile
	sed -e 's,@PACKAGE_VERSION\@,$(PACKAGE_VERSION),g' \
		-e 's,@PACKAGE_NAME\@,$(PACKAGE_NAME),g' \
		-e 's,@PA_BINARY\@,$(PA_BINARY),g' < $< > $@
	chmod +x esdcompat

start-pulseaudio-x11: daemon/start-pulseaudio-x11.in Makefile
	sed -e 's,@PA_BINARY\@,$(PA_BINARY),g' \
		-e 's,@PACTL_BINARY\@,$(bindir)/pactl,g' < $< > $@
	chmod +x start-pulseaudio-x11

client.conf: pulse/client.conf.in Makefile
	sed -e 's,@PA_BINARY\@,$(PA_BINARY),g' < $< > $@

if OS_IS_WIN32
default.pa: daemon/default.pa.win32
	cp $< $@
system.pa: daemon/default.pa.win32
	cp $< $@
else
default.pa: daemon/default.pa.in Makefile
	sed -e 's,@PA_BINARY\@,$(PA_BINARY),g' \
            -e 's,@PA_DLSEARCHPATH\@,$(modlibexecdir),g' \
	    -e 's,@PA_SOEXT\@,.so,g' < $< > $@
system.pa: daemon/system.pa.in Makefile
	sed -e 's,@PA_BINARY\@,$(PA_BINARY),g' \
            -e 's,@PA_DLSEARCHPATH\@,$(modlibexecdir),g' \
	    -e 's,@PA_SOEXT\@,.so,g' < $< > $@
endif

daemon.conf: daemon/daemon.conf.in Makefile
	sed -e 's,@PA_DLSEARCHPATH\@,$(modlibexecdir),g' \
		-e 's,@PA_DEFAULT_CONFIG_FILE\@,$(DEFAULT_CONFIG_DIR),g' < $< > $@

install-exec-hook:
	chown root $(DESTDIR)$(bindir)/pulseaudio ; true
	chmod u+s $(DESTDIR)$(bindir)/pulseaudio
	-chmod u+s $(DESTDIR)$(pulselibexecdir)/proximity-helper
	ln -sf pacat $(DESTDIR)$(bindir)/parec
	rm -f $(DESTDIR)$(modlibexecdir)/*.a
	rm -f $(DESTDIR)$(libdir)/libpulsedsp.a
	rm -f $(DESTDIR)$(libdir)/libpulsedsp.la
	rm -f $(DESTDIR)$(modlibexecdir)/*.la

massif: pulseaudio
	libtool --mode=execute valgrind --tool=massif --depth=6  --alloc-fn=pa_xmalloc --alloc-fn=pa_xmalloc0 --alloc-fn=pa_xrealloc --alloc-fn=dbus_realloc --alloc-fn=pa_xnew0_internal --alloc-fn=pa_xnew_internal ./pulseaudio

update-ffmpeg:
	wget -O pulsecore/ffmpeg/resample2.c http://svn.mplayerhq.hu/ffmpeg/trunk/libavcodec/resample2.c?view=co

# Automatically generate linker version script. We use the same one for all public .sos
update-map-file:
	( echo "PULSE_0 {" ; \
	  echo "global:" ; \
	  ctags -I PA_GCC_MALLOC,PA_GCC_ALLOC_SIZE2,PA_GCC_ALLOC_SIZE,PA_GCC_PURE,PA_GCC_CONST,PA_GCC_DEPRECATED,PA_GCC_PRINTF_ATTR -f - --c-kinds=p $(pulseinclude_HEADERS) | awk '/^pa_/ { print $$1 ";" }' | sort ; \
	  echo "local:" ;  \
	  echo "*;" ; \
	  echo "};" ) > $(srcdir)/map-file

.PHONY: utils/padsp
