#!/bin/bash

# This file is part of PulseAudio.
#
# PulseAudio is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# PulseAudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with PulseAudio; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.

VERSION=1.11

run_versioned() {
    local P
    local V

    V=$(echo "$2" | sed -e 's,\.,,g')

    if [ -e "`which $1$V 2> /dev/null`" ] ; then
        P="$1$V"
    else
	if [ -e "`which $1-$2 2> /dev/null`" ] ; then
	    P="$1-$2"
	else
	    P="$1"
	fi
    fi

    shift 2
    "$P" "$@"
}

set -ex

case $(uname) in
	*Darwin*)
		LIBTOOLIZE="glibtoolize"
		;;
esac

if [ -f .git/hooks/pre-commit.sample -a ! -f .git/hooks/pre-commit ] ; then
    cp -p .git/hooks/pre-commit.sample .git/hooks/pre-commit && \
    chmod +x .git/hooks/pre-commit && \
    echo "Activated pre-commit hook."
fi

if [ -f .tarball-version ]; then
    echo "Marking tarball version as modified."
    echo -n `cat .tarball-version | sed 's/-rebootstrapped$//'`-rebootstrapped >.tarball-version
fi

# We check for this here, because if pkg-config is not found in the
# system, it's likely that the pkg.m4 macro file is also not present,
# which will make PKG_PROG_PKG_CONFIG be undefined and the generated
# configure file faulty.
if ! pkg-config --version &>/dev/null; then
    echo "pkg-config is required to bootstrap this program" &>/dev/null
    exit 1
fi

if type -p colorgcc > /dev/null ; then
   export CC=colorgcc
fi

if [ "x$1" = "xam" ] ; then
    run_versioned automake "$VERSION" -a -c --foreign
    ./config.status
else
    rm -rf autom4te.cache
    rm -f config.cache

    rm -f Makefile.am~ configure.ac~
    # Evil, evil, evil, evil hack
    sed 's/read dummy/\#/' `which gettextize` | bash -s -- --copy --force
    test -f Makefile.am~ && mv Makefile.am~ Makefile.am
    test -f configure.ac~ && mv configure.ac~ configure.ac

    touch config.rpath
    test "x$LIBTOOLIZE" = "x" && LIBTOOLIZE=libtoolize

    intltoolize --copy --force --automake
    "$LIBTOOLIZE" -c --force
    run_versioned aclocal "$VERSION" -I m4
    run_versioned autoconf 2.63 -Wall
    run_versioned autoheader 2.63
    run_versioned automake "$VERSION" --copy --foreign --add-missing

    if test "x$NOCONFIGURE" = "x"; then
        CFLAGS="$CFLAGS -g -O0" ./configure --sysconfdir=/etc --localstatedir=/var --enable-force-preopen "$@"
        make clean
    fi
fi
