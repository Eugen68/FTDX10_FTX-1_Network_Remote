#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# Compile the production parser and handler against the HTTP server stubs below.
sed -n '/^static const char \*tx_reason(/,/^static unsigned lwip_sockets_used(/ {
    /^static unsigned lwip_sockets_used(/d
    p
}' "$root/main/web_server.c" > "$out/tx_ws_handler.inc"
sed -n '/^static void skip_ws(/,/^static bool token_to_uint(/ {
    /^static bool token_to_uint(/d
    p
}' "$root/main/web_server.c" >> "$out/tx_ws_handler.inc"
cc -std=gnu11 -Wall -Wextra -Werror \
   -I"$out" -I"$root/test/host_cat/stub" -I"$root/main" \
   "$root/test/host_tx_ws/test_tx_ws.c" -o "$out/test_tx_ws"
"$out/test_tx_ws"
