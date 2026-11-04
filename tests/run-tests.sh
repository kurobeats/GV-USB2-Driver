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

# core/i2c lane: real driver sources compiled against fake kernel headers in
# utk/ and a mock USB device (see test_core.c header comment). -Wno-unused-
# parameter: the fake struct signatures pass params the kernel build tolerates
# (kernel does not enable -Wextra) but host -Werror here would reject.
$CC $CFLAGS -Wno-unused-parameter -Iutk -o /tmp/test_core test_core.c \
	../gvusb2-core.c
/tmp/test_core