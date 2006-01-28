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

AM_CFLAGS = -I$(top_srcdir)
AM_CFLAGS += $(PTHREAD_CFLAGS) -D_POSIX_PTHREAD_SEMANTICS
AM_CFLAGS += $(LTDLINCL)
AM_CFLAGS += $(LIBSAMPLERATE_CFLAGS) $(LIBSNDFILE_CFLAGS)
#AM_CFLAGS += -DDLSEARCHPATH=\"$(modlibdir)\"
AM_CFLAGS += -DDLSEARCHPATH=\"$(shell pwd)\"
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
		client.conf.in \
		daemon.conf.in \
		default.pa.in \
		depmod.py \
		esdcompat.sh.in \
		module-defs.h.m4

polypconf_DATA = default.pa daemon.conf client.conf

BUILT_SOURCES = polyplib-version.h

###################################
#          Main daemon            #
###################################

bin_PROGRAMS = polypaudio

polypaudio_SOURCES = \
		caps.h caps.c \
		cmdline.c cmdline.h \
		cpulimit.c cpulimit.h \
		conf-parser.h conf-parser.c \
		daemon-conf.c daemon-conf.h \
		dumpmodules.c dumpmodules.h \
		gcc-printf.h \
		main.c \
		pid.c pid.h

polypaudio_CFLAGS = $(AM_CFLAGS) $(LIBOIL_CFLAGS)
polypaudio_CPPFLAGS = $(AM_CPPFLAGS) 
polypaudio_LDADD = $(AM_LDADD) libpolypcore.la $(LIBLTDL) \
		$(LIBSAMPLERATE_LIBS) $(LIBSNDFILE_LIBS) $(CAP_LIBS) $(LIBOIL_LIBS)
polypaudio_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) -dlopen force $(foreach f,$(PREOPEN_LIBS),-dlopen $(f))

if PREOPEN_MODS
PREOPEN_LIBS = $(PREOPEN_MODS)
else
PREOPEN_LIBS = $(modlib_LTLIBRARIES)
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
bin_PROGRAMS += 	pabrowse
endif

bin_SCRIPTS = esdcompat.sh

pacat_SOURCES = pacat.c
pacat_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
pacat_CFLAGS = $(AM_CFLAGS) 
pacat_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

paplay_SOURCES = paplay.c
paplay_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la $(LIBSNDFILE_LIBS)
paplay_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)
paplay_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pactl_SOURCES = pactl.c
pactl_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la $(LIBSNDFILE_LIBS)
pactl_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)
pactl_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pacmd_SOURCES = pacmd.c util.c util.h xmalloc.c xmalloc.h log.c log.h pid.c pid.h
pacmd_CFLAGS = $(AM_CFLAGS)
pacmd_LDADD = $(AM_LDADD)
pacmd_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pax11publish_SOURCES = pax11publish.c util.c util.h xmalloc.c xmalloc.h log.c log.h authkey.c authkey.h client-conf.c client-conf.h conf-parser.c conf-parser.h x11prop.c x11prop.h random.c random.h
pax11publish_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
pax11publish_LDADD = $(AM_LDADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)
pax11publish_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pabrowse_SOURCES = pabrowse.c
pabrowse_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la libpolyp-browse-@PA_MAJORMINOR@.la
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
		voltest

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

mainloop_test_SOURCES = mainloop-test.c
mainloop_test_CFLAGS = $(AM_CFLAGS)
mainloop_test_LDADD = $(AM_LDADD) libpolyp-mainloop-@PA_MAJORMINOR@.la libpolyp-@PA_MAJORMINOR@.la
mainloop_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mcalign_test_SOURCES = mcalign-test.c util.c util.h xmalloc.c xmalloc.h log.c log.h mcalign.c mcalign.h memchunk.c memchunk.h memblock.c memblock.h
mcalign_test_CFLAGS = $(AM_CFLAGS)
mcalign_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS)
mcalign_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

pacat_simple_SOURCES = pacat-simple.c
pacat_simple_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-simple-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
pacat_simple_CFLAGS = $(AM_CFLAGS)
pacat_simple_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

parec_simple_SOURCES = parec-simple.c
parec_simple_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-simple-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
parec_simple_CFLAGS = $(AM_CFLAGS)
parec_simple_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

strlist_test_SOURCES = strlist-test.c strlist.c strlist.h strbuf.c strbuf.h util.c util.h xmalloc.c xmalloc.h log.c log.h
strlist_test_CFLAGS = $(AM_CFLAGS)
strlist_test_LDADD = $(AM_LDADD) $(WINSOCK_LIBS)
strlist_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

voltest_SOURCES = voltest.c sample.c volume.c volume.h sample.h
voltest_CFLAGS = $(AM_CFLAGS)
voltest_LDADD = $(AM_LDADD)
voltest_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

cpulimit_test_SOURCES = cpulimit-test.c cpulimit.c util.c log.c cpulimit.h util.h log.h idxset.c idxset.h
cpulimit_test_CFLAGS = $(AM_CFLAGS)
cpulimit_test_LDADD = $(AM_LDADD) libpolyp-mainloop-@PA_MAJORMINOR@.la
cpulimit_test_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

cpulimit_test2_SOURCES = cpulimit-test.c cpulimit.c util.c log.c cpulimit.h util.h log.h idxset.c idxset.h
cpulimit_test2_CFLAGS = $(AM_CFLAGS) -DTEST2
cpulimit_test2_LDADD = $(AM_LDADD) libpolyp-mainloop-@PA_MAJORMINOR@.la
cpulimit_test2_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mainloop_test_glib_SOURCES = $(mainloop_test_SOURCES)
mainloop_test_glib_CFLAGS = $(mainloop_test_CFLAGS) $(GLIB20_CFLAGS) -DGLIB_MAIN_LOOP
mainloop_test_glib_LDADD = $(mainloop_test_LDADD) $(GLIB20_LIBS) libpolyp-mainloop-glib-@PA_MAJORMINOR@.la
mainloop_test_glib_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

mainloop_test_glib12_SOURCES = $(mainloop_test_SOURCES)
mainloop_test_glib12_CFLAGS = $(mainloop_test_CFLAGS) $(GLIB12_CFLAGS) -DGLIB_MAIN_LOOP
mainloop_test_glib12_LDADD = $(mainloop_test_LDADD) $(GLIB12_LIBS) libpolyp-mainloop-glib12-@PA_MAJORMINOR@.la
mainloop_test_glib12_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

###################################
#         Client library          #
###################################

polypinclude_HEADERS = \
		cdecl.h \
		glib-mainloop.h \
		mainloop.h \
		mainloop-api.h \
		mainloop-signal.h \
		polyplib.h \
		polyplib-context.h \
		polyplib-def.h \
		polyplib-error.h \
		polyplib-introspect.h \
		polyplib-operation.h \
		polyplib-scache.h \
		polyplib-simple.h \
		polyplib-stream.h \
		polyplib-subscribe.h \
		polyplib-version.h \
		sample.h

if HAVE_HOWL
polypinclude_HEADERS += \
		polyplib-browser.h
endif

lib_LTLIBRARIES = \
		libpolyp-@PA_MAJORMINOR@.la \
		libpolyp-error-@PA_MAJORMINOR@.la \
		libpolyp-mainloop-@PA_MAJORMINOR@.la \
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

libpolyp_@PA_MAJORMINOR@_la_SOURCES = \
		authkey.c authkey.h \
		cdecl.h \
		client-conf.c client-conf.h \
		conf-parser.c conf-parser.h \
		dllmain.c \
		dynarray.c dynarray.h \
		gcc-printf.h \
		idxset.c idxset.h \
		iochannel.c iochannel.h \
		llist.h \
		log.c log.h \
		mainloop-api.c mainloop-api.h \
		mcalign.c mcalign.h \
		memblock.c memblock.h \
		memchunk.c memchunk.h \
		native-common.h \
		packet.c packet.h \
		parseaddr.c parseaddr.h \
		pdispatch.c pdispatch.h \
		polyplib.h \
		polyplib-context.c polyplib-context.h \
		polyplib-def.h \
		polyplib-internal.h \
		polyplib-introspect.c polyplib-introspect.h \
		polyplib-operation.c polyplib-operation.h \
		polyplib-scache.c polyplib-scache.h \
		polyplib-stream.c polyplib-stream.h \
		polyplib-subscribe.c polyplib-subscribe.h \
		pstream.c pstream.h \
		pstream-util.c pstream-util.h \
		queue.c queue.h \
		random.c random.h \
		sample.c sample.h \
		socket-client.c socket-client.h \
		socket-util.c socket-util.h \
		strbuf.c strbuf.h \
		strlist.c strlist.h \
		tagstruct.c tagstruct.h \
		util.c util.h \
		winsock.h \
		xmalloc.c xmalloc.h \
		channelmap.c channelmap.h \
		volume.c volume.h

if HAVE_X11
libpolyp_@PA_MAJORMINOR@_la_SOURCES += \
		client-conf-x11.c client-conf-x11.h \
		x11prop.c x11prop.h
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

libpolyp_error_@PA_MAJORMINOR@_la_SOURCES = polyplib-error.c polyplib-error.h
libpolyp_error_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_error_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la
libpolyp_error_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_mainloop_@PA_MAJORMINOR@_la_SOURCES = \
		mainloop.c mainloop.h \
		mainloop-api.h mainloop-api.c \
		mainloop-signal.c mainloop-signal.h \
		poll.c poll.h
libpolyp_mainloop_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_mainloop_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la $(WINSOCK_LIBS)
libpolyp_mainloop_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_simple_@PA_MAJORMINOR@_la_SOURCES = polyplib-simple.c polyplib-simple.h 
libpolyp_simple_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_simple_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
libpolyp_simple_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_browse_@PA_MAJORMINOR@_la_SOURCES = polyplib-browser.c polyplib-browser.h 
libpolyp_browse_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(HOWL_CFLAGS)
libpolyp_browse_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la $(HOWL_LIBS)
libpolyp_browse_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0 

libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_SOURCES = glib-mainloop.h glib-mainloop.c
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(GLIB20_CFLAGS)
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-mainloop-@PA_MAJORMINOR@.la $(GLIB20_LIBS)
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_SOURCES = glib-mainloop.h glib12-mainloop.c
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(GLIB12_CFLAGS)
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-mainloop-@PA_MAJORMINOR@.la $(GLIB12_LIBS)
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

###################################
#      Daemon core library        #
###################################

polypinclude_HEADERS += \
		cli-command.h \
		client.h \
		core.h \
		dynarray.h \
		endianmacros.h \
		hashmap.h \
		idxset.h \
		iochannel.h \
		memblock.h \
		memblockq.h \
		memchunk.h \
		modargs.h \
		module.h \
		namereg.h \
		queue.h \
		resampler.h \
		sample-util.h \
		sink.h \
		sink-input.h \
		sioman.h \
		socket-server.h \
		socket-client.h \
		socket-util.h \
		source.h \
		source-output.h \
		strbuf.h \
		tokenizer.h \
		tagstruct.h \
		util.h

lib_LTLIBRARIES += libpolypcore.la

libpolypcore_la_SOURCES = \
		autoload.c autoload.h \
		cli-command.c cli-command.h \
		cli-text.c cli-text.h \
		client.c client.h \
		core.c core.h \
		dllmain.c \
		dynarray.c dynarray.h \
		endianmacros.h \
		g711.c g711.h \
		hashmap.c hashmap.h \
		idxset.c idxset.h \
		log.c log.h \
		mainloop.c mainloop.h \
		mainloop-api.c mainloop-api.h \
		mainloop-signal.c mainloop-signal.h \
		mcalign.c mcalign.h \
		memblock.c memblock.h \
		memblockq.c memblockq.h \
		memchunk.c memchunk.h \
		modargs.c modargs.h \
		modinfo.c modinfo.h \
		module.c module.h \
		namereg.c namereg.h \
		play-memchunk.c play-memchunk.h \
		poll.c poll.h \
		props.c props.h \
		queue.c queue.h \
		random.c random.h \
		resampler.c resampler.h \
		sample.c sample.h \
		sample-util.c sample-util.h \
		scache.c scache.h \
		sconv.c sconv.h \
		sconv-s16be.c sconv-s16be.h \
		sconv-s16le.c sconv-s16le.h \
		sink.c sink.h \
		sink-input.c sink-input.h \
		sioman.c sioman.h \
		sound-file.c sound-file.h \
		sound-file-stream.c sound-file-stream.h \
		source.c source.h \
		source-output.c source-output.h \
		strbuf.c strbuf.h \
		subscribe.c subscripe.h \
		tokenizer.c tokenizer.h \
		util.c util.h \
		winsock.h \
		xmalloc.c xmalloc.h \
		volume.c volume.h \
		channelmap.c channelmap.h

libpolypcore_la_CPPFLAGS = $(AM_CPPFLAGS) $(LIBOIL_CFLAGS)
libpolypcore_la_LDFLAGS = -avoid-version
libpolypcore_la_LIBADD = $(AM_LIBADD) $(LIBLTDL) $(LIBSAMPLERATE_LIBS) $(LIBSNDFILE_LIBS) $(WINSOCK_LIBS) $(LIBOIL_LIBS)

###################################
#   Plug-in support libraries     #
###################################

### Warning! Due to an obscure bug in libtool/automake it is required
### that the libraries in modlib_LTLIBRARIES are specified in-order,
### i.e. libraries near the end of the list depend on libraries near
### the head, and not the other way!

modlib_LTLIBRARIES = \
		libsocket-util.la \
		libiochannel.la \
		libsocket-server.la \
		libsocket-client.la \
		libparseaddr.la \
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

if HAVE_X11
modlib_LTLIBRARIES += \
		libx11wrap.la \
		libx11prop.la
endif

if HAVE_OSS
modlib_LTLIBRARIES += \
		liboss-util.la
endif

if HAVE_ALSA
modlib_LTLIBRARIES += \
		libalsa-util.la
endif

if HAVE_HOWL
modlib_LTLIBRARIES += \
		libhowl-wrap.la
endif

libprotocol_simple_la_SOURCES = protocol-simple.c protocol-simple.h
libprotocol_simple_la_LDFLAGS = -avoid-version
libprotocol_simple_la_LIBADD = $(AM_LIBADD) libpolypcore.la libsocket-server.la libiochannel.la

libsocket_server_la_SOURCES = \
		inet_ntop.c inet_ntop.h \
		socket-server.c socket-server.h
libsocket_server_la_LDFLAGS = -avoid-version
libsocket_server_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la libsocket-util.la $(LIBWRAP_LIBS) $(WINSOCK_LIBS)

libsocket_client_la_SOURCES = socket-client.c socket-client.h
libsocket_client_la_LDFLAGS = -avoid-version
libsocket_client_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la libsocket-util.la libparseaddr.la $(LIBASYNCNS_LIBS) $(WINSOCK_LIBS)
libsocket_client_la_CFLAGS = $(AM_CFLAGS) $(LIBASYNCNS_CFLAGS)

libparseaddr_la_SOURCES = parseaddr.c parseaddr.h
libparseaddr_la_LDFLAGS = -avoid-version
libparseaddr_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libpstream_la_SOURCES = pstream.c pstream.h
libpstream_la_LDFLAGS = -avoid-version
libpstream_la_LIBADD = $(AM_LIBADD) libpolypcore.la libpacket.la libiochannel.la $(WINSOCK_LIBS)

libpstream_util_la_SOURCES = pstream-util.c pstream-util.h
libpstream_util_la_LDFLAGS = -avoid-version
libpstream_util_la_LIBADD = $(AM_LIBADD) libpacket.la libpstream.la libtagstruct.la

libpdispatch_la_SOURCES = pdispatch.c pdispatch.h
libpdispatch_la_LDFLAGS = -avoid-version
libpdispatch_la_LIBADD = $(AM_LIBADD) libtagstruct.la libpolypcore.la

libiochannel_la_SOURCES = iochannel.c iochannel.h
libiochannel_la_LDFLAGS = -avoid-version
libiochannel_la_LIBADD = $(AM_LIBADD) libsocket-util.la libpolypcore.la $(WINSOCK_LIBS)

libpacket_la_SOURCES = packet.c packet.h
libpacket_la_LDFLAGS = -avoid-version
libpacket_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libioline_la_SOURCES = ioline.c ioline.h
libioline_la_LDFLAGS = -avoid-version
libioline_la_LIBADD = $(AM_LIBADD) libiochannel.la libpolypcore.la

libcli_la_SOURCES = cli.c cli.h
libcli_la_CPPFLAGS = $(AM_CPPFLAGS)
libcli_la_LDFLAGS = -avoid-version
libcli_la_LIBADD = $(AM_LIBADD) libiochannel.la libioline.la libpolypcore.la

libstrlist_la_SOURCES = strlist.c strlist.h
libstrlist_la_LDFLAGS = -avoid-version
libstrlist_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libprotocol_cli_la_SOURCES = protocol-cli.c protocol-cli.h
libprotocol_cli_la_LDFLAGS = -avoid-version
libprotocol_cli_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libcli.la libpolypcore.la

libprotocol_http_la_SOURCES = protocol-http.c protocol-http.h
libprotocol_http_la_LDFLAGS = -avoid-version
libprotocol_http_la_LIBADD = $(AM_LIBADD) libsocket-server.la libioline.la libpolypcore.la libiochannel.la

libprotocol_native_la_SOURCES = protocol-native.c protocol-native.h native-common.h
libprotocol_native_la_LDFLAGS = -avoid-version
libprotocol_native_la_LIBADD = $(AM_LIBADD) libsocket-server.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libstrlist.la libpolypcore.la libiochannel.la

libtagstruct_la_SOURCES = tagstruct.c tagstruct.h
libtagstruct_la_LDFLAGS = -avoid-version
libtagstruct_la_LIBADD = $(AM_LIBADD) libpolypcore.la $(WINSOCK_LIBS)

libprotocol_esound_la_SOURCES = protocol-esound.c protocol-esound.h esound.h
libprotocol_esound_la_LDFLAGS = -avoid-version
libprotocol_esound_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libauthkey.la libpolypcore.la

libauthkey_la_SOURCES = authkey.c authkey.h
libauthkey_la_LDFLAGS = -avoid-version
libauthkey_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libauthkey_prop_la_SOURCES = authkey-prop.c authkey-prop.h
libauthkey_prop_la_LDFLAGS = -avoid-version
libauthkey_prop_la_LIBADD = $(AM_LIBADD) libpolypcore.la

libsocket_util_la_SOURCES = socket-util.c socket-util.h
libsocket_util_la_LDFLAGS = -avoid-version
libsocket_util_la_LIBADD = $(AM_LIBADD) $(WINSOCK_LIBS)

# X11

libx11wrap_la_SOURCES = x11wrap.c x11wrap.h
libx11wrap_la_LDFLAGS = -avoid-version
libx11wrap_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
libx11wrap_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)

libx11prop_la_SOURCES = x11prop.c x11prop.h
libx11prop_la_LDFLAGS = -avoid-version
libx11prop_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
libx11prop_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS)

# OSS

liboss_util_la_SOURCES = oss-util.c oss-util.h
liboss_util_la_LDFLAGS = -avoid-version

# ALSA

libalsa_util_la_SOURCES = alsa-util.c alsa-util.h
libalsa_util_la_LDFLAGS = -avoid-version
libalsa_util_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS)
libalsa_util_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

# HOWL

libhowl_wrap_la_SOURCES = howl-wrap.c howl-wrap.h
libhowl_wrap_la_LDFLAGS = -avoid-version
libhowl_wrap_la_LIBADD = $(AM_LIBADD) $(HOWL_LIBS)
libhowl_wrap_la_CFLAGS = $(AM_CFLAGS) $(HOWL_CFLAGS)

###################################
#        Plug-in libraries        #
###################################

modlib_LTLIBRARIES += \
		module-cli.la \
		module-cli-protocol-tcp.la \
		module-cli-protocol-tcp6.la \
		module-simple-protocol-tcp.la \
		module-simple-protocol-tcp6.la \
		module-esound-protocol-tcp.la \
		module-esound-protocol-tcp6.la \
		module-native-protocol-tcp.la \
		module-native-protocol-tcp6.la \
		module-native-protocol-fd.la \
		module-sine.la \
		module-combine.la \
		module-tunnel-sink.la \
		module-tunnel-source.la \
		module-null-sink.la \
		module-esound-sink.la \
		module-http-protocol-tcp.la \
		module-http-protocol-tcp6.la

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
		module-oss.la \
		module-oss-mmap.la
endif

if HAVE_ALSA
modlib_LTLIBRARIES += \
		module-alsa-sink.la \
		module-alsa-source.la
endif

if HAVE_SOLARIS
modlib_LTLIBRARIES += \
		module-solaris.la
endif

if HAVE_HOWL
modlib_LTLIBRARIES += \
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

if OS_IS_WIN32
modlib_LTLIBRARIES += \
		module-waveout.la
endif

# These are generated by a M4 script

SYMDEF_FILES = \
		module-cli-symdef.h \
		module-cli-protocol-tcp-symdef.h \
		module-cli-protocol-tcp6-symdef.h \
		module-cli-protocol-unix-symdef.h \
		module-pipe-sink-symdef.h \
		module-pipe-source-symdef.h \
		module-simple-protocol-tcp-symdef.h \
		module-simple-protocol-tcp6-symdef.h \
		module-simple-protocol-unix-symdef.h \
		module-esound-protocol-tcp-symdef.h \
		module-esound-protocol-tcp6-symdef.h \
		module-esound-protocol-unix-symdef.h \
		module-native-protocol-tcp-symdef.h \
		module-native-protocol-tcp6-symdef.h \
		module-native-protocol-unix-symdef.h \
		module-native-protocol-fd-symdef.h \
		module-sine-symdef.h \
		module-combine-symdef.h \
		module-esound-compat-spawnfd-symdef.h \
		module-esound-compat-spawnpid-symdef.h \
		module-match-symdef.h \
		module-tunnel-sink-symdef.h \
		module-tunnel-source-symdef.h \
		module-null-sink-symdef.h \
		module-esound-sink-symdef.h \
		module-zeroconf-publish-symdef.h \
		module-lirc-symdef.h \
		module-mmkbd-evdev-symdef.h \
		module-http-protocol-tcp-symdef.h \
		module-http-protocol-tcp6-symdef.h \
		module-http-protocol-unix-symdef.h \
		module-x11-bell-symdef.h \
		module-x11-publish-symdef.h \
		module-oss-symdef.h \
		module-oss-mmap-symdef.h \
		module-alsa-sink-symdef.h \
		module-alsa-source-symdef.h \
		module-solaris-symdef.h \
		module-waveout-symdef.h

EXTRA_DIST += $(SYMDEF_FILES)
BUILT_SOURCES += $(SYMDEF_FILES)

$(SYMDEF_FILES): module-defs.h.m4
	$(M4) -Dfname="$@" $< > $@

# Simple protocol

module_simple_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_simple_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-simple.la libsocket-server.la

module_simple_protocol_tcp6_la_SOURCES = module-protocol-stub.c
module_simple_protocol_tcp6_la_CFLAGS = -DUSE_TCP6_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_tcp6_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp6_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-simple.la libsocket-server.la

module_simple_protocol_unix_la_SOURCES = module-protocol-stub.c
module_simple_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_unix_la_LDFLAGS = -module -avoid-version
module_simple_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-simple.la libsocket-server.la libsocket-util.la

# CLI protocol

module_cli_la_SOURCES = module-cli.c
module_cli_la_LDFLAGS = -module -avoid-version
module_cli_la_LIBADD = $(AM_LIBADD) libcli.la libiochannel.la libpolypcore.la

module_cli_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_cli_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_cli_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-cli.la libsocket-server.la

module_cli_protocol_tcp6_la_SOURCES = module-protocol-stub.c
module_cli_protocol_tcp6_la_CFLAGS = -DUSE_TCP6_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_tcp6_la_LDFLAGS = -module -avoid-version
module_cli_protocol_tcp6_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-cli.la libsocket-server.la

module_cli_protocol_unix_la_SOURCES = module-protocol-stub.c
module_cli_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_unix_la_LDFLAGS = -module -avoid-version
module_cli_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-cli.la libsocket-server.la libsocket-util.la

# HTTP protocol

module_http_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_http_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_HTTP $(AM_CFLAGS)
module_http_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_http_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-http.la libsocket-server.la

module_http_protocol_tcp6_la_SOURCES = module-protocol-stub.c
module_http_protocol_tcp6_la_CFLAGS = -DUSE_TCP6_SOCKETS -DUSE_PROTOCOL_HTTP $(AM_CFLAGS)
module_http_protocol_tcp6_la_LDFLAGS = -module -avoid-version
module_http_protocol_tcp6_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-http.la libsocket-server.la

module_http_protocol_unix_la_SOURCES = module-protocol-stub.c
module_http_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_HTTP $(AM_CFLAGS)
module_http_protocol_unix_la_LDFLAGS = -module -avoid-version
module_http_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-http.la libsocket-server.la libsocket-util.la

# Native protocol

module_native_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_native_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_native_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-native.la libsocket-server.la

module_native_protocol_tcp6_la_SOURCES = module-protocol-stub.c
module_native_protocol_tcp6_la_CFLAGS = -DUSE_TCP6_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_tcp6_la_LDFLAGS = -module -avoid-version
module_native_protocol_tcp6_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-native.la libsocket-server.la

module_native_protocol_unix_la_SOURCES = module-protocol-stub.c
module_native_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_unix_la_LDFLAGS = -module -avoid-version
module_native_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-native.la libsocket-server.la libsocket-util.la

module_native_protocol_fd_la_SOURCES = module-native-protocol-fd.c
module_native_protocol_fd_la_CFLAGS = $(AM_CFLAGS)
module_native_protocol_fd_la_LDFLAGS = -module -avoid-version
module_native_protocol_fd_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-native.la libsocket-server.la libsocket-util.la libiochannel.la

# EsounD protocol

module_esound_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_esound_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_esound_protocol_tcp_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-esound.la libsocket-server.la

module_esound_protocol_tcp6_la_SOURCES = module-protocol-stub.c
module_esound_protocol_tcp6_la_CFLAGS = -DUSE_TCP6_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_tcp6_la_LDFLAGS = -module -avoid-version
module_esound_protocol_tcp6_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-esound.la libsocket-server.la

module_esound_protocol_unix_la_SOURCES = module-protocol-stub.c
module_esound_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_unix_la_LDFLAGS = -module -avoid-version
module_esound_protocol_unix_la_LIBADD = $(AM_LIBADD) libpolypcore.la libprotocol-esound.la libsocket-server.la libsocket-util.la

module_esound_compat_spawnfd_la_SOURCES = module-esound-compat-spawnfd.c
module_esound_compat_spawnfd_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnfd_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_esound_compat_spawnpid_la_SOURCES = module-esound-compat-spawnpid.c
module_esound_compat_spawnpid_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnpid_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_esound_sink_la_SOURCES = module-esound-sink.c
module_esound_sink_la_LDFLAGS = -module -avoid-version
module_esound_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la libsocket-client.la libauthkey.la

# Pipes

module_pipe_sink_la_SOURCES = module-pipe-sink.c
module_pipe_sink_la_LDFLAGS = -module -avoid-version
module_pipe_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la

module_pipe_source_la_SOURCES = module-pipe-source.c
module_pipe_source_la_LDFLAGS = -module -avoid-version
module_pipe_source_la_LIBADD = $(AM_LIBADD) libpolypcore.la libiochannel.la

# Fake sources/sinks

module_sine_la_SOURCES = module-sine.c
module_sine_la_LDFLAGS = -module -avoid-version
module_sine_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_null_sink_la_SOURCES = module-null-sink.c
module_null_sink_la_LDFLAGS = -module -avoid-version
module_null_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la

# Couplings

module_combine_la_SOURCES = module-combine.c
module_combine_la_LDFLAGS = -module -avoid-version
module_combine_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_match_la_SOURCES = module-match.c
module_match_la_LDFLAGS = -module -avoid-version
module_match_la_LIBADD = $(AM_LIBADD) libpolypcore.la

module_tunnel_sink_la_SOURCES = module-tunnel.c
module_tunnel_sink_la_CFLAGS = -DTUNNEL_SINK=1 $(AM_CFLAGS)
module_tunnel_sink_la_LDFLAGS = -module -avoid-version
module_tunnel_sink_la_LIBADD = $(AM_LIBADD) libpolypcore.la libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libsocket-util.la libiochannel.la

module_tunnel_source_la_SOURCES = module-tunnel.c
module_tunnel_source_la_LDFLAGS = -module -avoid-version
module_tunnel_source_la_LIBADD = $(AM_LIBADD) libpolypcore.la libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libsocket-util.la libiochannel.la

# X11

module_x11_bell_la_SOURCES = module-x11-bell.c
module_x11_bell_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_bell_la_LDFLAGS = -module -avoid-version
module_x11_bell_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libx11wrap.la

module_x11_publish_la_SOURCES = module-x11-publish.c
module_x11_publish_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_publish_la_LDFLAGS = -module -avoid-version
module_x11_publish_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIBS) libx11wrap.la libauthkey.la libauthkey-prop.la libx11prop.la libstrlist.la

# OSS

module_oss_la_SOURCES = module-oss.c
module_oss_la_LDFLAGS = -module -avoid-version
module_oss_la_LIBADD = $(AM_LIBADD) libiochannel.la liboss-util.la

module_oss_mmap_la_SOURCES = module-oss-mmap.c
module_oss_mmap_la_LDFLAGS = -module -avoid-version
module_oss_mmap_la_LIBADD = $(AM_LIBADD) liboss-util.la

# ALSA

module_alsa_sink_la_SOURCES = module-alsa-sink.c
module_alsa_sink_la_LDFLAGS = -module -avoid-version
module_alsa_sink_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la
module_alsa_sink_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_source_la_SOURCES = module-alsa-source.c
module_alsa_source_la_LDFLAGS = -module -avoid-version
module_alsa_source_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la
module_alsa_source_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

# Solaris

module_solaris_la_SOURCES = module-solaris.c
module_solaris_la_LDFLAGS = -module -avoid-version
module_solaris_la_LIBADD = $(AM_LIBADD) libiochannel.la

# HOWL

module_zeroconf_publish_la_SOURCES = module-zeroconf-publish.c
module_zeroconf_publish_la_LDFLAGS = -module -avoid-version
module_zeroconf_publish_la_LIBADD = $(AM_LIBADD) $(HOWL_LIBS) libhowl-wrap.la
module_zeroconf_publish_la_CFLAGS = $(AM_CFLAGS) $(HOWL_CFLAGS)

# LIRC

module_lirc_la_SOURCES = module-lirc.c
module_lirc_la_LDFLAGS = -module -avoid-version
module_lirc_la_LIBADD = $(AM_LIBADD) $(LIRC_LIBS) 
module_lirc_la_CFLAGS = $(AM_CFLAGS) $(LIRC_CFLAGS)

# Linux evdev

module_mmkbd_evdev_la_SOURCES = module-mmkbd-evdev.c
module_mmkbd_evdev_la_LDFLAGS = -module -avoid-version
module_mmkbd_evdev_la_LIBADD = $(AM_LIBADD)
module_mmkbd_evdev_la_CFLAGS = $(AM_CFLAGS)

# Windows waveout

module_waveout_la_SOURCES = module-waveout.c
module_waveout_la_LDFLAGS = -module -avoid-version
module_waveout_la_LIBADD = $(AM_LIBADD) libpolypcore.la -lwinmm
module_waveout_la_CFLAGS = $(AM_CFLAGS)

###################################
#        Some minor stuff         #
###################################

suid: polypaudio
	chown root $<
	chmod u+s $<

esdcompat.sh: esdcompat.sh.in Makefile
	sed -e 's,@PACKAGE_VERSION\@,$(PACKAGE_VERSION),g' \
		-e 's,@PACKAGE_NAME\@,$(PACKAGE_NAME),g' \
		-e 's,@POLYPAUDIO_BINARY\@,$(POLYPAUDIO_BINARY),g' < $< > $@

client.conf: client.conf.in Makefile
	sed -e 's,@POLYPAUDIO_BINARY\@,$(POLYPAUDIO_BINARY),g' < $< > $@

if OS_IS_WIN32    
default.pa: default.pa.win32
	cp $< $@
else
default.pa: default.pa.in Makefile
	sed -e 's,@POLYPAUDIO_BINARY\@,$(POLYPAUDIO_BINARY),g' < $< > $@
endif

daemon.conf: daemon.conf.in Makefile
	sed -e 's,@DLSEARCHPATH\@,$(modlibdir),g' \
		-e 's,@DEFAULT_CONFIG_FILE\@,$(DEFAULT_CONFIG_DIR),g' < $< > $@

install-exec-hook:
	chown root $(DESTDIR)$(bindir)/polypaudio ; true
	chmod u+s $(DESTDIR)$(bindir)/polypaudio
	ln -sf pacat $(DESTDIR)$(bindir)/parec
