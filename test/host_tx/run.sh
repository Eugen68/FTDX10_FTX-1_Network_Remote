#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
   -I"$root/test/host_cat/stub" -I"$root/main" -I"$root/test/host_tx" \
   "$root/test/host_tx/test_tx.c" -o "$out/test_tx"
"$out/test_tx"
