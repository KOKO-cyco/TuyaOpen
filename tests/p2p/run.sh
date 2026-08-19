#!/bin/sh
# Host tests for the P2P transport. No board and no cross toolchain needed:
# the congestion control is plain integer arithmetic over the ikcpcb, so it can
# be exercised natively and much faster than over a live link.
#
#   ./tests/p2p/run.sh
set -e

here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../src/tuya_p2p/base_ice
out=${TMPDIR:-/tmp}/tuya_p2p_tests
mkdir -p "$out"

cc=${CC:-gcc}
$cc -O1 -g -Wall -I"$src/src" -I"$src/include" \
    -o "$out/test_ikcp_cong" \
    "$here/test_ikcp_cong.c" "$here/stubs.c" \
    "$src/src/ikcp.c" "$src/src/ikcp_cong.c" "$src/src/ikcp_pacing.c"

"$out/test_ikcp_cong"
