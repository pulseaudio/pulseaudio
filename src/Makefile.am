# $Id: Makefile.am 27 2003-10-22 22:34:06Z lennart $
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
		module-simple-protocol-unix.la

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

module_simple_protocol_tcp_la_SOURCES = module-simple-protocol.c
module_simple_protocol_tcp_la_CFLAGS = -DUSE_TCP_SOCKETS $(AM_CFLAGS)
module_simple_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp_la_LIBADD = libprotocol-simple.la libiochannel.la

module_simple_protocol_unix_la_SOURCES = module-simple-protocol.c
module_simple_protocol_unix_la_CFLAGS = -DUSE_UNIX_SOCKETS $(AM_CFLAGS)
module_simple_protocol_unix_la_LDFLAGS = -module -avoid-version
module_simple_protocol_unix_la_LIBADD = libprotocol-simple.la libiochannel.la

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
