#!/bin/sh
# Build and run the host test of main/cat.c (needs only gcc)
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
gcc -std=gnu11 -Wall -Wno-unused-variable -Wno-unused-function \
    -I"$here/stub" -I"$here/../../main" -o "$out/test_cat" "$here/test_cat.c"
"$out/test_cat"
rm -rf "$out"
