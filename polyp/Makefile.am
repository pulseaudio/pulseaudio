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

AM_CFLAGS=-ansi -D_GNU_SOURCE -DDLSEARCHDIR=\"$(pkglibdir)\" -I$(srcdir)/..
AM_LDADD=-L.
AM_LIBADD=-L.

EXTRA_DIST = polypaudio.pa depmod.py
bin_PROGRAMS = polypaudio pacat pactl
noinst_PROGRAMS = pacat-simple parec-simple

pkginclude_HEADERS=polyplib.h \
		polyplib-def.h \
		polyplib-simple.h \
		polyplib-error.h \
		mainloop-api.h \
		mainloop.h \
		mainloop-signal.h \
		sample.h

pkglib_LTLIBRARIES=libiochannel.la \
		libsocket-server.la \
		libsocket-client.la \
		libpstream.la \
		libpacket.la \
		liboss-util.la \
		libalsa-util.la \
		libioline.la \
		libcli.la \
		libprotocol-cli.la \
		libtagstruct.la \
		libpstream-util.la \
		libpdispatch.la \
		libauthkey.la \
		libsocket-util.la \
		libprotocol-simple.la \
		libprotocol-esound.la \
		libprotocol-native.la \
		module-cli.la \
		module-cli-protocol-tcp.la \
		module-cli-protocol-unix.la \
		module-pipe-sink.la \
		module-alsa-sink.la \
		module-alsa-source.la \
		module-oss.la \
		module-oss-mmap.la \
		module-simple-protocol-tcp.la \
		module-simple-protocol-unix.la \
		module-esound-protocol-tcp.la \
		module-esound-protocol-unix.la \
		module-native-protocol-tcp.la \
		module-native-protocol-unix.la \
		module-x11-bell.la

lib_LTLIBRARIES=libpolyp.la \
		libpolyp-simple.la \
		libpolyp-error.la \
		libpolyp-mainloop.la

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
		clitext.c clitext.h \
		tokenizer.c tokenizer.h \
		dynarray.c dynarray.h \
		scache.c scache.h \
		sound-file.c sound-file.h \
		play-memchunk.c play-memchunk.h

polypaudio_CFLAGS = $(AM_CFLAGS) $(LIBSAMPLERATE_CFLAGS) $(LIBSNDFILE_CFLAGS)
polypaudio_INCLUDES = $(INCLTDL)
polypaudio_LDADD = $(AM_LDADD) $(LIBLTDL) $(LIBSAMPLERATE_LIBS) $(LIBSNDFILE_LIBS)
polypaudio_LDFLAGS=-export-dynamic

libprotocol_simple_la_SOURCES = protocol-simple.c protocol-simple.h
libprotocol_simple_la_LDFLAGS = -avoid-version
libprotocol_simple_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la

libsocket_server_la_SOURCES = socket-server.c socket-server.h
libsocket_server_la_LDFLAGS = -avoid-version
libsocket_server_la_LIBADD = $(AM_LIBADD) libiochannel.la libsocket-util.la

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

libalsa_util_la_SOURCES = alsa-util.c alsa-util.h
libalsa_util_la_LDFLAGS = -avoid-version
libalsa_util_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS)
libalsa_util_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

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
libprotocol_native_la_LIBADD = $(AM_LIBADD) libsocket-server.la libpstream.la libpstream-util.la libpdispatch.la libtagstruct.la libauthkey.la

libtagstruct_la_SOURCES = tagstruct.c tagstruct.h
libtagstruct_la_LDFLAGS = -avoid-version

libprotocol_esound_la_SOURCES = protocol-esound.c protocol-esound.h esound.h
libprotocol_esound_la_LDFLAGS = -avoid-version
libprotocol_esound_la_LIBADD = $(AM_LIBADD) libsocket-server.la libiochannel.la libauthkey.la

libauthkey_la_SOURCES = authkey.c authkey.h
libauthkey_la_LDFLAGS = -avoid-version

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

module_alsa_sink_la_SOURCES = module-alsa-sink.c
module_alsa_sink_la_LDFLAGS = -module -avoid-version
module_alsa_sink_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la
module_alsa_sink_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_alsa_source_la_SOURCES = module-alsa-source.c
module_alsa_source_la_LDFLAGS = -module -avoid-version
module_alsa_source_la_LIBADD = $(AM_LIBADD) $(ASOUNDLIB_LIBS) libalsa-util.la
module_alsa_source_la_CFLAGS = $(AM_CFLAGS) $(ASOUNDLIB_CFLAGS)

module_oss_la_SOURCES = module-oss.c
module_oss_la_LDFLAGS = -module -avoid-version
module_oss_la_LIBADD = $(AM_LIBADD) libiochannel.la liboss-util.la

module_oss_mmap_la_SOURCES = module-oss-mmap.c
module_oss_mmap_la_LDFLAGS = -module -avoid-version
module_oss_mmap_la_LIBADD = $(AM_LIBADD) liboss-util.la

module_cli_la_SOURCES = module-cli.c
module_cli_la_LDFLAGS = -module -avoid-version
module_cli_la_LIBADD = $(AM_LIBADD) libcli.la libiochannel.la

module_x11_bell_la_SOURCES = module-x11-bell.c
module_x11_bell_la_LDFLAGS = -module -avoid-version
module_x11_bell_la_LIBADD = $(AM_LIBADD) -lX11 -L/usr/X11R6/lib

libpolyp_la_SOURCES = polyplib.c polyplib.h \
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
		sample.c sample.h
libpolyp_la_CFLAGS = $(AM_CFLAGS)

libpolyp_mainloop_la_SOURCES = mainloop-api.h mainloop-api.c \
		mainloop.c mainloop.h \
		mainloop-signal.c mainloop-signal.h
libpolyp_mainloop_la_CFLAGS = $(AM_CFLAGS)

libpolyp_error_la_SOURCES = polyplib-error.c polyplib-error.h
libpolyp_error_la_CFLAGS = $(AM_CFLAGS)

libpolyp_simple_la_SOURCES = polyplib-simple.c polyplib-simple.h 
libpolyp_simple_la_CFLAGS = $(AM_CFLAGS)
libpolyp_simple_la_LIBADD = $(AM_LIBADD) libpolyp.la libpolyp-mainloop.la

pacat_SOURCES = pacat.c
pacat_LDADD = $(AM_LDADD) libpolyp.la libpolyp-error.la libpolyp-mainloop.la
pacat_CFLAGS = $(AM_CFLAGS) 

pactl_SOURCES = pactl.c
pactl_LDADD = $(AM_LDADD) libpolyp.la libpolyp-error.la libpolyp-mainloop.la $(LIBSNDFILE_LIBS)
pactl_CFLAGS = $(AM_CFLAGS) $(LIBSDNFILE_CFLAGS)

pacat_simple_SOURCES = pacat-simple.c
pacat_simple_LDADD = $(AM_LDADD) libpolyp-simple.la libpolyp-error.la
pacat_simple_CFLAGS = $(AM_CFLAGS)

parec_simple_SOURCES = parec-simple.c
parec_simple_LDADD = $(AM_LDADD) libpolyp-simple.la libpolyp-error.la
parec_simple_CFLAGS = $(AM_CFLAGS)

if BUILD_LIBPOLYPCORE

pkginclude_HEADERS+=cli-command.h\
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
		clitext.c clitext.h \
		tokenizer.c tokenizer.h \
		dynarray.c dynarray.h

endif
