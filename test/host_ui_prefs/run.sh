#!/bin/sh
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$here/../../main" \
    "$here/test_ui_prefs.c" "$here/../../main/ui_prefs.c" -o "$out/test_ui_prefs"
"$out/test_ui_prefs"
node "$here/test_step.cjs"
