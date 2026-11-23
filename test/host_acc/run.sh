#!/bin/sh
# Build and run the host test of main/acc_frame.c against real FTDX10 captures (needs only gcc)
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
gcc -std=gnu11 -Wall -Wextra -I"$here/../host_cat/stub" -I"$here/../../main" -o "$out/test_acc" "$here/test_acc.c" "$here/../../main/acc_frame.c"
"$out/test_acc" "$here/captures"
rm -rf "$out"
