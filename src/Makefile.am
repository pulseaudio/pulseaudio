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

pkglib_LTLIBRARIES=protocol-simple.la protocol-simple-tcp.la \
		socket-server.la sink-pipe.la pstream.la iochannel.la packet.la

polypaudio_SOURCES = idxset.c queue.c strbuf.c mainloop.c  \
		memblock.c sample.c memblockq.c client.c \
		core.c main.c outputstream.c inputstream.c source.c sink.c \
		module.c
polypaudio_INCLUDES = $(INCLTDL)
polypaudio_LDADD = $(LIBLTDL) 
polypaudio_LDFLAGS=-export-dynamic

protocol_simple_la_SOURCES = protocol-simple.c
protocol_simple_la_LDFLAGS = -module -avoid-version

protocol_simple_tcp_la_SOURCES = protocol-simple-tcp.c
protocol_simple_tcp_la_LDFLAGS = -module -avoid-version
protocol_simple_tcp_la_LIBADD = protocol-simple.la socket-server.la

socket_server_la_SOURCES = socket-server.c
socket_server_la_LDFLAGS = -module -avoid-version

sink_pipe_la_SOURCES = sink-pipe.c
sink_pipe_la_LDFLAGS = -module -avoid-version

pstream_la_SOURCES = pstream.c
pstream_la_LDFLAGS = -module -avoid-version

iochannel_la_SOURCES = pstream.c
iochannel_la_LDFLAGS = -module -avoid-version 

packet_la_SOURCES = pstream.c
packet_la_LDFLAGS = -module -avoid-version 

