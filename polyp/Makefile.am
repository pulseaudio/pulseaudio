# $Id$
#
# This file is part of polypaudio.
#
# polypaudio is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# polypaudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with polypaudio; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
# USA.

polypincludedir=$(includedir)/polyp
polypconfdir=$(sysconfdir)/polypaudio

modlibdir=$(libdir)/polypaudio-@PA_MAJORMINOR@

AM_CFLAGS=-D_GNU_SOURCE  -I$(top_srcdir) $(PTHREAD_CFLAGS)
AM_CFLAGS+=-DDLSEARCHPATH=\"$(modlibdir)\"
AM_CFLAGS+=-DDEFAULT_CONFIG_DIR=\"$(polypconfdir)\"
AM_CFLAGS+=-DPOLYPAUDIO_BINARY=\"$(bindir)/polypaudio\"

AM_LDADD=$(PTHREAD_LIBS) -lm
AM_LIBADD=$(PTHREAD_LIBS) -lm

EXTRA_DIST = default.pa.in daemon.conf.in client.conf.in depmod.py esdcompat.sh.in module-defs.h.m4
bin_PROGRAMS = polypaudio pacat pactl paplay pax11publish
bin_SCRIPTS = esdcompat.sh
noinst_PROGRAMS = \
		mainloop-test \
		pacat-simple \
		parec-simple \
		cpulimit-test \
		cpulimit-test2 \
		voltest

polypconf_DATA=default.pa daemon.conf client.conf

BUILT_SOURCES=polyplib-version.h

polypinclude_HEADERS= \
		polyplib.h \
		polyplib-def.h \
		polyplib-simple.h \
		polyplib-error.h \
		polyplib-stream.h \
		polyplib-context.h \
		polyplib-introspect.h \
		polyplib-subscribe.h \
		polyplib-operation.h \
		polyplib-scache.h \
		polyplib-version.h \
		cdecl.h \
		mainloop-api.h \
		mainloop.h \
		mainloop-signal.h \
		sample.h \
		glib-mainloop.h

### Warning! Due to an obscure bug in libtool/automake it is required
### that the libraries in modlib_LTLIBRARIES are specified in-order,
### i.e. libraries near the end of the list depend on libraries near
### the head, and not the other way!

modlib_LTLIBRARIES= \
		libsocket-util.la \
		libiochannel.la \
		libsocket-server.la \
		libsocket-client.la \
		libpacket.la \
		libpstream.la \
		liboss-util.la \
		libioline.la \
		libcli.la \
		libprotocol-cli.la \
		libtagstruct.la \
		libpstream-util.la \
		libpdispatch.la \
		libauthkey.la \
		libauthkey-prop.la \
		libprotocol-simple.la \
		libprotocol-esound.la \
		libprotocol-native.la \
		module-cli.la \
		module-cli-protocol-tcp.la \
		module-cli-protocol-unix.la \
		module-pipe-sink.la \
		module-pipe-source.la \
		module-oss.la \
		module-oss-mmap.la \
		module-simple-protocol-tcp.la \
		module-simple-protocol-unix.la \
		module-esound-protocol-tcp.la \
		module-esound-protocol-unix.la \
		module-native-protocol-tcp.la \
		module-native-protocol-unix.la \
		module-native-protocol-fd.la \
		module-sine.la \
		module-combine.la \
		module-esound-compat-spawnfd.la \
		module-esound-compat-spawnpid.la \
		module-match.la \
		module-tunnel-sink.la \
		module-tunnel-source.la \
		module-null-sink.la

SYMDEF_FILES= \
		module-cli-symdef.h \
		module-cli-protocol-tcp-symdef.h \
		module-cli-protocol-unix-symdef.h \
		module-pipe-sink-symdef.h \
		module-pipe-source-symdef.h \
		module-oss-symdef.h \
		module-oss-mmap-symdef.h \
		module-simple-protocol-tcp-symdef.h \
		module-simple-protocol-unix-symdef.h \
		module-esound-protocol-tcp-symdef.h \
		module-esound-protocol-unix-symdef.h \
		module-native-protocol-tcp-symdef.h \
		module-native-protocol-unix-symdef.h \
		module-native-protocol-fd-symdef.h \
		module-sine-symdef.h \
		module-combine-symdef.h \
		module-esound-compat-spawnfd-symdef.h \
		module-esound-compat-spawnpid-symdef.h \
		module-match-symdef.h \
		module-tunnel-sink-symdef.h \
		module-tunnel-source-symdef.h \
		module-null-sink-symdef.h

EXTRA_DIST+=$(SYMDEF_FILES)
BUILT_SOURCES+=$(SYMDEF_FILES)

lib_LTLIBRARIES= \
		libpolyp-@PA_MAJORMINOR@.la \
		libpolyp-error-@PA_MAJORMINOR@.la \
		libpolyp-mainloop-@PA_MAJORMINOR@.la \
		libpolyp-simple-@PA_MAJORMINOR@.la

polypaudio_SOURCES = idxset.c idxset.h \
		queue.c queue.h \
		strbuf.c strbuf.h \
		main.c \
		mainloop.c mainloop.h \
		memblock.c memblock.h \
		sample.c sample.h \
		sample-util.c sample-util.h \
		memblockq.c memblockq.h \
		client.c client.h \
		core.c core.h \
		source-output.c source-output.h \
		sink-input.c sink-input.h \
		source.c source.h \
		sink.c sink.h \
		module.c module.h \
		mainloop-signal.c mainloop-signal.h \
		mainloop-api.c mainloop-api.h \
		util.c util.h \
		hashmap.c hashmap.h \
		namereg.c namereg.h \
		sconv.c sconv.h \
		resampler.c resampler.h \
		endianmacros.h \
		memchunk.c memchunk.h \
		sconv-s16le.c sconv-s16le.h \
		sconv-s16be.c sconv-s16be.h \
		sioman.c sioman.h \
		modargs.c modargs.h \
		cmdline.c cmdline.h \
		cli-command.c cli-command.h \
		cli-text.c cli-text.h \
		tokenizer.c tokenizer.h \
		dynarray.c dynarray.h \
		scache.c scache.h \
		sound-file.c sound-file.h \
		play-memchunk.c play-memchunk.h \
		autoload.c autoload.h \
		xmalloc.c xmalloc.h \
		subscribe.h subscribe.c \
		debug.h \
		sound-file-stream.c sound-file-stream.h \
		cpulimit.c cpulimit.h \
		log.c log.h \
		gcc-printf.h \
		modinfo.c modinfo.h \
		daemon-conf.c daemon-conf.h \
		dumpmodules.c dumpmodules.h \
		conf-parser.h conf-parser.c \
		caps.h caps.c \
		props.h props.c

polypaudio_CFLAGS = $(AM_CFLAGS) $(LIBSAMPLERATE_CFLAGS) $(LIBSNDFILE_CFLAGS)
polypaudio_INCLUDES = $(INCLTDL)
polypaudio_LDADD = $(AM_LDADD) $(LIBLTDL) $(LIBSAMPLERATE_LIBS) $(LIBSNDFILE_LIBS) $(CAP_LIBS)
polypaudio_LDFLAGS= -export-dynamic -dlopen force
# -static $(foreach f,$(modlib_LTLIBRARIES),-dlpreopen $(f))

libprotocol_simple_la_SOURCES = protocol-simple.c protocol-simple.h
libprotocol_simple_la_LDFLAGS = -avoid-version
libprotocol_simple_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la

libsocket_server_la_SOURCES = socket-server.c socket-server.h
libsocket_server_la_LDFLAGS = -avoid-version
libsocket_server_la_LIBADD = $(AM_LIBADD) libiochannel.la libsocket-util.la $(LIBWRAP_LIBS)

libsocket_client_la_SOURCES = socket-client.c socket-client.h
libsocket_client_la_LDFLAGS = -avoid-version
libsocket_client_la_LIBADD = $(AM_LIBADD) libiochannel.la libsocket-util.la

libpstream_la_SOURCES = pstream.c pstream.h
libpstream_la_LDFLAGS = -avoid-version
libpstream_la_LIBADD = $(AM_LIBADD) libpacket.la libiochannel.la

libpstream_util_la_SOURCES = pstream-util.c pstream-util.h
libpstream_util_la_LDFLAGS = -avoid-version
libpstream_util_la_LIBADD = $(AM_LIBADD) libpacket.la libpstream.la libtagstruct.la

libpdispatch_la_SOURCES = pdispatch.c pdispatch.h
libpdispatch_la_LDFLAGS = -avoid-version
libpdispatch_la_LIBADD = $(AM_LIBADD) libtagstruct.la

libiochannel_la_SOURCES = iochannel.c iochannel.h
libiochannel_la_LDFLAGS = -avoid-version
libiochannel_la_LIBADD = $(AM_LIBADD) libsocket-util.la

libpacket_la_SOURCES = packet.c packet.h
libpacket_la_LDFLAGS = -avoid-version

liboss_util_la_SOURCES = oss-util.c oss-util.h
liboss_util_la_LDFLAGS = -avoid-version

libioline_la_SOURCES = ioline.c ioline.h
libioline_la_LDFLAGS = -avoid-version
libioline_la_LIBADD = $(AM_LIBADD) libiochannel.la

libcli_la_SOURCES = cli.c cli.h
libcli_la_LDFLAGS = -avoid-version
libcli_la_LIBADD = $(AM_LIBADD) libiochannel.la libioline.la

libprotocol_cli_la_SOURCES = protocol-cli.c protocol-cli.h
libprotocol_cli_la_LDFLAGS = -avoid-version
libprotocol_cli_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libcli.la

libprotocol_native_la_SOURCES = protocol-native.c protocol-native.h native-common.h
libprotocol_native_la_LDFLAGS = -avoid-version
libprotocol_native_la_LIBADD = $(AM_LIBADD) libsocket-server.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la

libtagstruct_la_SOURCES = tagstruct.c tagstruct.h
libtagstruct_la_LDFLAGS = -avoid-version

libprotocol_esound_la_SOURCES = protocol-esound.c protocol-esound.h esound.h
libprotocol_esound_la_LDFLAGS = -avoid-version
libprotocol_esound_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libauthkey.la

libauthkey_la_SOURCES = authkey.c authkey.h
libauthkey_la_LDFLAGS = -avoid-version

libauthkey_prop_la_SOURCES = authkey-prop.c authkey-prop.h
libauthkey_prop_la_LDFLAGS = -avoid-version

libsocket_util_la_SOURCES = socket-util.c socket-util.h
libsocket_util_la_LDFLAGS = -avoid-version

module_simple_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_simple_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp_la_LIBADD = $(AM_LIBADD) libprotocol-simple.la libsocket-server.la

module_simple_protocol_unix_la_SOURCES = module-protocol-stub.c
module_simple_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_unix_la_LDFLAGS = -module -avoid-version
module_simple_protocol_unix_la_LIBADD = $(AM_LIBADD) libprotocol-simple.la libsocket-server.la libsocket-util.la

module_cli_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_cli_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_cli_protocol_tcp_la_LIBADD = $(AM_LIBADD) libprotocol-cli.la libsocket-server.la

module_cli_protocol_unix_la_SOURCES = module-protocol-stub.c
module_cli_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_unix_la_LDFLAGS = -module -avoid-version
module_cli_protocol_unix_la_LIBADD = $(AM_LIBADD) libprotocol-cli.la libsocket-server.la libsocket-util.la

module_native_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_native_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_native_protocol_tcp_la_LIBADD = $(AM_LIBADD) libprotocol-native.la libsocket-server.la

module_native_protocol_unix_la_SOURCES = module-protocol-stub.c
module_native_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_unix_la_LDFLAGS = -module -avoid-version
module_native_protocol_unix_la_LIBADD = $(AM_LIBADD) libprotocol-native.la libsocket-server.la libsocket-util.la

module_native_protocol_fd_la_SOURCES = module-native-protocol-fd.c
module_native_protocol_fd_la_CFLAGS = $(AM_CFLAGS)
module_native_protocol_fd_la_LDFLAGS = -module -avoid-version
module_native_protocol_fd_la_LIBADD = $(AM_LIBADD) libprotocol-native.la libsocket-server.la libsocket-util.la

module_esound_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_esound_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_esound_protocol_tcp_la_LIBADD = $(AM_LIBADD) libprotocol-esound.la libsocket-server.la

module_esound_protocol_unix_la_SOURCES = module-protocol-stub.c
module_esound_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_ESOUND $(AM_CFLAGS)
module_esound_protocol_unix_la_LDFLAGS = -module -avoid-version
module_esound_protocol_unix_la_LIBADD = $(AM_LIBADD) libprotocol-esound.la libsocket-server.la libsocket-util.la

module_pipe_sink_la_SOURCES = module-pipe-sink.c
module_pipe_sink_la_LDFLAGS = -module -avoid-version
module_pipe_sink_la_LIBADD = $(AM_LIBADD) libiochannel.la

module_pipe_source_la_SOURCES = module-pipe-source.c
module_pipe_source_la_LDFLAGS = -module -avoid-version
module_pipe_source_la_LIBADD = $(AM_LIBADD) libiochannel.la

module_oss_la_SOURCES = module-oss.c
module_oss_la_LDFLAGS = -module -avoid-version
module_oss_la_LIBADD = $(AM_LIBADD) libiochannel.la liboss-util.la

module_oss_mmap_la_SOURCES = module-oss-mmap.c
module_oss_mmap_la_LDFLAGS = -module -avoid-version
module_oss_mmap_la_LIBADD = $(AM_LIBADD) liboss-util.la

module_cli_la_SOURCES = module-cli.c
module_cli_la_LDFLAGS = -module -avoid-version
module_cli_la_LIBADD = $(AM_LIBADD) libcli.la libiochannel.la

module_sine_la_SOURCES = module-sine.c
module_sine_la_LDFLAGS = -module -avoid-version
module_sine_la_LIBADD = $(AM_LIBADD)

module_combine_la_SOURCES = module-combine.c
module_combine_la_LDFLAGS = -module -avoid-version
module_combine_la_LIBADD = $(AM_LIBADD)

module_null_sink_la_SOURCES = module-null-sink.c
module_null_sink_la_LDFLAGS = -module -avoid-version
module_null_sink_la_LIBADD = $(AM_LIBADD)

module_match_la_SOURCES = module-match.c
module_match_la_LDFLAGS = -module -avoid-version
module_match_la_LIBADD = $(AM_LIBADD)

module_tunnel_sink_la_SOURCES = module-tunnel.c
module_tunnel_sink_la_CFLAGS = -DTUNNEL_SINK=1 $(AM_CFLAGS)
module_tunnel_sink_la_LDFLAGS = -module -avoid-version
module_tunnel_sink_la_LIBADD = $(AM_LIBADD) libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libsocket-util.la libiochannel.la

module_tunnel_source_la_SOURCES = module-tunnel.c
module_tunnel_source_la_LDFLAGS = -module -avoid-version
module_tunnel_source_la_LIBADD = $(AM_LIBADD) libsocket-client.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la libauthkey-prop.la libsocket-util.la libiochannel.la

module_esound_compat_spawnfd_la_SOURCES = module-esound-compat-spawnfd.c
module_esound_compat_spawnfd_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnfd_la_LIBADD = $(AM_LIBADD)

module_esound_compat_spawnpid_la_SOURCES = module-esound-compat-spawnpid.c
module_esound_compat_spawnpid_la_LDFLAGS = -module -avoid-version
module_esound_compat_spawnpid_la_LIBADD = $(AM_LIBADD)

libpolyp_@PA_MAJORMINOR@_la_SOURCES = polyplib.h \
		polyplib-def.h \
		tagstruct.c tagstruct.h \
		iochannel.c iochannel.h \
		pstream.c pstream.h \
		pstream-util.c pstream-util.h \
		pdispatch.c pdispatch.h \
		mainloop-api.c mainloop-api.h \
		idxset.c idxset.h \
		util.c util.h \
		memblock.c memblock.h \
		socket-client.c socket-client.h \
		packet.c packet.h \
		queue.c queue.h \
		dynarray.c dynarray.h \
		memchunk.c memchunk.h \
		authkey.c authkey.h \
		socket-util.c socket-util.h \
		native-common.h \
		sample.c sample.h \
		xmalloc.c xmalloc.h \
		polyplib-operation.c polyplib-operation.h \
		polyplib-context.c polyplib-context.h \
		polyplib-stream.c polyplib-stream.h \
		polyplib-introspect.c polyplib-introspect.h \
		polyplib-scache.c polyplib-scache.h \
		polyplib-subscribe.c polyplib-subscribe.h \
		polyplib-internal.h \
		cdecl.h \
		llist.h \
		log.c log.h \
		gcc-printf.h \
		client-conf.c client-conf.h \
		conf-parser.c conf-parser.h

libpolyp_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_mainloop_@PA_MAJORMINOR@_la_SOURCES = mainloop-api.h mainloop-api.c \
		mainloop.c mainloop.h \
		mainloop-signal.c mainloop-signal.h
libpolyp_mainloop_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_mainloop_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la
libpolyp_mainloop_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_error_@PA_MAJORMINOR@_la_SOURCES = polyplib-error.c polyplib-error.h
libpolyp_error_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_error_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la
libpolyp_error_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

libpolyp_simple_@PA_MAJORMINOR@_la_SOURCES = polyplib-simple.c polyplib-simple.h 
libpolyp_simple_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpolyp_simple_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
libpolyp_simple_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

pacat_SOURCES = pacat.c
pacat_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
pacat_CFLAGS = $(AM_CFLAGS) 

paplay_SOURCES = paplay.c
paplay_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la $(LIBSNDFILE_LIBS)
paplay_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)

pactl_SOURCES = pactl.c
pactl_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la $(LIBSNDFILE_LIBS)
pactl_CFLAGS = $(AM_CFLAGS) $(LIBSNDFILE_CFLAGS)

pacat_simple_SOURCES = pacat-simple.c
pacat_simple_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-simple-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
pacat_simple_CFLAGS = $(AM_CFLAGS)

parec_simple_SOURCES = parec-simple.c
parec_simple_LDADD = $(AM_LDADD) libpolyp-@PA_MAJORMINOR@.la libpolyp-simple-@PA_MAJORMINOR@.la libpolyp-error-@PA_MAJORMINOR@.la libpolyp-mainloop-@PA_MAJORMINOR@.la
parec_simple_CFLAGS = $(AM_CFLAGS)

pax11publish_SOURCES = pax11publish.c util.c xmalloc.c log.c authkey.c client-conf.c conf-parser.c
pax11publish_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
pax11publish_LDADD = $(AM_LDADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIB)

mainloop_test_SOURCES = mainloop-test.c
mainloop_test_CFLAGS = $(AM_CFLAGS)
mainloop_test_LDADD = $(AM_LDADD) libpolyp-mainloop-@PA_MAJORMINOR@.la libpolyp-@PA_MAJORMINOR@.la

voltest_SOURCES = voltest.c sample.c
voltest_CFLAGS = $(AM_CFLAGS)
voltest_LDADD = $(AM_LDADD)

cpulimit_test_SOURCES = cpulimit-test.c cpulimit.c util.c log.c
cpulimit_test_CFLAGS = $(AM_CFLAGS)
cpulimit_test_LDADD = $(AM_LDADD) libpolyp-mainloop-@PA_MAJORMINOR@.la

cpulimit_test2_SOURCES = cpulimit-test.c cpulimit.c util.c log.c
cpulimit_test2_CFLAGS = $(AM_CFLAGS) -DTEST2
cpulimit_test2_LDADD = $(AM_LDADD) libpolyp-mainloop-@PA_MAJORMINOR@.la

### X11 stuff

if HAVE_X11
modlib_LTLIBRARIES+= \
		libx11wrap.la \
		module-x11-bell.la \
		module-x11-publish.la
SYMDEF_FILES += \
		module-x11-bell-symdef.h \
		module-x11-publish-symdef.h

libx11wrap_la_SOURCES = x11wrap.c x11wrap.h
libx11wrap_la_LDFLAGS = -avoid-version
libx11wrap_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
libx11wrap_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIB)

module_x11_bell_la_SOURCES = module-x11-bell.c
module_x11_bell_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_bell_la_LDFLAGS = -module -avoid-version
module_x11_bell_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIB) libx11wrap.la

module_x11_publish_la_SOURCES = module-x11-publish.c
module_x11_publish_la_CFLAGS = $(AM_CFLAGS) $(X_CFLAGS)
module_x11_publish_la_LDFLAGS = -module -avoid-version
module_x11_publish_la_LIBADD = $(AM_LIBADD) $(X_PRE_LIBS) -lX11 $(X_LIBS) $(X_EXTRA_LIB) libx11wrap.la libauthkey.la libauthkey-prop.la

endif

### ALSA modules

if HAVE_ALSA
modlib_LTLIBRARIES+= \
		libalsa-util.la \
		module-alsa-sink.la \
		module-alsa-source.la
SYMDEF_FILES += \
		module-alsa-sink-symdef.h \
		module-alsa-source-symdef.h

libalsa_util_la_SOURCES = alsa-util.c alsa-util.h
libalsa_util_la_LDFLAGS = -avoid-version
libalsa_util_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS)
libalsa_util_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_sink_la_SOURCES = module-alsa-sink.c
module_alsa_sink_la_LDFLAGS = -module -avoid-version
module_alsa_sink_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la
module_alsa_sink_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_source_la_SOURCES = module-alsa-source.c
module_alsa_source_la_LDFLAGS = -module -avoid-version
module_alsa_source_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la
module_alsa_source_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)
endif

### GLIB 2.0 support

if HAVE_GLIB20
lib_LTLIBRARIES+= \
		libpolyp-mainloop-glib-@PA_MAJORMINOR@.la

noinst_PROGRAMS+= \
		mainloop-test-glib

libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_SOURCES = glib-mainloop.h glib-mainloop.c
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(GLIB20_CFLAGS)
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-mainloop-@PA_MAJORMINOR@.la $(GLIB20_LIBS)
libpolyp_mainloop_glib_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

mainloop_test_glib_SOURCES = $(mainloop_test_SOURCES)
mainloop_test_glib_CFLAGS = $(mainloop_test_CFLAGS) $(GLIB20_CFLAGS) -DGLIB_MAIN_LOOP
mainloop_test_glib_LDADD = $(mainloop_test_LDADD) $(GLIB20_LIBS) libpolyp-mainloop-glib-@PA_MAJORMINOR@.la
endif

### GLIB 1.2 support

if HAVE_GLIB12

lib_LTLIBRARIES+= \
		libpolyp-mainloop-glib12-@PA_MAJORMINOR@.la

noinst_PROGRAMS+= \
		mainloop-test-glib12

libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_SOURCES = glib-mainloop.h glib12-mainloop.c
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(GLIB12_CFLAGS)
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) libpolyp-mainloop-@PA_MAJORMINOR@.la $(GLIB12_LIBS)
libpolyp_mainloop_glib12_@PA_MAJORMINOR@_la_LDFLAGS = -version-info 0:0:0

mainloop_test_glib12_SOURCES = $(mainloop_test_SOURCES)
mainloop_test_glib12_CFLAGS = $(mainloop_test_CFLAGS) $(GLIB12_CFLAGS) -DGLIB_MAIN_LOOP
mainloop_test_glib12_LDADD = $(mainloop_test_LDADD) $(GLIB12_LIBS) libpolyp-mainloop-glib12-@PA_MAJORMINOR@.la

endif

### libpolypcore (needs to be updated)

if BUILD_LIBPOLYPCORE

polypinclude_HEADERS+=cli-command.h\
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

lib_LTLIBRARIES+= libpolypcore.la

libpolypcore_la_SOURCES = idxset.c idxset.h \
		queue.c queue.h \
		strbuf.c strbuf.h \
		mainloop.c mainloop.h \
		memblock.c memblock.h \
		sample.c sample.h \
		sample-util.c sample-util.h \
		memblockq.c memblockq.h \
		client.c client.h \
		core.c core.h \
		source-output.c source-output.h \
		sink-input.c sink-input.h \
		source.c source.h \
		sink.c sink.h \
		module.c module.h \
		mainloop-signal.c mainloop-signal.h \
		mainloop-api.c mainloop-api.h \
		util.c util.h \
		hashmap.c hashmap.h \
		namereg.c namereg.h \
		sconv.c sconv.h \
		resampler.c resampler.h \
		endianmacros.h \
		memchunk.c memchunk.h \
		sconv-s16le.c sconv-s16le.h \
		sconv-s16be.c sconv-s16be.h \
		sioman.c sioman.h \
		modargs.c modargs.h \
		cli-command.c cli-command.h \
		cli-text.c cli-text.h \
		tokenizer.c tokenizer.h \
		dynarray.c dynarray.h

endif

### Some minor stuff

suid: polypaudio
	chown root $<
	chmod u+s $<

esdcompat.sh: esdcompat.sh.in Makefile
	sed -e 's,@PACKAGE_VERSION\@,$(PACKAGE_VERSION),g' \
		-e 's,@PACKAGE_NAME\@,$(PACKAGE_NAME),g' \
		-e 's,@POLYPAUDIO_BINARY\@,$(bindir)/polypaudio,g' < $< > $@

client.conf: client.conf.in Makefile
	sed -e 's,@POLYPAUDIO_BINARY\@,$(bindir)/polypaudio,g' < $< > $@

default.pa: default.pa.in Makefile
	sed -e 's,@POLYPAUDIO_BINARY\@,$(bindir)/polypaudio,g' < $< > $@

daemon.conf: daemon.conf.in Makefile
	sed -e 's,@DLSEARCHPATH\@,$(modlibdir),g' \
		-e 's,@DEFAULT_CONFIG_FILE\@,$(polypconfdir)/daemon.conf,g' < $< > $@

install-exec-hook:
	chown root $(DESTDIR)$(bindir)/polypaudio
	chmod u+s $(DESTDIR)$(bindir)/polypaudio

$(SYMDEF_FILES): module-defs.h.m4
	$(M4) -Dfname="$@" $< > $@
