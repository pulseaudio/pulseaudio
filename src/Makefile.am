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

pkglib_LTLIBRARIES=libprotocol-simple.la protocol-simple-tcp.la \
		libsocket-server.la sink-pipe.la libpstream.la libiochannel.la libpacket.la

polypaudio_SOURCES = idxset.c queue.c strbuf.c mainloop.c  \
		memblock.c sample.c memblockq.c client.c \
		core.c main.c outputstream.c inputstream.c source.c sink.c \
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

protocol_simple_tcp_la_SOURCES = protocol-simple-tcp.c
protocol_simple_tcp_la_LDFLAGS = -module -avoid-version
protocol_simple_tcp_la_LIBADD = libprotocol-simple.la libiochannel.la

sink_pipe_la_SOURCES = sink-pipe.c
sink_pipe_la_LDFLAGS = -module -avoid-version
sink_pipe_la_LIBADD = libiochannel.la

sink_oss_la_SOURCES = sink-pipe.c
sink_oss_la_LDFLAGS = -module -avoid-version
sink_oss_la_LIBADD = libiochannel.la
