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
	pulseaudio.1 \
	esdcompat.1 \
	pax11publish.1 \
	paplay.1 \
	pacat.1 \
	pacmd.1 \
	pactl.1 \
	pasuspender.1 \
	padsp.1 \
	pabrowse.1 \
	pulse-daemon.conf.5 \
	pulse-client.conf.5 \
	default.pa.5

noinst_DATA = \
	pulseaudio.1.xml \
	esdcompat.1.xml \
	pax11publish.1.xml \
	paplay.1.xml \
	pacat.1.xml \
	pacmd.1.xml \
	pactl.1.xml \
	pasuspender.1.xml \
	padsp.1.xml \
	pabrowse.1.xml \
	pulse-daemon.conf.5.xml \
	pulse-client.conf.5.xml \
	default.pa.5.xml

CLEANFILES = \
	$(noinst_DATA)

pulseaudio.1.xml: pulseaudio.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

esdcompat.1.xml: esdcompat.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pax11publish.1.xml: pax11publish.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

paplay.1.xml: paplay.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pacat.1.xml: pacat.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pacmd.1.xml: pacmd.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pactl.1.xml: pactl.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pasuspender.1.xml: pasuspender.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

padsp.1.xml: padsp.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pabrowse.1.xml: pabrowse.1.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pulse-daemon.conf.5.xml: pulse-daemon.conf.5.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

pulse-client.conf.5.xml: pulse-client.conf.5.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

default.pa.5.xml: default.pa.5.xml.in Makefile
	sed -e 's,@pulseconfdir\@,$(pulseconfdir),g' \
	    -e 's,@PACKAGE_BUGREPORT\@,$(PACKAGE_BUGREPORT),g' \
            -e 's,@PACKAGE_URL\@,$(PACKAGE_URL),g' $< > $@

if USE_XMLTOMAN

CLEANFILES += \
	$(man_MANS)

pulseaudio.1: pulseaudio.1.xml Makefile
	xmltoman $< > $@

esdcompat.1: esdcompat.1.xml Makefile
	xmltoman $< > $@

pax11publish.1: pax11publish.1.xml Makefile
	xmltoman $< > $@

paplay.1: paplay.1.xml Makefile
	xmltoman $< > $@

pacat.1: pacat.1.xml Makefile
	xmltoman $< > $@

pacmd.1: pacmd.1.xml Makefile
	xmltoman $< > $@

pactl.1: pactl.1.xml Makefile
	xmltoman $< > $@

pasuspender.1: pasuspender.1.xml Makefile
	xmltoman $< > $@

padsp.1: padsp.1.xml Makefile
	xmltoman $< > $@

pabrowse.1: pabrowse.1.xml Makefile
	xmltoman $< > $@

pulse-daemon.conf.5: pulse-daemon.conf.5.xml Makefile
	xmltoman $< > $@

pulse-client.conf.5: pulse-client.conf.5.xml Makefile
	xmltoman $< > $@

default.pa.5: default.pa.5.xml Makefile
	xmltoman $< > $@

xmllint: $(noinst_DATA)
	for f in $(noinst_DATA) ; do \
			xmllint --noout --valid "$$f" || exit 1 ; \
	done

endif

endif

EXTRA_DIST = \
	$(man_MANS) \
	pulseaudio.1.xml.in \
	esdcompat.1.xml.in \
	pax11publish.1.xml.in \
	paplay.1.xml.in \
	pacat.1.xml.in \
	pacmd.1.xml.in \
	pactl.1.xml.in \
	pasuspender.1.xml.in \
	padsp.1.xml.in \
	pabrowse.1.xml.in \
	pulse-daemon.conf.5.xml.in \
	pulse-client.conf.5.xml.in \
	default.pa.5.xml.in \
	xmltoman.css \
	xmltoman.xsl \
	xmltoman.dtd
