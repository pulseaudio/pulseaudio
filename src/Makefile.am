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
		libpacket.la module-oss.la module-oss-mmap.la liboss.la

polypaudio_SOURCES = idxset.c queue.c strbuf.c mainloop.c  \
		memblock.c sample.c memblockq.c client.c \
		core.c main.c sourceoutput.c sinkinput.c source.c sink.c \
		module.c
polypaudio_INCLUDES = $(INCLTDL)
polypaudio_LDADD = $(LIBLTDL) 
polypaudio_LDFLAGS=-export-dynamic

libprotocol_simple_la_SOURCES = protocol-simple.c
libprotocol_simple_la_LDFLAGS = -avoid-version
libprotocol_simple_la_LIBADD = libsocket-server.la libiochannel.la

libsocket_server_la_SOURCES = socket-server.c
libsocket_server_la_LDFLAGS = -avoid-version
libsocket_server_la_LIBADD = libiochannel.la

libpstream_la_SOURCES = pstream.c
libpstream_la_LDFLAGS = -avoid-version
libpstream_la_LIBADD = libpacket.la

libiochannel_la_SOURCES = iochannel.c
libiochannel_la_LDFLAGS = -avoid-version

libpacket_la_SOURCES = packet.c
libpacket_la_LDFLAGS = -avoid-version

liboss_la_SOURCES = oss.c
liboss_la_LDFLAGS = -avoid-version

module_simple_protocol_tcp_la_SOURCES = module-simple-protocol-tcp.c
module_simple_protocol_tcp_la_LDFLAGS = -module -avoid-version
module_simple_protocol_tcp_la_LIBADD = libprotocol-simple.la libiochannel.la

module_pipe_sink_la_SOURCES = module-pipe-sink.c
module_pipe_sink_la_LDFLAGS = -module -avoid-version
module_pipe_sink_la_LIBADD = libiochannel.la

module_oss_la_SOURCES = module-oss.c
module_oss_la_LDFLAGS = -module -avoid-version
module_oss_la_LIBADD = libiochannel.la liboss.la

module_oss_mmap_la_SOURCES = module-oss-mmap.c
module_oss_mmap_la_LDFLAGS = -module -avoid-version
module_oss_mmap_la_LIBADD = libiochannel.la liboss.la
