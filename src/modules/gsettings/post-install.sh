#!/bin/sh

set -e
set -u

DATADIR="$1"

# Package managers set this so we don't need to run
if [ "${DESTDIR:-}" ]; then exit 0; fi

echo "Compiling GSettings schemas..."
glib-compile-schemas "$DATADIR/glib-2.0/schemas"
