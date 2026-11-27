#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -I"$root/main" -I"$root/components/ft8_lib" \
   "$root/test/host_ft8/test_ft8.c" "$root/main/ft8_wave.c" \
   "$root/components/ft8_lib"/ft8/{constants,crc,encode,message,text}.c -lm -o "$out/test_ft8"
"$out/test_ft8"
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable \
   -I"$root/test/host_ft8" -I"$root/test/host_cat/stub" -I"$root/test/host_tx" -I"$root/main" -I"$root/components/ft8_lib" \
   "$root/test/host_ft8/test_schedule.c" "$root/main/ft8_wave.c" \
   "$root/components/ft8_lib"/ft8/{constants,crc,encode,message,text}.c -lm -o "$out/test_schedule"
"$out/test_schedule"
