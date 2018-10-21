#!/bin/sh

set -e
set -u

DATADIR="$1"

echo "Compiling GSettings schemas..."
glib-compile-schemas "$DATADIR/glib-2.0/schemas"
