# $Id$
#
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

AM_CFLAGS=-ansi -D_GNU_SOURCE

bin_PROGRAMS = polypaudio 

pkglib_LTLIBRARIES=libprotocol-simple.la module-simple-protocol-tcp.la \
		libsocket-server.la module-pipe-sink.la libpstream.la libiochannel.la \
		libpacket.la module-oss.la module-oss-mmap.la liboss.la libioline.la \
		libcli.la module-cli.la libtokenizer.la libdynarray.la \
		module-simple-protocol-unix.la module-cli-protocol-tcp.la \
		libprotocol-cli.la libprotocol-native.la module-native-protocol-tcp.la \
        module-native-protocol-unix.la module-cli-protocol-unix.la libtagstruct.la

polypaudio_SOURCES = idxset.c idxset.h \
		queue.c queue.h \
		strbuf.c strbuf.h \
		mainloop.c mainloop.h \
		memblock.c memblock.h \
		sample.c sample.h \
		memblockq.c memblockq.h \
		client.c client.h \
		core.c core.h \
		main.c main.h \
		sourceoutput.c sourceoutput.h \
		sinkinput.c sinkinput.h \
		source.c source.h \
		sink.c sink.h \
		module.c module.h

polypaudio_INCLUDES = $(INCLTDL)
polypaudio_LDADD = $(LIBLTDL) 
polypaudio_LDFLAGS=-export-dynamic

libprotocol_simple_la_SOURCES = protocol-simple.c protocol-simple.h
libprotocol_simple_la_LDFLAGS = -avoid-version
libprotocol_simple_la_LIBADD = libsocket-server.la libiochannel.la

libsocket_server_la_SOURCES = socket-server.c socket-server.h
libsocket_server_la_LDFLAGS = -avoid-version
libsocket_server_la_LIBADD = libiochannel.la

libpstream_la_SOURCES = pstream.c pstream.h
libpstream_la_LDFLAGS = -avoid-version
libpstream_la_LIBADD = libpacket.la

libiochannel_la_SOURCES = iochannel.c iochannel.h
libiochannel_la_LDFLAGS = -avoid-version

libpacket_la_SOURCES = packet.c packet.h
libpacket_la_LDFLAGS = -avoid-version

liboss_la_SOURCES = oss.c oss.h
liboss_la_LDFLAGS = -avoid-version

libioline_la_SOURCES = ioline.c ioline.h
libioline_la_LDFLAGS = -avoid-version
libioline_la_LIBADD = libiochannel.la

libcli_la_SOURCES = cli.c cli.h
libcli_la_LDFLAGS = -avoid-version
libcli_la_LIBADD = libiochannel.la libioline.la

libdynarray_la_SOURCES = dynarray.c dynarray.h
libdynarray_la_LDFLAGS = -avoid-version

libtokenizer_la_SOURCES = tokenizer.c tokenizer.h
libtokenizer_la_LDFLAGS = -avoid-version
libtokenizer_la_LIBADD = libdynarray.la

libprotocol_cli_la_SOURCES = protocol-cli.c protocol-cli.h
libprotocol_cli_la_LDFLAGS = -avoid-version
libprotocol_cli_la_LIBADD = libsocket-server.la libiochannel.la libcli.la

libprotocol_native_la_SOURCES = protocol-native.c protocol-native.h
libprotocol_native_la_LDFLAGS = -avoid-version
libprotocol_native_la_LIBADD = libsocket-server.la libiochannel.la libpacket.la libpstream.la

libtagstruct_la_SOURCES = tagstruct.c tagstruct.h
libtagstruct_la_LDFLAGS = -avoid-version

module_simple_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_simple_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp_la_LIBADD = libprotocol-simple.la libiochannel.la

module_simple_protocol_unix_la_SOURCES = module-protocol-stub.c
module_simple_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_SIMPLE $(AM_CFLAGS)
module_simple_protocol_unix_la_LDFLAGS = -module -avoid-version
module_simple_protocol_unix_la_LIBADD = libprotocol-simple.la libiochannel.la

module_cli_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_cli_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_cli_protocol_tcp_la_LIBADD = libprotocol-cli.la libiochannel.la

module_cli_protocol_unix_la_SOURCES = module-protocol-stub.c
module_cli_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_CLI $(AM_CFLAGS)
module_cli_protocol_unix_la_LDFLAGS = -module -avoid-version
module_cli_protocol_unix_la_LIBADD = libprotocol-cli.la libiochannel.la

module_native_protocol_tcp_la_SOURCES = module-protocol-stub.c
module_native_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_native_protocol_tcp_la_LIBADD = libprotocol-native.la libiochannel.la libtagstruct.la

module_native_protocol_unix_la_SOURCES = module-protocol-stub.c
module_native_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS -DUSE_PROTOCOL_NATIVE $(AM_CFLAGS)
module_native_protocol_unix_la_LDFLAGS = -module -avoid-version
module_native_protocol_unix_la_LIBADD = libprotocol-native.la libiochannel.la libtagstruct.la

module_pipe_sink_la_SOURCES = module-pipe-sink.c
module_pipe_sink_la_LDFLAGS = -module -avoid-version
module_pipe_sink_la_LIBADD = libiochannel.la

module_oss_la_SOURCES = module-oss.c
module_oss_la_LDFLAGS = -module -avoid-version
module_oss_la_LIBADD = libiochannel.la liboss.la

module_oss_mmap_la_SOURCES = module-oss-mmap.c
module_oss_mmap_la_LDFLAGS = -module -avoid-version
module_oss_mmap_la_LIBADD = libiochannel.la liboss.la

module_cli_la_SOURCES = module-cli.c
module_cli_la_LDFLAGS = -module -avoid-version
module_cli_la_LIBADD = libcli.la libiochannel.la libtokenizer.la
