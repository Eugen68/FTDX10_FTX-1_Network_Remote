#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -I"$here/../host_cat/stub" -I"$here/../../main" \
    "$here/test_rigctl.c" "$here/../../main/rigctl_protocol.c" -o "$out/test_rigctl"
"$out/test_rigctl"
