# $Id$
#
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

pulseconfdir=$(sysconfdir)/pulse

if BUILD_MANPAGES

man_MANS = \
	pulseaudio.1

noinst_DATA = \
	pulseaudio.1.xml

CLEANFILES = \
	$(noinst_DATA)

pulseaudio.1.xml: pulseaudio.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

if USE_XMLTOMAN

CLEANFILES += \
	$(man_MANS)

pulseaudio.1: pulseaudio.1.xml Makefile
	xmltoman $< > $@

xmllint: $(noinst_DATA)
	for f in $(noinst_DATA) ; do \
			xmllint --noout --valid "$$f" || exit 1 ; \
	done

endif

endif

EXTRA_DIST = \
	$(man_MANS) \
	pulseaudio.1.xml.in
	xmltoman.css \
	xmltoman.xsl \
	xmltoman.dtd
