#!/bin/sh

set -e
set -u

PATH=${MESON_BUILD_ROOT}/src/daemon:${MESON_BUILD_ROOT}/src/tests:${MESON_BUILD_ROOT}/src/utils:${PATH}
export PATH

${MESON_SOURCE_ROOT}/src/tests/test-daemon.sh $@
