# $Id$

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

noinst_DATA = README.html cli.html modules.html daemon.html README
EXTRA_DIST = $(noinst_DATA) style.css README.html.in cli.html.in modules.html.in daemon.html.in todo FAQ.html.in

MAINTAINERCLEANFILES = README README.html cli.html modules.html daemon.html FAQ.html
CLEANFILES =

if USE_LYNX
README: README.html
	lynx --dump $^ | sed 's,file://localhost/.*/doc/README.html,README,' > $@

CLEANFILES += README
endif

tidy: README.html cli.html modules.html daemon.html
	tidy -e < README.html
	tidy -e < cli.html
	tidy -e < daemon.html
	tidy -e < modules.html
	tidy -e < FAQ.html

.PHONY: tidy

