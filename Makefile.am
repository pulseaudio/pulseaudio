# This file is part of PulseAudio.
#
# PulseAudio is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# PulseAudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with PulseAudio; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
# USA.

ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = \
	bootstrap.sh \
	git-version-gen \
	LICENSE \
	GPL \
	LGPL \
	doxygen/Makefile.am \
	doxygen/Makefile.in \
	doxygen/doxygen.conf.in \
	README \
	todo \
	vala/libpulse.deps \
	vala/libpulse.vapi \
	vala/libpulse-mainloop-glib.deps \
	vala/libpulse-mainloop-glib.vapi

SUBDIRS = src doxygen man po

MAINTAINERCLEANFILES =
noinst_DATA =

vapidir = $(datadir)/vala/vapi
vapi_DATA = \
		vala/libpulse.deps vala/libpulse.vapi \
		vala/libpulse-mainloop-glib.deps vala/libpulse-mainloop-glib.vapi

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libpulse.pc libpulse-simple.pc

if HAVE_GLIB20
pkgconfig_DATA += \
	 libpulse-mainloop-glib.pc
endif

cmakedir = $(libdir)/cmake/PulseAudio
cmake_DATA = PulseAudioConfig.cmake PulseAudioConfigVersion.cmake

homepage: all dist doxygen
	test -d $$HOME/homepage/private
	mkdir -p $$HOME/homepage/private/projects/pulseaudio $$HOME/homepage/private/projects/pulseaudio/doxygen
	cp pulseaudio-@PACKAGE_VERSION@.tar.gz $$HOME/homepage/private/projects/pulseaudio
	cp pulseaudio-@PACKAGE_VERSION@.tar.gz $$HOME/git.fedora/pulseaudio
	cp -a doxygen/html/* $$HOME/homepage/private/projects/pulseaudio/doxygen

doxygen:
	$(MAKE) -C doxygen doxygen

eolspace:
	find \( -name '*.c' -o -name '*.h' -o -name 'Makefile.am' \) -exec perl -i -pe 's/\s+\n$$/\1\n/;' \{\} \;

untabify:
	find \( -name '*.c' -o -name '*.h' \) -exec perl -i -pe 's/\t/        /g;' \{\} \;

fedora-snapshot: dist
	cp $(distdir).tar.gz $$HOME/git.fedora/pulseaudio/$(distdir).tar.gz

dist-hook:
	if test -d .git ; then \
		test -z $$USE_GIT || git pull ; \
		test -f ${distdir}/ChangeLog && chmod u+w ${distdir}/ChangeLog ; \
		( git-changelog.perl || echo "git-changelog.perl failed." ) > ${distdir}/ChangeLog 2>&1 ; \
	fi
	echo $(VERSION) > $(distdir)/.tarball-version
	echo $(VERSION) > $(distdir)/.version

.PHONY: homepage distcleancheck doxygen

# see git-version-gen
BUILT_SOURCES = $(top_srcdir)/.version
$(top_srcdir)/.version:
	echo $(VERSION) > $@-t && mv $@-t $@

DISTCLEANFILES = \
        po/.intltool-merge-cache

DISTCHECK_CONFIGURE_FLAGS = --with-udev-rules-dir="$$dc_install_base/lib/udev/rules.d"
