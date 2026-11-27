#!/usr/bin/env bash
# Build a test firmware for the owner and pack it with the Windows flashing script.
#   tools/make_test_zip.sh 1.9.64        -> dist/fw-1.9.64.zip
# The version is set only for this build; main/build_info.h is restored afterwards
# (the maintainer bumps it on main after the merge).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
version=${1:?usage: tools/make_test_zip.sh <version, e.g. 1.9.64>}
info="$root/main/build_info.h"
cd "$root"
cp "$info" "$info.orig"
trap 'mv "$info.orig" "$info"' EXIT
sed -i -E "s/(#define FTX1_VERSION \")[^\"]*(\")/\1${version}-yo9va\2/" "$info"
idf.py build
out="$root/dist/fw-$version"
rm -rf "$out" "$out.zip"
mkdir -p "$out"
cp build/network_remote.bin "$out/network_remote_ota.bin"
cp build/ota_data_initial.bin tools/scrie_firmware.bat "$out/"
(cd "$out" && zip -q "../fw-$version.zip" ./*)
echo "Test firmware: $out.zip (flash with option 1, settings are kept)"
