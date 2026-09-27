#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_bin=$(mktemp /tmp/naomidiag-maple.XXXXXX)
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror tools/test_maple.c -o "$test_bin"
"$test_bin"
