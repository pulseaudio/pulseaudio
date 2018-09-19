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
# along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.

ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = \
	bootstrap.sh \
	coverity/model.c \
	git-version-gen \
	LICENSE \
	pulseaudio.supp \
	GPL \
	LGPL \
	doxygen/Makefile.am \
	doxygen/Makefile.in \
	doxygen/doxygen.conf.in \
	PROTOCOL \
	README \
	CODE_OF_CONDUCT.md \
	scripts/benchmark_memory_usage.sh \
	scripts/plot_memory_usage.gp \
	scripts/benchmarks/README \
	todo \
	.gitignore \
	doxygen/.gitignore \
	m4/.gitignore \
	man/.gitignore \
	po/.gitignore \
	scripts/benchmarks/.gitignore \
	src/.gitignore \
	src/daemon/.gitignore \
	src/pulse/.gitignore

SUBDIRS = src doxygen man po

MAINTAINERCLEANFILES =
noinst_DATA =

vapidir = $(datadir)/vala/vapi
dist_vapi_DATA = \
		vala/libpulse.deps vala/libpulse.vapi \
		vala/libpulse-mainloop-glib.deps vala/libpulse-mainloop-glib.vapi \
		vala/libpulse-simple.deps vala/libpulse-simple.vapi

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libpulse.pc libpulse-simple.pc

if HAVE_GLIB20
pkgconfig_DATA += \
	 libpulse-mainloop-glib.pc
endif

cmakedir = $(libdir)/cmake/PulseAudio
cmake_DATA = PulseAudioConfig.cmake PulseAudioConfigVersion.cmake

bashcompletiondir=@bashcompletiondir@
dist_bashcompletion_DATA = shell-completion/bash/pulseaudio

install-bashcompletion-aliases:
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio pactl
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio pacmd
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio pasuspender
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio padsp
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio pacat
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio paplay
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio parec
	cd $(DESTDIR)$(bashcompletiondir) && $(LN_S) -f pulseaudio parecord

install-data-hook: install-bashcompletion-aliases

zshcompletiondir=@zshcompletiondir@
dist_zshcompletion_DATA = shell-completion/zsh/_pulseaudio

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
	echo $(VERSION) > $(distdir)/.tarball-version
	echo $(VERSION) > $(distdir)/.version

check-daemon:
	$(MAKE) -C src check-daemon

.PHONY: homepage distcleancheck doxygen

# see git-version-gen
BUILT_SOURCES = $(top_srcdir)/.version
$(top_srcdir)/.version:
	echo $(VERSION) > $@-t && mv $@-t $@

DISTCHECK_CONFIGURE_FLAGS = --with-udev-rules-dir="$$dc_install_base/lib/udev/rules.d" --with-systemduserunitdir="$$dc_install_base/lib/systemd/user" --with-bash-completion-dir="$$dc_install_base/share/bash-completion/completions"
