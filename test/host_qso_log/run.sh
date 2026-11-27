#!/bin/sh
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$here/../../main" \
    "$here/test_qso_adif.c" "$here/../../main/qso_adif.c" -o "$out/test_qso_adif"
"$out/test_qso_adif"
