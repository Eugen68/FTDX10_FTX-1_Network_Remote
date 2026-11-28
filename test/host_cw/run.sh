#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
node "$here/test_qsy.cjs"
node "$here/test_pitch.cjs"
