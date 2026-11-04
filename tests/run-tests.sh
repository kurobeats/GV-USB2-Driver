#!/bin/bash
# Host-side unit tests. No kernel headers or hardware needed.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-cc}
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror"

for t in test_assemble test_regplan test_snd_hw; do
	$CC $CFLAGS -o "/tmp/$t" "$t.c"
	"/tmp/$t"
done
