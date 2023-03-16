#!/bin/bash

# Generator for linker version script.
# We use the same linker version script for all public .so files
#
# generate-map-file.sh where-is/map-file public_interface_1.h public_interface_2.h ... public_interface_N.h
#

CTAGS_IDENTIFIER_LIST="PA_GCC_MALLOC,PA_GCC_ALLOC_SIZE2,PA_GCC_ALLOC_SIZE,PA_GCC_PURE,PA_GCC_CONST,PA_GCC_DEPRECATED,PA_GCC_PRINTF_ATTR"

print_map_file() {
    echo "PULSE_0 {"
    echo "global:"
    ctags -I ${CTAGS_IDENTIFIER_LIST} -f - --c-kinds=p "$@" | awk '/^pa_/ { print $1 ";" }' | sort
    echo "local:"
    echo "*;"
    echo "};"
}

print_def_file() {
    echo "EXPORTS"
    ctags -I ${CTAGS_IDENTIFIER_LIST} -f - --c-kinds=p "$@" | awk '/^pa_/ && !/(^pa_glib_|^pa_simple_)/ { print $1 }' | sort
}

MAP_FILE=$1
DEF_FILE=$2
shift 2

cd "${MESON_SOURCE_ROOT}/${MESON_SUBDIR}" && print_map_file "$@" > ${MAP_FILE}
cd "${MESON_SOURCE_ROOT}/${MESON_SUBDIR}" && print_def_file "$@" > ${DEF_FILE}
