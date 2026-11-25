#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
   -I"$root/test/host_cat/stub" -I"$root/main" -I"$root/test/host_tx" \
   "$root/test/host_tx/test_tx.c" "$root/main/tx_swr.c" -o "$out/test_tx"
"$out/test_tx"
cc -std=gnu11 -Wall -Wextra -Werror -I"$root/test/host_cat/stub" -I"$root/main" \
   "$root/test/host_tx/test_tx_signal.c" "$root/main/tx_swr.c" "$root/main/tx_tone.c" -lm -o "$out/test_tx_signal"
"$out/test_tx_signal"
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
   -I"$root/test/host_cat/stub" -I"$root/main" -I"$root/test/host_tx" \
   "$root/test/host_tx/test_tx_voice.c" "$root/main/tx_voice.c" -lm -o "$out/test_tx_voice"
"$out/test_tx_voice"
