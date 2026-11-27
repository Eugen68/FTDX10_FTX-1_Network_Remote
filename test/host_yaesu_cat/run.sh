#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -I"$here/../host_cat/stub" -I"$here/../../main" \
    "$here/test_yaesu_cat.c" "$here/../../main/yaesu_cat_protocol.c" -o "$out/test_yaesu_cat"
"$out/test_yaesu_cat"
