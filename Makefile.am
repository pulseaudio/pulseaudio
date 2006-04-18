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

EXTRA_DIST = bootstrap.sh LICENSE doxygen/Makefile.am doxygen/Makefile.in doxygen/doxygen.conf.in libtool.m4 ltdl.m4
SUBDIRS=libltdl src doc doxygen

MAINTAINERCLEANFILES =
noinst_DATA =

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = polyplib.pc polyplib-simple.pc polyplib-browse.pc

if HAVE_GLIB20
pkgconfig_DATA += \
	 polyplib-glib-mainloop.pc
endif

if HAVE_GLIB12
pkgconfig_DATA += \
	polyplib-glib12-mainloop.pc
endif

if USE_LYNX
EXTRA_DIST += README
MAINTAINERCLEANFILES += README
noinst_DATA += README

README:
	rm -f README
	$(MAKE) -C doc README
	cd $(srcdir) && ln -s doc/README README
endif

homepage: all dist doxygen
	test -d $$HOME/homepage/private
	mkdir -p $$HOME/homepage/private/projects/polypaudio $$HOME/homepage/private/projects/polypaudio/doxygen
	cp polypaudio-@PACKAGE_VERSION@.tar.gz $$HOME/homepage/private/projects/polypaudio
	cp doc/README.html doc/FAQ.html doc/cli.html doc/daemon.html doc/modules.html doc/style.css $$HOME/homepage/private/projects/polypaudio
	cp -a doxygen/html/* $$HOME/homepage/private/projects/polypaudio/doxygen
	cp $$HOME/homepage/private/projects/polypaudio/README.html $$HOME/homepage/private/projects/polypaudio/index.html

#distcleancheck:
#	@:

doxygen:
	$(MAKE) -C doxygen doxygen

.PHONY: homepage distcleancheck doxygen
