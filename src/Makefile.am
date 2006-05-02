# $Id$
#
# This file is part of polypaudio.
#
# polypaudio is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# polypaudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with polypaudio; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
# USA.


###################################
#       Extra directories         #
###################################

polypincludedir=$(includedir)/polyp
polypcoreincludedir=$(includedir)/polypcore
polypconfdir=$(sysconfdir)/polypaudio

modlibdir=$(libdir)/polypaudio

###################################
#            Defines              #
###################################

POLYPAUDIO_BINARY=$(bindir)/polypaudio$(EXEEXT)
if OS_IS_WIN32
DEFAULT_CONFIG_DIR=%POLYP_ROOT%
else
DEFAULT_CONFIG_DIR=$(polypconfdir)
endif

###################################
#     Compiler/linker flags       #
###################################

AM_CFLAGS = -I$(top_srcdir)/src
AM_CFLAGS += $(PTHREAD_CFLAGS) -D_POSIX_PTHREAD_SEMANTICS
AM_CFLAGS += $(LTDLINCL)
AM_CFLAGS += $(LIBSAMPLERATE_CFLAGS) $(LIBSNDFILE_CFLAGS)
AM_CFLAGS += -DDLSEARCHPATH=\"$(modlibdir)\"
#AM_CFLAGS += -DDLSEARCHPATH=\"$(shell pwd)\"
AM_CFLAGS += -DDEFAULT_CONFIG_DIR=\"$(DEFAULT_CONFIG_DIR)\"
AM_CFLAGS += -DPOLYPAUDIO_BINARY=\"$(POLYPAUDIO_BINARY)\"

# This cool debug trap works on i386/gcc only
AM_CFLAGS += '-DDEBUG_TRAP=__asm__("int $$3")'

AM_LIBADD = $(PTHREAD_LIBS)
AM_LDADD = $(PTHREAD_LIBS)

# Only required on some platforms but defined for all to avoid errors
AM_LDFLAGS = -no-undefined

if STATIC_BINS
BINLDFLAGS = -static
endif

if OS_IS_WIN32
AM_LDFLAGS+=-Wl,--export-all-symbols
WINSOCK_LIBS=-lwsock32 -lws2_32 -lwininet
endif

###################################
#          Extra files            #
###################################

EXTRA_DIST = \
		polyp/client.conf.in \
		daemon/daemon.conf.in \
		daemon/default.pa.in \
		depmod.py \
		daemon/esdcompat.sh.in \
		modules/module-defs.h.m4

polypconf_DATA = \
		default.pa \
		daemon.conf \
		client.conf

BUILT_SOURCES = \
		polyp/version.h

###################################
#          Main daemon            #
###################################

bin_PROGRAMS = polypaudio

polypaudio_SOURCES = \
		daemon/caps.h daemon/caps.c \
		daemon/cmdline.c daemon/cmdline.h \
		daemon/cpulimit.c daemon/cpulimit.h \
		daemon/daemon-conf.c daemon/daemon-conf.h \
		daemon/dumpmodules.c daemon/dumpmodules.h \
		daemon/main.c \
		polypcore/gccmacro.h

polypaudio_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
polypaudio_CPPFLAGS = $(AM_CPPFLAGS) 
polypaudio_LDADD = $(AM_LDADD) libpolypcore.la $(LIBLTDL) \
		$(LIBSAMPLERATE_LIBS) $(LIBSNDFILE_LIBS) $(CAP_LIBS) $(LIBOIL_LIBS)

if PREOPEN_MODS
PREOPEN_LIBS = $(PREOPEN_MODS)
else
PREOPEN_LIBS = $(modlib_LTLIBRARIES)
endif

if FORCE_PREOPEN
polypaudio_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) -dlpreopen force $(foreach f,$(PREOPEN_LIBS),-dlpreopen $(f))
else
polypaudio_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) -dlopen force $(foreach f,$(PREOPEN_LIBS),-dlopen $(f))
endif

###################################
#       Utility programs          #
###################################

bin_PROGRAMS += \
		pacat \
		pactl \
		paplay

if HAVE_AF_UNIX
bin_PROGRAMS += pacmd
endif

if HAVE_X11
bin_PROGRAMS += pax11publish
endif

if HAVE_HOWL
bin_PROGRAMS += pabrowse
endif

bin_SCRIPTS = esdcompat.sh

pacat_SOURCES = utils/pacat.c
pacat_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la 
pacat_CFLAGS = $(AM_CFLAGS) 
pacat_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

paplay_SOURCES = utils/paplay.c
paplay_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la $(LIBSNDFILE_LIBS)
paplay_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)
paplay_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pactl_SOURCES = utils/pactl.c
pactl_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la $(LIBSNDFILE_LIBS)
pactl_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)
pactl_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pacmd_SOURCES = utils/pacmd.c polypcore/pid.c polypcore/pid.h
pacmd_CFLAGS = $(AM_CFLAGS)
pacmd_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la
pacmd_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pax11publish_SOURCES = utils/pax11publish.c
pax11publish_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
pax11publish_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)
pax11publish_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pabrowse_SOURCES = utils/pabrowse.c
pabrowse_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-browse-@PA_MAJORMINOR@.la
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
		voltest \
		memblockq-test \
		sync-playback \
		channelmap-test

if HAVE_SIGXCPU
noinst_PROGRAMS += \
		cpulimit-test \
		cpulimit-test2
endif

if HAVE_GLIB20
noinst_PROGRAMS += \
		mainloop-test-glib
endif

if HAVE_GLIB12
noinst_PROGRAMS += \
		mainloop-test-glib12
endif

# FIXME: We need to make thread-mainloop win32-compatible first
if !OS_IS_WIN32
noinst_PROGRAMS += \
		thread-mainloop-test
endif

mainloop_test_SOURCES = tests/mainloop-test.c
mainloop_test_CFLAGS = $(AM_CFLAGS)
mainloop_test_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la
mainloop_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

thread_mainloop_test_SOURCES = tests/thread-mainloop-test.c
thread_mainloop_test_CFLAGS = $(AM_CFLAGS)
thread_mainloop_test_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la
thread_mainloop_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mcalign_test_SOURCES = tests/mcalign-test.c
mcalign_test_CFLAGS = $(AM_CFLAGS)
mcalign_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS) libpolypcore.la
mcalign_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pacat_simple_SOURCES = tests/pacat-simple.c
pacat_simple_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-simple-@PA_MAJORMINOR@.la
pacat_simple_CFLAGS = $(AM_CFLAGS)
pacat_simple_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

parec_simple_SOURCES = tests/parec-simple.c
parec_simple_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-simple-@PA_MAJORMINOR@.la
parec_simple_CFLAGS = $(AM_CFLAGS)
parec_simple_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

strlist_test_SOURCES = tests/strlist-test.c
strlist_test_CFLAGS = $(AM_CFLAGS)
strlist_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS) libpolypcore.la libstrlist.la
strlist_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

voltest_SOURCES = tests/voltest.c
voltest_CFLAGS = $(AM_CFLAGS)
voltest_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la
voltest_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

channelmap_test_SOURCES = tests/channelmap-test.c
channelmap_test_CFLAGS = $(AM_CFLAGS)
channelmap_test_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la
channelmap_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

cpulimit_test_SOURCES = tests/cpulimit-test.c daemon/cpulimit.c daemon/cpulimit.h
cpulimit_test_CFLAGS = $(AM_CFLAGS)
cpulimit_test_LDADD = $(AM_LDADD) libpolypcore.la
cpulimit_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

cpulimit_test2_SOURCES = tests/cpulimit-test.c daemon/cpulimit.c daemon/cpulimit.h
cpulimit_test2_CFLAGS = $(AM_CFLAGS) -DTEST2
cpulimit_test2_LDADD = $(AM_LDADD) libpolypcore.la
cpulimit_test2_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mainloop_test_glib_SOURCES = $(mainloop_test_SOURCES)
mainloop_test_glib_CFLAGS = $(mainloop_test_CFLAGS) $(GLIB20_CFLAGS) -DGLIB_MAIN_LOOP
mainloop_test_glib_LDADD = $(mainloop_test_LDADD) $(GLIB20_LIBS) libpolyp-mainloop-glib-@PA_MAJORMINOR@.la
mainloop_test_glib_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mainloop_test_glib12_SOURCES = $(mainloop_test_SOURCES)
mainloop_test_glib12_CFLAGS = $(mainloop_test_CFLAGS) $(GLIB12_CFLAGS) -DGLIB_MAIN_LOOP
mainloop_test_glib12_LDADD = $(mainloop_test_LDADD) $(GLIB12_LIBS) libpolyp-mainloop-glib12-@PA_MAJORMINOR@.la
mainloop_test_glib12_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

memblockq_test_SOURCES = \
	tests/memblockq-test.c \
	polypcore/memblockq.c \
	polypcore/log.c \
	polypcore/memblock.c \
	polypcore/xmalloc.c \
	polypcore/util.c \
	polypcore/mcalign.c \
	polypcore/memchunk.c
memblockq_test_CFLAGS = $(AM_CFLAGS)
memblockq_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS)
memblockq_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

sync_playback_SOURCES = tests/sync-playback.c
sync_playback_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la
sync_playback_CFLAGS = $(AM_CFLAGS) 
sync_playback_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

###################################
#         Client library          #
###################################

polypinclude_HEADERS = \
		polyp/cdecl.h \
		polyp/channelmap.h \
		polyp/glib-mainloop.h \
		polyp/mainloop.h \
		polyp/mainloop-api.h \
		polyp/mainloop-signal.h \
		polyp/thread-mainloop.h \
		polyp/polypaudio.h \
		polyp/context.h \
		polyp/def.h \
		polyp/error.h \
		polyp/introspect.h \
		polyp/operation.h \
		polyp/scache.h \
		polyp/simple.h \
		polyp/stream.h \
		polyp/subscribe.h \
		polyp/version.h \
		polyp/sample.h \
		polyp/volume.h

if HAVE_HOWL
polypinclude_HEADERS += \
		polyp/browser.h
endif

lib_LTLIBRARIES = \
		libpolyp-@PA_MAJORMINOR@.la \
		libpolyp-simple-@PA_MAJORMINOR@.la

if HAVE_HOWL
lib_LTLIBRARIES += \
		libpolyp-browse-@PA_MAJORMINOR@.la
endif

if HAVE_GLIB20
lib_LTLIBRARIES += \
		libpolyp-mainloop-glib-@PA_MAJORMINOR@.la
endif

if HAVE_GLIB12
lib_LTLIBRARIES += \
		libpolyp-mainloop-glib12-@PA_MAJORMINOR@.la
endif

# Public interface
libpolyp_@PA_MAJORMINOR@_la_SOURCES = \
		polyp/cdecl.h \
		polyp/channelmap.c polyp/channelmap.h \
		polyp/client-conf.c polyp/client-conf.h \
		polyp/mainloop-api.c polyp/mainloop-api.h \
		polyp/polypaudio.h \
		polyp/context.c polyp/context.h \
		polyp/def.h \
		polyp/error.c polyp/error.h \
		polyp/internal.h \
		polyp/introspect.c polyp/introspect.h \
		polyp/operation.c polyp/operation.h \
		polyp/scache.c polyp/scache.h \
		polyp/stream.c polyp/stream.h \
		polyp/subscribe.c polyp/subscribe.h \
		polyp/sample.c polyp/sample.h \
		polyp/volume.c polyp/volume.h \
		polyp/mainloop.c polyp/mainloop.h \
		polyp/mainloop-signal.c polyp/mainloop-signal.h \
		polyp/thread-mainloop.c polyp/thread-mainloop.h \
		polypcore/poll.c polypcore/poll.h

# Internal stuff that is shared with libpolypcore
libpolyp_@PA_MAJORMINOR@_la_SOURCES += \
		polypcore/authkey.c polypcore/authkey.h \
		polypcore/conf-parser.c polypcore/conf-parser.h \
		polypcore/dynarray.c polypcore/dynarray.h \
		polypcore/gccmacro.h \
		polypcore/hashmap.c polypcore/hashmap.h \
		polypcore/idxset.c polypcore/idxset.h \
		polypcore/iochannel.c polypcore/iochannel.h \
		polypcore/log.c polypcore/log.h \
		polypcore/mcalign.c polypcore/mcalign.h \
		polypcore/memblock.c polypcore/memblock.h \
		polypcore/memblockq.c polypcore/memblockq.h \
		polypcore/memchunk.c polypcore/memchunk.h \
		polypcore/native-common.h \
		polypcore/packet.c polypcore/packet.h \
		polypcore/parseaddr.c polypcore/parseaddr.h \
		polypcore/pdispatch.c polypcore/pdispatch.h \
		polypcore/pstream.c polypcore/pstream.h \
		polypcore/pstream-util.c polypcore/pstream-util.h \
		polypcore/queue.c polypcore/queue.h \
		polypcore/random.c polypcore/random.h \
		polypcore/socket-client.c polypcore/socket-client.h \
		polypcore/socket-util.c polypcore/socket-util.h \
		polypcore/strbuf.c polypcore/strbuf.h \
		polypcore/strlist.c polypcore/strlist.h \
		polypcore/tagstruct.c polypcore/tagstruct.h \
		polypcore/util.c polypcore/util.h \
		polypcore/winsock.h \
		polypcore/xmalloc.c polypcore/xmalloc.h \
		polypcore/llist.h

if OS_IS_WIN32
libpolyp_@PA_MAJORMINOR@_la_SOURCES += \
		polypcore/dllmain.c 
endif

if HAVE_X11
libpolyp_@PA_MAJORMINOR@_la_SOURCES += \
		polyp/client-conf-x11.c polyp/client-conf-x11.h \
		polypcore/x11prop.c polypcore/x11prop.h
endif

libpolyp_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0
libpolyp_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) $(WINSOCK_LIBS)

if HAVE_X11
libpolyp_@PA_MAJORMINOR@_la_CFLAGS += $(X_CFLAGS)
libpolyp_@PA_MAJORMINOR@_la_LDFLAGS += $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)
endif

if HAVE_LIBASYNCNS
libpolyp_@PA_MAJORMINOR@_la_CFLAGS += $(LIBASYNCNS_CFLAGS)
libpolyp_@PA_MAJORMINOR@_la_LIBADD += $(LIBASYNCNS_LIBS)
endif

libpolyp_simple_@PA_MAJORMINOR@_la_SOURCES = polyp/simple.c polyp/simple.h 
libpolyp_simple_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_simple_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la
libpolyp_simple_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_browse_@PA_MAJORMINOR@_la_SOURCES = polyp/browser.c polyp/browser.h 
libpolyp_browse_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(HOWL_CFLAGS)
libpolyp_browse_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la $(HOWL_LIBS)
libpolyp_browse_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0 

libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_SOURCES = polyp/glib-mainloop.h polyp/glib-mainloop.c
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(GLIB20_CFLAGS)
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la $(GLIB20_LIBS)
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_SOURCES = polyp/glib-mainloop.h polyp/glib12-mainloop.c
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(GLIB12_CFLAGS)
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la $(GLIB12_LIBS)
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

###################################
#      Daemon core library        #
###################################

polypcoreinclude_HEADERS = \
		polypcore/autoload.h \
		polypcore/cli-command.h \
		polypcore/cli-text.h \
		polypcore/client.h \
		polypcore/core.h \
		polypcore/core-def.h \
		polypcore/core-scache.h \
		polypcore/core-subscribe.h \
		polypcore/conf-parser.h \
		polypcore/dynarray.h \
		polypcore/g711.h \
		polypcore/hashmap.h \
		polypcore/idxset.h \
		polypcore/log.h \
		polypcore/mcalign.h \
		polypcore/memblock.h \
		polypcore/memblockq.h \
		polypcore/memchunk.h \
		polypcore/modargs.h \
		polypcore/modinfo.h \
		polypcore/module.h \
		polypcore/namereg.h \
		polypcore/pid.h \
		polypcore/play-memchunk.h \
		polypcore/props.h \
		polypcore/queue.h \
		polypcore/random.h \
		polypcore/resampler.h \
		polypcore/sample-util.h \
		polypcore/sconv.h \
		polypcore/sink.h \
		polypcore/sink-input.h \
		polypcore/sioman.h \
		polypcore/sound-file.h \
		polypcore/sound-file-stream.h \
		polypcore/source.h \
		polypcore/source-output.h \
		polypcore/strbuf.h \
		polypcore/tokenizer.h \
		polypcore/util.h \
		polypcore/xmalloc.h

lib_LTLIBRARIES += libpolypcore.la

# Some public stuff is used even in the core.
libpolypcore_la_SOURCES = \
		polyp/channelmap.c polyp/channelmap.h \
		polyp/mainloop.c polyp/mainloop.h \
		polyp/mainloop-api.c polyp/mainloop-api.h \
		polyp/mainloop-signal.c polyp/mainloop-signal.h \
		polyp/sample.c polyp/sample.h \
		polyp/volume.c polyp/volume.h

# Pure core stuff (some are shared in libpolyp though).
libpolypcore_la_SOURCES += \
		polypcore/autoload.c polypcore/autoload.h \
		polypcore/cli-command.c polypcore/cli-command.h \
		polypcore/cli-text.c polypcore/cli-text.h \
		polypcore/client.c polypcore/client.h \
		polypcore/conf-parser.c polypcore/conf-parser.h \
		polypcore/core.c polypcore/core.h \
		polypcore/core-scache.c polypcore/core-scache.h \
		polypcore/core-subscribe.c polypcore/core-subscribe.h \
		polypcore/dynarray.c polypcore/dynarray.h \
		polypcore/endianmacros.h \
		polypcore/g711.c polypcore/g711.h \
		polypcore/hashmap.c polypcore/hashmap.h \
		polypcore/idxset.c polypcore/idxset.h \
		polypcore/log.c polypcore/log.h \
		polypcore/mcalign.c polypcore/mcalign.h \
		polypcore/memblock.c polypcore/memblock.h \
		polypcore/memblockq.c polypcore/memblockq.h \
		polypcore/memchunk.c polypcore/memchunk.h \
		polypcore/modargs.c polypcore/modargs.h \
		polypcore/modinfo.c polypcore/modinfo.h \
		polypcore/module.c polypcore/module.h \
		polypcore/namereg.c polypcore/namereg.h \
		polypcore/pid.c polypcore/pid.h \
		polypcore/play-memchunk.c polypcore/play-memchunk.h \
		polypcore/poll.c polypcore/poll.h \
		polypcore/props.c polypcore/props.h \
		polypcore/queue.c polypcore/queue.h \
		polypcore/random.c polypcore/random.h \
		polypcore/resampler.c polypcore/resampler.h \
		polypcore/sample-util.c polypcore/sample-util.h \
		polypcore/sconv.c polypcore/sconv.h \
		polypcore/sconv-s16be.c polypcore/sconv-s16be.h \
		polypcore/sconv-s16le.c polypcore/sconv-s16le.h \
		polypcore/sink.c polypcore/sink.h \
		polypcore/sink-input.c polypcore/sink-input.h \
		polypcore/sioman.c polypcore/sioman.h \
		polypcore/sound-file.c polypcore/sound-file.h \
		polypcore/sound-file-stream.c polypcore/sound-file-stream.h \
		polypcore/source.c polypcore/source.h \
		polypcore/source-output.c polypcore/source-output.h \
		polypcore/strbuf.c polypcore/strbuf.h \
		polypcore/tokenizer.c polypcore/tokenizer.h \
		polypcore/util.c polypcore/util.h \
		polypcore/winsock.h \
		polypcore/xmalloc.c polypcore/xmalloc.h

if OS_IS_WIN32
libpolypcore_la_SOURCES += \
		polypcore/dllmain.c 
endif

libpolypcore_la_CPPFLAGS = $(AM_CPPFLAGS) $(LIBOIL_CFLAGS)
libpolypcore_la_LDFLAGS = -avoid-version
libpolypcore_la_LIBADD = $(AM_LIBADD) $(LIBLTDL) $(LIBSAMPLERATE_LIBS) $(LIBSNDFILE_LIBS) $(WINSOCK_LIBS) $(LIBOIL_LIBS)

###################################
#   Plug-in support libraries     #
###################################

polypcoreinclude_HEADERS += \
		polypcore/socket-util.h \
		polypcore/iochannel.h \
		polypcore/socket-server.h \
		polypcore/socket-client.h \
		polypcore/parseaddr.h \
		polypcore/packet.h \
		polypcore/pstream.h \
		polypcore/ioline.h \
		polypcore/cli.h \
		polypcore/protocol-cli.h \
		polypcore/tagstruct.h \
		polypcore/pstream-util.h \
		polypcore/pdispatch.h \
		polypcore/authkey.h \
		polypcore/authkey-prop.h \
		polypcore/strlist.h \
		polypcore/protocol-simple.h \
		polypcore/esound.h \
		polypcore/protocol-esound.h \
		polypcore/native-common.h \
		polypcore/protocol-native.h \
		polypcore/protocol-http.h

### Warning! Due to an obscure bug in libtool/automake it is required
### that the libraries in modlib_LTLIBRARIES are specified in-order,
### i.e. libraries near the end of the list depend on libraries near
### the head, and not the other way!

modlib_LTLIBRARIES = \
		libsocket-util.la \
		libiochannel.la \
		libsocket-server.la \
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
		libauthkey-prop.la \
		libstrlist.la \
		libprotocol-simple.la \
		libprotocol-esound.la \
		libprotocol-native.la \
		libprotocol-http.la

# We need to emulate sendmsg/recvmsg to support this on Win32
if !OS_IS_WIN32
modlib_LTLIBRARIES += \
		librtp.la
endif

if HAVE_X11
polypcoreinclude_HEADERS += \
		polypcore/x11wrap.h \
		polypcore/x11prop.h

modlib_LTLIBRARIES += \
		libx11wrap.la \
		libx11prop.la
endif

libprotocol_simple_la_SOURCES = polypcore/protocol-simple.c polypcore/protocol-simple.h
libprotocol_simple_la_LDFLAGS = -avoid-version
libprotocol_simple_la_LIBADD = $(AM_LIBADD) libpolypcore.la libsocket-server.la libiochannel.la

libsocket_server_la_SOURCES = \
		polypcore/inet_ntop.c polypcore/inet_ntop.h \
		polypcore/inet_pton.c polypcore/inet_pton.h \
		polypcore/socket-server.c polypcore/socket-server.h
libsocket_server_la_LDFLAGS = -avoid-version
libsocket_server_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la libsocket-util.la $(LIBWRAP_LIBS) $(WINSOCK_LIBS)

libsocket_client_la_SOURCES = polypcore/socket-client.c polypcore/socket-client.h
libsocket_client_la_LDFLAGS = -avoid-version
libsocket_client_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la libsocket-util.la libparseaddr.la $(LIBASYNCNS_LIBS) $(WINSOCK_LIBS)
libsocket_client_la_CFLAGS = $(AM_CFLAGS) $(LIBASYNCNS_CFLAGS)

libparseaddr_la_SOURCES = polypcore/parseaddr.c polypcore/parseaddr.h
libparseaddr_la_LDFLAGS = -avoid-version
libparseaddr_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libpstream_la_SOURCES = polypcore/pstream.c polypcore/pstream.h
libpstream_la_LDFLAGS = -avoid-version
libpstream_la_LIBADD = $(AM_LIBADD) libpolypcore.la libpacket.la libiochannel.la $(WINSOCK_LIBS)

libpstream_util_la_SOURCES = polypcore/pstream-util.c polypcore/pstream-util.h
libpstream_util_la_LDFLAGS = -avoid-version
libpstream_util_la_LIBADD = $(AM_LIBADD) libpacket.la libpstream.la libtagstruct.la

libpdispatch_la_SOURCES = polypcore/pdispatch.c polypcore/pdispatch.h
libpdispatch_la_LDFLAGS = -avoid-version
libpdispatch_la_LIBADD = $(AM_LIBADD) libtagstruct.la libpolypcore.la

libiochannel_la_SOURCES = polypcore/iochannel.c polypcore/iochannel.h
libiochannel_la_LDFLAGS = -avoid-version
libiochannel_la_LIBADD = $(AM_LIBADD) libsocket-util.la libpolypcore.la $(WINSOCK_LIBS)

libpacket_la_SOURCES = polypcore/packet.c polypcore/packet.h
libpacket_la_LDFLAGS = -avoid-version
libpacket_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libioline_la_SOURCES = polypcore/ioline.c polypcore/ioline.h
libioline_la_LDFLAGS = -avoid-version
libioline_la_LIBADD = $(AM_LIBADD) libiochannel.la libpolypcore.la

libcli_la_SOURCES = polypcore/cli.c polypcore/cli.h
libcli_la_CPPFLAGS = $(AM_CPPFLAGS)
libcli_la_LDFLAGS = -avoid-version
libcli_la_LIBADD = $(AM_LIBADD) libiochannel.la libioline.la libpolypcore.la

libstrlist_la_SOURCES = polypcore/strlist.c polypcore/strlist.h
libstrlist_la_LDFLAGS = -avoid-version
libstrlist_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libprotocol_cli_la_SOURCES = polypcore/protocol-cli.c polypcore/protocol-cli.h
libprotocol_cli_la_LDFLAGS = -avoid-version
libprotocol_cli_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libcli.la libpolypcore.la

libprotocol_http_la_SOURCES = polypcore/protocol-http.c polypcore/protocol-http.h
libprotocol_http_la_LDFLAGS = -avoid-version
libprotocol_http_la_LIBADD = $(AM_LIBADD) libsocket-server.la libioline.la libpolypcore.la libiochannel.la

libprotocol_native_la_SOURCES = polypcore/protocol-native.c polypcore/protocol-native.h polypcore/native-common.h
libprotocol_native_la_LDFLAGS = -avoid-version
libprotocol_native_la_LIBADD = $(AM_LIBADD) libsocket-server.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libstrlist.la libpolypcore.la libiochannel.la

libtagstruct_la_SOURCES = polypcore/tagstruct.c polypcore/tagstruct.h
libtagstruct_la_LDFLAGS = -avoid-version
libtagstruct_la_LIBADD = $(AM_LIBADD) libpolypcore.la $(WINSOCK_LIBS)

libprotocol_esound_la_SOURCES = polypcore/protocol-esound.c polypcore/protocol-esound.h polypcore/esound.h
libprotocol_esound_la_LDFLAGS = -avoid-version
libprotocol_esound_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libauthkey.la libpolypcore.la

libauthkey_la_SOURCES = polypcore/authkey.c polypcore/authkey.h
libauthkey_la_LDFLAGS = -avoid-version
libauthkey_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libauthkey_prop_la_SOURCES = polypcore/authkey-prop.c polypcore/authkey-prop.h
libauthkey_prop_la_LDFLAGS = -avoid-version
libauthkey_prop_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libsocket_util_la_SOURCES = polypcore/socket-util.c polypcore/socket-util.h
libsocket_util_la_LDFLAGS = -avoid-version
libsocket_util_la_LIBADD = $(AM_LIBADD) $(WINSOCK_LIBS) libpolypcore.la

librtp_la_SOURCES = modules/rtp/rtp.c modules/rtp/rtp.h modules/rtp/sdp.c modules/rtp/sdp.h modules/rtp/sap.c modules/rtp/sap.h 
librtp_la_LDFLAGS = -avoid-version
librtp_la_LIBADD = $(AM_LIBADD) libpolypcore.la

# X11

libx11wrap_la_SOURCES = polypcore/x11wrap.c polypcore/x11wrap.h
libx11wrap_la_LDFLAGS = -avoid-version
libx11wrap_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
libx11wrap_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libpolypcore.la

libx11prop_la_SOURCES = polypcore/x11prop.c polypcore/x11prop.h
libx11prop_la_LDFLAGS = -avoid-version
libx11prop_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
libx11prop_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)

###################################
#        Plug-in libraries        #
###################################

modlib_LTLIBRARIES += \
		module-cli.la \
		module-cli-protocol-tcp.la \
		module-simple-protocol-tcp.la \
		module-esound-protocol-tcp.la \
		module-native-protocol-tcp.la \
		module-native-protocol-fd.la \
		module-sine.la \
		module-combine.la \
		module-tunnel-sink.la \
		module-tunnel-source.la \
		module-null-sink.la \
		module-esound-sink.la \
		module-http-protocol-tcp.la \
		module-detect.la

# See comment at librtp.la above
if !OS_IS_WIN32
modlib_LTLIBRARIES += \
		module-rtp-send.la \
		module-rtp-recv.la
endif

if HAVE_AF_UNIX
modlib_LTLIBRARIES += \
		module-cli-protocol-unix.la \
		module-simple-protocol-unix.la \
		module-esound-protocol-unix.la \
		module-native-protocol-unix.la \
		module-http-protocol-unix.la
endif

if HAVE_MKFIFO
modlib_LTLIBRARIES += \
		module-pipe-sink.la \
		module-pipe-source.la
endif

if !OS_IS_WIN32
modlib_LTLIBRARIES += \
		module-esound-compat-spawnfd.la \
		module-esound-compat-spawnpid.la
endif

if HAVE_REGEX
modlib_LTLIBRARIES += \
		module-match.la
endif

if HAVE_X11
modlib_LTLIBRARIES += \
		module-x11-bell.la \
		module-x11-publish.la
endif

if HAVE_OSS
modlib_LTLIBRARIES += \
		liboss-util.la \
		module-oss.la \
		module-oss-mmap.la
endif

if HAVE_ALSA
modlib_LTLIBRARIES += \
		libalsa-util.la \
		module-alsa-sink.la \
		module-alsa-source.la
endif

if HAVE_SOLARIS
modlib_LTLIBRARIES += \
		module-solaris.la
endif

if HAVE_HOWL
modlib_LTLIBRARIES += \
		libhowl-wrap.la \
		module-zeroconf-publish.la
endif

if HAVE_LIRC
modlib_LTLIBRARIES += \
		module-lirc.la
endif

if HAVE_EVDEV
modlib_LTLIBRARIES += \
		module-mmkbd-evdev.la
endif

if HAVE_JACK
modlib_LTLIBRARIES += \
		module-jack-sink.la \
		module-jack-source.la
endif

if OS_IS_WIN32
modlib_LTLIBRARIES += \
		module-waveout.la
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
		modules/module-esound-compat-spawnfd-symdef.h \
		modules/module-esound-compat-spawnpid-symdef.h \
		modules/module-match-symdef.h \
		modules/module-tunnel-sink-symdef.h \
		modules/module-tunnel-source-symdef.h \
		modules/module-null-sink-symdef.h \
		modules/module-esound-sink-symdef.h \
		modules/module-zeroconf-publish-symdef.h \
		modules/module-lirc-symdef.h \
		modules/module-mmkbd-evdev-symdef.h \
		modules/module-http-protocol-tcp-symdef.h \
		modules/module-http-protocol-unix-symdef.h \
		modules/module-x11-bell-symdef.h \
		modules/module-x11-publish-symdef.h \
		modules/module-oss-symdef.h \
		modules/module-oss-mmap-symdef.h \
		modules/module-alsa-sink-symdef.h \
		modules/module-alsa-source-symdef.h \
		modules/module-solaris-symdef.h \
		modules/module-waveout-symdef.h \
		modules/module-detect-symdef.h \
		modules/rtp/module-rtp-send-symdef.h \
		modules/rtp/module-rtp-recv-symdef.h \
		modules/module-jack-sink-symdef.h \
		modules/module-jack-source-symdef.h 


EXTRA_DIST += $(SYMDEF_FILES)
BUILT_SOURCES += $(SYMDEF_FILES)

$(SYMDEF_FILES): modules/module-defs.h.m4
	$(M4) -Dfname="$@" $< > $@

# Simple protocol

module_simple_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_simple_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-simple.la libsocket-server.la

module_simple_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_simple_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_unix_la_LDFLAGS = -module -avoid-version
module_simple_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-simple.la libsocket-server.la libsocket-util.la

# CLI protocol

module_cli_la_SOURCES = modules/module-cli.c
module_cli_la_LDFLAGS = -module -avoid-version
module_cli_la_LIBADD = $(AM_LIBADD) libcli.la libiochannel.la libpolypcore.la

module_cli_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_cli_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_cli_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-cli.la libsocket-server.la

module_cli_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_cli_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_unix_la_LDFLAGS = -module -avoid-version
module_cli_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-cli.la libsocket-server.la libsocket-util.la

# HTTP protocol

module_http_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_http_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_HTTP $(AM_CFLAGS)
module_http_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_http_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-http.la libsocket-server.la

module_http_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_http_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_HTTP $(AM_CFLAGS)
module_http_protocol_unix_la_LDFLAGS = -module -avoid-version
module_http_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-http.la libsocket-server.la libsocket-util.la

# Native protocol

module_native_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_native_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_native_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-native.la libsocket-server.la

module_native_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_native_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_unix_la_LDFLAGS = -module -avoid-version
module_native_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-native.la libsocket-server.la libsocket-util.la

module_native_protocol_fd_la_SOURCES = modules/module-native-protocol-fd.c
module_native_protocol_fd_la_CFLAGS = $(AM_CFLAGS)
module_native_protocol_fd_la_LDFLAGS = -module -avoid-version
module_native_protocol_fd_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-native.la libsocket-server.la libsocket-util.la libiochannel.la

# EsounD protocol

module_esound_protocol_tcp_la_SOURCES = modules/module-protocol-stub.c
module_esound_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_esound_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-esound.la libsocket-server.la

module_esound_protocol_unix_la_SOURCES = modules/module-protocol-stub.c
module_esound_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_unix_la_LDFLAGS = -module -avoid-version
module_esound_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-esound.la libsocket-server.la libsocket-util.la

module_esound_compat_spawnfd_la_SOURCES = modules/module-esound-compat-spawnfd.c
module_esound_compat_spawnfd_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnfd_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_esound_compat_spawnpid_la_SOURCES = modules/module-esound-compat-spawnpid.c
module_esound_compat_spawnpid_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnpid_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_esound_sink_la_SOURCES = modules/module-esound-sink.c
module_esound_sink_la_LDFLAGS = -module -avoid-version
module_esound_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la libsocket-client.la libauthkey.la

# Pipes

module_pipe_sink_la_SOURCES = modules/module-pipe-sink.c
module_pipe_sink_la_LDFLAGS = -module -avoid-version
module_pipe_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la

module_pipe_source_la_SOURCES = modules/module-pipe-source.c
module_pipe_source_la_LDFLAGS = -module -avoid-version
module_pipe_source_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la

# Fake sources/sinks

module_sine_la_SOURCES = modules/module-sine.c
module_sine_la_LDFLAGS = -module -avoid-version
module_sine_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_null_sink_la_SOURCES = modules/module-null-sink.c
module_null_sink_la_LDFLAGS = -module -avoid-version
module_null_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la

# Couplings

module_combine_la_SOURCES = modules/module-combine.c
module_combine_la_LDFLAGS = -module -avoid-version
module_combine_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_match_la_SOURCES = modules/module-match.c
module_match_la_LDFLAGS = -module -avoid-version
module_match_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_tunnel_sink_la_SOURCES = modules/module-tunnel.c
module_tunnel_sink_la_CFLAGS = -DTUNNEL_SINK=1 $(AM_CFLAGS)
module_tunnel_sink_la_LDFLAGS = -module -avoid-version
module_tunnel_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libsocket-util.la libiochannel.la

module_tunnel_source_la_SOURCES = modules/module-tunnel.c
module_tunnel_source_la_LDFLAGS = -module -avoid-version
module_tunnel_source_la_LIBADD = $(AM_LIBADD) libpolypcore.la libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libsocket-util.la libiochannel.la

# X11

module_x11_bell_la_SOURCES = modules/module-x11-bell.c
module_x11_bell_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_bell_la_LDFLAGS = -module -avoid-version
module_x11_bell_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libx11wrap.la libpolypcore.la

module_x11_publish_la_SOURCES = modules/module-x11-publish.c
module_x11_publish_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_publish_la_LDFLAGS = -module -avoid-version
module_x11_publish_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libx11wrap.la libauthkey.la libauthkey-prop.la libx11prop.la libstrlist.la

# OSS

liboss_util_la_SOURCES = modules/oss-util.c modules/oss-util.h
liboss_util_la_LDFLAGS = -avoid-version
liboss_util_la_LIBADD = libpolypcore.la

module_oss_la_SOURCES = modules/module-oss.c
module_oss_la_LDFLAGS = -module -avoid-version
module_oss_la_LIBADD = $(AM_LIBADD) libiochannel.la liboss-util.la

module_oss_mmap_la_SOURCES = modules/module-oss-mmap.c
module_oss_mmap_la_LDFLAGS = -module -avoid-version
module_oss_mmap_la_LIBADD = $(AM_LIBADD) liboss-util.la libpolypcore.la

# ALSA

libalsa_util_la_SOURCES = modules/alsa-util.c modules/alsa-util.h
libalsa_util_la_LDFLAGS = -avoid-version
libalsa_util_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libpolypcore.la
libalsa_util_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_sink_la_SOURCES = modules/module-alsa-sink.c
module_alsa_sink_la_LDFLAGS = -module -avoid-version
module_alsa_sink_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la libpolypcore.la
module_alsa_sink_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_source_la_SOURCES = modules/module-alsa-source.c
module_alsa_source_la_LDFLAGS = -module -avoid-version
module_alsa_source_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la libpolypcore.la
module_alsa_source_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

# Solaris

module_solaris_la_SOURCES = modules/module-solaris.c
module_solaris_la_LDFLAGS = -module -avoid-version
module_solaris_la_LIBADD = $(AM_LIBADD) libiochannel.la

# HOWL

libhowl_wrap_la_SOURCES = modules/howl-wrap.c modules/howl-wrap.h
libhowl_wrap_la_LDFLAGS = -avoid-version
libhowl_wrap_la_LIBADD = $(AM_LIBADD) $(HOWL_LIBS)
libhowl_wrap_la_CFLAGS = $(AM_CFLAGS) $(HOWL_CFLAGS)

module_zeroconf_publish_la_SOURCES = modules/module-zeroconf-publish.c
module_zeroconf_publish_la_LDFLAGS = -module -avoid-version
module_zeroconf_publish_la_LIBADD = $(AM_LIBADD) $(HOWL_LIBS) libhowl-wrap.la
module_zeroconf_publish_la_CFLAGS = $(AM_CFLAGS) $(HOWL_CFLAGS)

# LIRC

module_lirc_la_SOURCES = modules/module-lirc.c
module_lirc_la_LDFLAGS = -module -avoid-version
module_lirc_la_LIBADD = $(AM_LIBADD) $(LIRC_LIBS) libpolypcore.la
module_lirc_la_CFLAGS = $(AM_CFLAGS) $(LIRC_CFLAGS)

# Linux evdev

module_mmkbd_evdev_la_SOURCES = modules/module-mmkbd-evdev.c
module_mmkbd_evdev_la_LDFLAGS = -module -avoid-version
module_mmkbd_evdev_la_LIBADD = $(AM_LIBADD) libpolypcore.la
module_mmkbd_evdev_la_CFLAGS = $(AM_CFLAGS)

# Windows waveout

module_waveout_la_SOURCES = modules/module-waveout.c
module_waveout_la_LDFLAGS = -module -avoid-version
module_waveout_la_LIBADD = $(AM_LIBADD) libpolypcore.la -lwinmm
module_waveout_la_CFLAGS = $(AM_CFLAGS)

# Hardware autodetection module
module_detect_la_SOURCES = modules/module-detect.c
module_detect_la_LDFLAGS = -module -avoid-version
module_detect_la_LIBADD = $(AM_LIBADD) libpolypcore.la
module_detect_la_CFLAGS = $(AM_CFLAGS)

# RTP modules
module_rtp_send_la_SOURCES = modules/rtp/module-rtp-send.c
module_rtp_send_la_LDFLAGS = -module -avoid-version
module_rtp_send_la_LIBADD = $(AM_LIBADD) libpolypcore.la librtp.la
module_rtp_send_la_CFLAGS = $(AM_CFLAGS)

module_rtp_recv_la_SOURCES = modules/rtp/module-rtp-recv.c
module_rtp_recv_la_LDFLAGS = -module -avoid-version
module_rtp_recv_la_LIBADD = $(AM_LIBADD) libpolypcore.la librtp.la
module_rtp_recv_la_CFLAGS = $(AM_CFLAGS)

# JACK

module_jack_sink_la_SOURCES = modules/module-jack-sink.c
module_jack_sink_la_LDFLAGS = -module -avoid-version
module_jack_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la $(JACK_LIBS)
module_jack_sink_la_CFLAGS = $(AM_LIBADD) $(JACK_CFLAGS)

module_jack_source_la_SOURCES = modules/module-jack-source.c
module_jack_source_la_LDFLAGS = -module -avoid-version
module_jack_source_la_LIBADD = $(AM_LIBADD) libpolypcore.la $(JACK_LIBS)
module_jack_source_la_CFLAGS = $(AM_LIBADD) $(JACK_CFLAGS)

###################################
#        Some minor stuff         #
###################################

suid: polypaudio
	chown root $<
	chmod u+s $<

CLEANFILES=esdcompat.sh client.conf default.pa daemon.conf

esdcompat.sh: daemon/esdcompat.sh.in Makefile
	sed -e 's,@PACKAGE_VERSION\@,$(PACKAGE_VERSION),g' \
		-e 's,@PACKAGE_NAME\@,$(PACKAGE_NAME),g' \
		-e 's,@POLYPAUDIO_BINARY\@,$(POLYPAUDIO_BINARY),g' < $< > $@

client.conf: polyp/client.conf.in Makefile
	sed -e 's,@POLYPAUDIO_BINARY\@,$(POLYPAUDIO_BINARY),g' < $< > $@

if OS_IS_WIN32    
default.pa: daemon/default.pa.win32
	cp $< $@
else
default.pa: daemon/default.pa.in Makefile
	sed -e 's,@POLYPAUDIO_BINARY\@,$(POLYPAUDIO_BINARY),g' < $< > $@
endif

daemon.conf: daemon/daemon.conf.in Makefile
	sed -e 's,@DLSEARCHPATH\@,$(modlibdir),g' \
		-e 's,@DEFAULT_CONFIG_FILE\@,$(DEFAULT_CONFIG_DIR),g' < $< > $@

install-exec-hook:
	chown root $(DESTDIR)$(bindir)/polypaudio ; true
	chmod u+s $(DESTDIR)$(bindir)/polypaudio
	ln -sf pacat $(DESTDIR)$(bindir)/parec
