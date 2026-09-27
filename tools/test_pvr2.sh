#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_bin=$(mktemp /tmp/naomidiag-pvr2.XXXXXX)
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
for model in 0 1 2; do
    cc -std=c11 -Wall -Wextra -Werror -DCFG_BOARD_MODEL="$model" \
        tools/test_pvr2.c -o "$test_bin"
    "$test_bin"
done
cc -std=c11 -Wall -Wextra -Werror tools/test_vram_diag.c -o "$test_bin"
"$test_bin"
cc -std=c11 -Wall -Wextra -Werror tools/test_vram_mapping.c -o "$test_bin"
"$test_bin"
cc -std=c11 -Wall -Wextra -Werror tools/test_vram_scan.c -o "$test_bin"
"$test_bin"
