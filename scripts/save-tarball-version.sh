#!/bin/bash

if [ "x${1}" == "x" ]; then
    echo "Package version must be specified to generate tarball version"
    exit 1
fi

echo "${1}" > "$MESON_DIST_ROOT/.tarball-version"
